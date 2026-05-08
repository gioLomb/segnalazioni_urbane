/**
 * client_pool.c — Slab allocator for ClientCtx objects
 *
 * Memory layout, free-list design, and close protocol are unchanged from
 * the original implementation.  This revision adds the active connection
 * list so that server_functions.c no longer manages it manually.
 *
 * Two separate lists live in this file:
 *
 *   Chunk list  (g_chunks_head)
 *     Doubly-linked list of MemoryChunk slabs for memory management.
 *     Used only by alloc/release/destroy.  Private.
 *
 *   Active list (g_active_head)
 *     Doubly-linked list of ClientCtx slots currently in use.
 *     Threaded through ClientCtx.next / ClientCtx.prev.
 *     Managed by client_pool_link() / client_pool_unlink().
 *     Read by client_pool_active_count() and client_pool_close_all().
 *
 * Note on dual use of ClientCtx.next
 * ───────────────────────────────────
 * When a slot is FREE, .next is used by the chunk's internal free-list
 * (singly-linked).  When a slot is IN USE, .next and .prev form the
 * active doubly-linked list.  client_pool_alloc() resets both to NULL
 * before handing the slot to the caller, so there is no aliasing risk.
 */

#include "client_pool.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal types ──────────────────────────────────────────────────── */

typedef struct MemoryChunk {
    ClientCtx  clients[CHUNK_SIZE];
    ClientCtx *localFreeList;
    int        usedCount;
    struct MemoryChunk *next;
    struct MemoryChunk *prev;
} MemoryChunk;

/* ── Module state ────────────────────────────────────────────────────── */

static MemoryChunk *g_chunks_head = NULL;   /* slab allocator chain      */
static ClientCtx   *g_active_head = NULL;   /* live connections chain     */
static int          g_active_count = 0;     /* length of active chain     */

/* ── Chunk helpers ───────────────────────────────────────────────────── */

static MemoryChunk *create_chunk(void) {
    MemoryChunk *c = calloc(1, sizeof(MemoryChunk));
    if (!c) return NULL;

    for (int i = 0; i < CHUNK_SIZE - 1; i++) {
        c->clients[i].next        = &c->clients[i + 1];
        c->clients[i].parentChunk = c;
    }
    c->clients[CHUNK_SIZE - 1].next        = NULL;
    c->clients[CHUNK_SIZE - 1].parentChunk = c;
    c->localFreeList = &c->clients[0];
    return c;
}

/* ── Pool lifecycle ──────────────────────────────────────────────────── */

int client_pool_init(void) {
    g_chunks_head = create_chunk();
    return g_chunks_head ? 0 : -1;
}

void client_pool_destroy(void) {
    MemoryChunk *cur = g_chunks_head;
    while (cur) {
        MemoryChunk *next = cur->next;
        free(cur);
        cur = next;
    }
    g_chunks_head = NULL;
    g_active_head = NULL;
    g_active_count = 0;
}

/* ── Per-connection alloc / release ─────────────────────────────────── */

ClientCtx *client_pool_alloc(void) {
    /* Find the first chunk with a free slot. */
    MemoryChunk *chunk = g_chunks_head;
    while (chunk && chunk->usedCount == CHUNK_SIZE)
        chunk = chunk->next;

    if (!chunk) {
        chunk = create_chunk();
        if (!chunk) return NULL;
        chunk->next = g_chunks_head;
        if (g_chunks_head) g_chunks_head->prev = chunk;
        g_chunks_head = chunk;
    }

    ClientCtx *ctx      = chunk->localFreeList;
    chunk->localFreeList = ctx->next;
    chunk->usedCount++;

    /* Reset all fields except parentChunk. */
    memset(ctx->buffer, 0, BUFFER_SIZE);
    ctx->closing        = false;
    ctx->pending_closes = 0;
    ctx->totalRead      = 0;
    ctx->next           = NULL;
    ctx->prev           = NULL;
    return ctx;
}

void client_pool_release(ClientCtx *ctx) {
    if (!ctx) return;
    MemoryChunk *c = (MemoryChunk *)ctx->parentChunk;

    ctx->next        = c->localFreeList;
    c->localFreeList = ctx;
    c->usedCount--;

    /* Free empty chunk unless it is the last one. */
    if (c->usedCount == 0 && (c->next || c->prev)) {
        if (c->prev) c->prev->next = c->next;
        if (c->next) c->next->prev = c->prev;
        if (c == g_chunks_head) g_chunks_head = c->next;
        free(c);
    }
}

/* ── Active connection list ──────────────────────────────────────────── */

void client_pool_link(ClientCtx *ctx) {
    ctx->next = g_active_head;
    ctx->prev = NULL;
    if (g_active_head) g_active_head->prev = ctx;
    g_active_head = ctx;
    g_active_count++;
}

void client_pool_unlink(ClientCtx *ctx) {
    if (ctx->prev) ctx->prev->next = ctx->next;
    else           g_active_head   = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;
    ctx->next = ctx->prev = NULL;
    g_active_count--;
}

int client_pool_active_count(void) {
    return g_active_count;
}

void client_pool_close_all(void (*fn)(ClientCtx *ctx)) {
    ClientCtx *cur = g_active_head;
    while (cur) {
        ClientCtx *next = cur->next;   /* save before fn() may modify the slot */
        fn(cur);
        cur = next;
    }
}