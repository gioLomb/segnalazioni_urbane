/**
 * client_pool.c — Slab allocator for ClientCtx objects
 *
 * Problem solved
 * ──────────────
 * A high-throughput server that accepts thousands of short-lived TCP
 * connections would suffer from severe heap fragmentation if each
 * connection called malloc(sizeof(ClientCtx)) on arrival and free() on
 * departure.  This pool avoids that by pre-allocating fixed-size slabs
 * (MemoryChunk) and serving slots from them in O(1).
 *
 * Memory layout
 * ─────────────
 * Each MemoryChunk holds exactly CHUNK_SIZE ClientCtx objects in a plain
 * embedded array (contiguous, cache-friendly):
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ MemoryChunk                                          │
 *   │   clients[0] … clients[CHUNK_SIZE-1]   (embedded)   │
 *   │   localFreeList  ──► first free slot in this chunk   │
 *   │   usedCount                                          │
 *   │   next / prev    ──► neighbouring chunks             │
 *   └──────────────────────────────────────────────────────┘
 *
 * Free-list (within a chunk)
 * ──────────────────────────
 * Free slots are linked through ClientCtx.next.  Allocating a slot pops
 * the head of localFreeList (O(1)).  Releasing a slot pushes it back onto
 * the head (O(1)).
 *
 * Back-pointer (ClientCtx → MemoryChunk)
 * ───────────────────────────────────────
 * Every slot carries a parentChunk pointer so client_pool_release() can
 * locate the owning chunk in O(1) without a global search.
 *
 * Chunk list
 * ──────────
 * Chunks form a doubly-linked list (chunksHead) so a fully-freed chunk can
 * unlink and free itself in O(1).  The last remaining chunk is never freed
 * to avoid re-allocating at the next connection burst.
 *
 * libuv handle initialisation
 * ───────────────────────────
 * client_pool_alloc() does NOT call uv_tcp_init or uv_timer_init.
 * The caller (setup_client in server_functions.c) must do that before use,
 * because libuv handles require the event-loop pointer which is only
 * available inside the loop.
 *
 * Connection close protocol
 * ─────────────────────────
 * A ClientCtx slot MUST NOT be released until both its libuv handles
 * (tcp + timer) have fired their uv_close callback.  This is enforced by
 * on_close() in server_functions.c via ClientCtx.pending_closes:
 *   - pending_closes is set to 2 when close_client() issues both uv_close calls.
 *   - on_close() decrements it; client_pool_release() is called only when it hits 0.
 */

#include "client_pool.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal type ───────────────────────────────────────────────────── */

/*
 * MemoryChunk — one fixed-size slab of CHUNK_SIZE ClientCtx slots.
 *
 * Fields:
 *   clients       — the slots themselves (embedded, no per-slot heap).
 *   localFreeList — singly-linked list of free slots within this chunk,
 *                   threaded through ClientCtx.next.
 *   usedCount     — number of slots currently in use; when it drops to 0
 *                   the chunk may be freed (unless it is the last one).
 *   next / prev   — doubly-linked list of all chunks (chunksHead is the head).
 */
typedef struct MemoryChunk {
    ClientCtx  clients[CHUNK_SIZE];
    ClientCtx *localFreeList;
    int        usedCount;
    struct MemoryChunk *next;
    struct MemoryChunk *prev;
} MemoryChunk;

/* Head of the doubly-linked list of all live chunks. */
static MemoryChunk *chunksHead = NULL;

/* ── Internal helpers ────────────────────────────────────────────────── */

/**
 * Allocates and initialises a new MemoryChunk.
 *
 * Links all CHUNK_SIZE slots into a free-list through ClientCtx.next and
 * sets parentChunk on every slot so client_pool_release() can find the
 * owning chunk in O(1).
 */
