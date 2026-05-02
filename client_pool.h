/**
 * client_pool.h — Slab allocator for ClientCtx objects
 *
 * See client_pool.c for a full description of the allocator design,
 * the free-list layout, and the connection-close protocol.
 *
 * Quick reference
 * ───────────────
 *  client_pool_init()     — call once at startup
 *  client_pool_alloc()    — O(1) slot acquisition, call on new connection
 *  client_pool_release()  — O(1) slot return, call after BOTH uv handles closed
 *  client_pool_destroy()  — call at shutdown to free all chunks
 */

#ifndef CLIENT_POOL_H
#define CLIENT_POOL_H

#include "config.h"
#include <uv.h>
#include <stdbool.h>

/** Number of ClientCtx slots per MemoryChunk slab. */
#define CHUNK_SIZE 64

/* Forward declaration so ClientCtx can reference its owning chunk. */
struct MemoryChunk;

/**
 * Per-connection context.
 *
 * Lifetime
 * ────────
 * Allocated by client_pool_alloc() when a TCP connection is accepted.
 * Released by client_pool_release() only after BOTH libuv handles (tcp
 * and timer) have fired their uv_close callback — tracked by pending_closes
 * counting down from 2 to 0 inside on_close() in server_functions.c.
 *
 * Handle initialisation
 * ─────────────────────
 * tcp and timer are embedded directly (no extra heap allocation per slot).
 * Their .data field always points back to the owning ClientCtx so every
 * libuv callback can recover context with a simple cast:
 *
 *     ClientCtx *ctx = (ClientCtx *)handle->data;
 *
 * The handles are NOT initialised by the pool; uv_tcp_init() and
 * uv_timer_init() must be called by setup_client() in server_functions.c.
 *
 * Fields
 * ──────
 *  tcp            — libuv TCP handle for the accepted connection.
 *  timer          — libuv timer handle for the keepalive timeout.
 *  next / prev    — doubly-linked list of all live contexts, used for
 *                   O(n) traversal during graceful shutdown.
 *  parentChunk    — back-pointer to the owning MemoryChunk slab;
 *                   lets client_pool_release() locate the chunk in O(1).
 *  closing        — set to true by close_client() before the first
 *                   uv_close() call; prevents double-close races.
 *  pending_closes — initialised to 2 by close_client(); decremented once
 *                   per uv_close callback by on_close(); the pool slot is
 *                   released when it reaches 0.
 *  totalRead      — number of valid bytes currently in buffer.
 *  buffer         — accumulates raw HTTP request bytes as libuv delivers
 *                   them; always NUL-terminated at buffer[totalRead].
 */
typedef struct ClientCtx {
    uv_tcp_t            tcp;
    uv_timer_t          timer;
    struct ClientCtx   *next;
    struct ClientCtx   *prev;
    struct MemoryChunk *parentChunk;
    bool                closing;
    int                 pending_closes;
    size_t              totalRead;
    char                buffer[BUFFER_SIZE];
} ClientCtx;

/* ── Pool lifecycle ──────────────────────────────────────────────────── */

/**
 * Allocates the first MemoryChunk slab.
 * Must be called once before any client_pool_alloc() or
 * client_pool_release() call.
 * Returns 0 on success, -1 on allocation failure.
 */
int client_pool_init(void);

/**
 * Frees every MemoryChunk unconditionally.
 * Must only be called at shutdown, after every connection has been fully
 * closed (i.e. all on_close() callbacks have fired).
 */
void client_pool_destroy(void);

/* ── Per-connection API ───────────────────────────────────────────────── */

/**
 * Returns a ClientCtx slot from the pool (O(1) amortised).
 *
 * The slot has its scalar fields zeroed; buffer is cleared.
 * parentChunk is preserved across reuse — do not overwrite it.
 *
 * The caller MUST call uv_tcp_init() and uv_timer_init() before using
 * the embedded handles, then set handle.data = ctx on both.
 *
 * Returns NULL if a new chunk slab could not be allocated.
 */
ClientCtx *client_pool_alloc(void);

/**
 * Returns a ClientCtx slot to its owning chunk's free-list (O(1)).
 *
 * MUST be called only after both libuv handles are fully closed
 * (pending_closes == 0).  Calling this while a handle is still being
 * closed by libuv is undefined behaviour.
 *
 * If the owning chunk becomes empty AND there is at least one other chunk,
 * the chunk is freed to return memory to the OS.
 */
void client_pool_release(ClientCtx *ctx);

#endif /* CLIENT_POOL_H */