/**
 * client_pool.h — Slab allocator for ClientCtx objects
 *
 * See client_pool.c for a full description of the allocator design,
 * the free-list layout, and the connection-close protocol.
 *
 * Quick reference
 * ───────────────
 *  client_pool_init()         — call once at startup
 *  client_pool_alloc()        — O(1) slot acquisition, call on new connection
 *  client_pool_release()      — O(1) slot return, call after BOTH uv handles closed
 *  client_pool_destroy()      — call at shutdown to free all chunks
 *
 *  client_pool_link()         — register a slot as an active connection
 *  client_pool_unlink()       — deregister a slot from the active list
 *  client_pool_active_count() — number of currently active connections
 *  client_pool_close_all()    — call a function on every active connection
 *                               (used for graceful shutdown)
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
 * Active list (next / prev)
 * ─────────────────────────
 * When a slot is in use, next/prev form a doubly-linked list of all live
 * connections, owned by client_pool.c.  server_functions.c never touches
 * these pointers directly; it uses client_pool_link() / client_pool_unlink()
 * and client_pool_close_all() instead.
 *
 * When a slot is FREE (between release and the next alloc), next is reused
 * by the pool's internal free-list (singly-linked, no prev needed).
 *
 * Fields
 * ──────
 *  tcp            — libuv TCP handle for the accepted connection.
 *  timer          — libuv timer handle for the keepalive timeout.
 *  next / prev    — active connection list (managed by client_pool).
 *  parentChunk    — back-pointer to the owning MemoryChunk slab.
 *  closing        — set to true by close_client() before the first uv_close().
 *  pending_closes — decremented by on_close(); pool slot released when 0.
 *  totalRead      — valid bytes currently in buffer.
 *  buffer         — raw HTTP request accumulation buffer (NUL-terminated).
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

int  client_pool_init   (void);
void client_pool_destroy(void);

/* ── Per-connection alloc / release ─────────────────────────────────── */

ClientCtx *client_pool_alloc  (void);
void       client_pool_release(ClientCtx *ctx);

/* ── Active connection list ──────────────────────────────────────────── */

/**
 * Registers ctx as an active connection (prepends to the active list).
 * Must be called by server_functions.c after a successful uv_accept().
 */
void client_pool_link(ClientCtx *ctx);

/**
 * Removes ctx from the active list.
 * Must be called by server_functions.c inside on_close() when
 * pending_closes reaches 0 and before client_pool_release().
 */
void client_pool_unlink(ClientCtx *ctx);

/**
 * Returns the number of currently active connections.
 */
int client_pool_active_count(void);

/**
 * Calls fn(ctx) on every active connection in list order.
 * Saves ctx->next before each call so fn() may modify the list safely
 * (e.g. by calling close_client which sets ctx->closing but does NOT
 * unlink immediately — unlinking happens later in on_close).
 *
 * Used by server_loop() for graceful shutdown:
 *   client_pool_close_all(close_client);
 */
void client_pool_close_all(void (*fn)(ClientCtx *ctx));

#endif /* CLIENT_POOL_H */