static MemoryChunk *create_chunk(void) {
    MemoryChunk *c = calloc(1, sizeof(MemoryChunk));
    if (!c) return NULL;

    /* Thread slots into the free-list: slots[0] → slots[1] → … → NULL */
    for (int i = 0; i < CHUNK_SIZE - 1; i++) {
        c->clients[i].next        = &c->clients[i + 1];
        c->clients[i].parentChunk = c;
    }
    c->clients[CHUNK_SIZE - 1].next        = NULL;
    c->clients[CHUNK_SIZE - 1].parentChunk = c;

    c->localFreeList = &c->clients[0];
    c->usedCount     = 0;
    return c;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * Allocates the first MemoryChunk.
 * Must be called once at startup before any client_pool_alloc() call.
 * Returns 0 on success, -1 on allocation failure.
 */
int client_pool_init(void) {
    chunksHead = create_chunk();
    return chunksHead ? 0 : -1;
}

/**
 * Returns a ClientCtx slot from the pool (O(1) amortised).
 *
 * Search order:
 *   1. Walk the chunk list for a chunk with a free slot (usedCount < CHUNK_SIZE).
 *   2. If none found, allocate a new chunk and prepend it to the list.
 *
 * The returned slot has its scalar fields reset to zero/false.
 * The tcp and timer handles are NOT initialised here; the caller must call
 * uv_tcp_init() and uv_timer_init() with the event loop before using them.
 *
 * Returns NULL if a new chunk could not be allocated.
 */
ClientCtx *client_pool_alloc(void) {
    /* Find the first chunk with at least one free slot. */
    MemoryChunk *curr = chunksHead;
    while (curr && curr->usedCount == CHUNK_SIZE)
        curr = curr->next;

    /* No space anywhere — allocate a new chunk and prepend it. */
    if (!curr) {
        curr = create_chunk();
        if (!curr) return NULL;
        curr->next = chunksHead;
        if (chunksHead) chunksHead->prev = curr;
        chunksHead = curr;
    }

    /* Pop a slot from this chunk's free-list. */
    ClientCtx *ctx      = curr->localFreeList;
    curr->localFreeList = ctx->next;
    curr->usedCount++;

    /* Reset all fields except parentChunk (which must survive reuse). */
    memset(ctx->buffer, 0, BUFFER_SIZE);
    ctx->closing        = false;
    ctx->pending_closes = 0;
    ctx->totalRead      = 0;
    ctx->next           = NULL;
    ctx->prev           = NULL;

    return ctx;
}

/**
 * Returns a ClientCtx slot to its owning chunk's free-list (O(1)).
 *
 * Must only be called after both libuv handles (tcp and timer) have been
 * fully closed (i.e. their uv_close callbacks have fired and
 * pending_closes has reached 0 — enforced by on_close in server_functions.c).
 *
 * Chunk shrinking: if the chunk becomes empty and it is not the last chunk,
 * it is unlinked and freed to return memory to the OS.
 */
void client_pool_release(ClientCtx *ctx) {
    if (!ctx) return;
    MemoryChunk *c = (MemoryChunk *)ctx->parentChunk;

    /* Push the slot back onto this chunk's free-list. */
    ctx->next        = c->localFreeList;
    c->localFreeList = ctx;
    c->usedCount--;

    /*
     * Free the chunk if it is now empty and there is at least one other
     * chunk remaining (we always keep the last chunk to avoid thrashing).
     */
    if (c->usedCount == 0 && (c->next || c->prev)) {
        if (c->prev) c->prev->next = c->next;
        if (c->next) c->next->prev = c->prev;
        if (c == chunksHead) chunksHead = c->next;
        free(c);
    }
}

/**
 * Frees every MemoryChunk unconditionally.
 * Call at server shutdown after all connections have been closed.
 * Any outstanding ClientCtx pointers become dangling after this call.
 */
void client_pool_destroy(void) {
    MemoryChunk *curr = chunksHead;
    while (curr) {
        MemoryChunk *next = curr->next;
        free(curr);
        curr = next;
    }
    chunksHead = NULL;
}