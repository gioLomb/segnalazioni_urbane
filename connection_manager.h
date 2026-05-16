/**
 * @file connection_manager.h
 * @brief Registry and memory manager for active TCP connections.
 *
 * Manages the lifecycle of ClientCtx objects through two mechanisms:
 *   1. Memory: a SlabAllocator provides O(1) alloc/release of contexts.
 *   2. Tracking: a global doubly-linked list of live connections supports
 *      monitoring and ordered graceful shutdown.
 */

#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include "config.h"
#include <uv.h>
#include <stdbool.h>

/**
 * @brief State for a single active client connection.
 */
typedef struct __attribute__((aligned(64))) ClientCtx {
    uv_tcp_t     handle;              /**< libuv TCP handle for this connection  */
    uv_timer_t   timer;               /**< Inactivity keepalive timer            */
    unsigned int closing       : 1;   /**< Set when shutdown has been initiated  */
    unsigned int pendingCloses : 2;   /**< libuv close callbacks still pending (timer and event)   */
    struct ClientCtx *next;           /**< Next node in the active list          */
    struct ClientCtx *prev;           /**< Previous node in the active list      */
    size_t       totalRead;           /**< Cumulative bytes read this session    */
    char        *buffer;              /**< Receive buffer; NULL until first read */
} ClientCtx;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Initialises the internal slab pool and resets global state.
 * @post The slab allocator is ready; conn_manager_alloc() can be called.
 * @return 0 on success, -1 on allocation failure.
 */
int conn_manager_init(void);

/**
 * @brief Releases all slab memory and resets the manager to an empty state.
 * @pre All connections should be closed before calling this.
 * @post No further alloc/release calls are valid until re-initialisation.
 */
void conn_manager_destroy(void);

/* ── Per-connection alloc / release ─────────────────────────────────── */

/**
 * @brief Allocates a zeroed ClientCtx from the slab pool.
 * @pre conn_manager_init() has been called.
 * @post Returns a valid pointer with all fields reset, or NULL on OOM.
 * @return Pointer to a new ClientCtx, or NULL if memory is exhausted.
 */
ClientCtx *conn_manager_alloc(void);

/**
 * @brief Returns a ClientCtx to the slab pool and frees its buffer.
 * @pre ctx was allocated via conn_manager_alloc().
 * @pre ctx->pendingCloses == 0 (all libuv handles have been closed).
 * @post ctx->buffer is freed; the slab slot is available for reuse.
 * @param ctx Pointer to the context to release.
 */
void conn_manager_release(ClientCtx *ctx);

/* ── Active list ─────────────────────────────────────────────────────── */

/**
 * @brief Inserts a connection at the head of the active tracking list.
 * @pre ctx is allocated and not already linked.
 * @post ctx is the new head of the global list; active count increments.
 * @param ctx Pointer to the context to link.
 */
void conn_manager_link(ClientCtx *ctx);

/**
 * @brief Removes a connection from the active tracking list.
 * @pre ctx is currently linked in the list.
 * @post ctx is unlinked; active count decrements.
 * @param ctx Pointer to the context to unlink.
 */
void conn_manager_unlink(ClientCtx *ctx);

/**
 * @brief Returns the number of currently active connections.
 * @return Count of linked ClientCtx objects.
 */
int conn_manager_active_count(void);

/**
 * @brief Iterates over all active connections and applies fn to each.
 *
 * The next pointer is cached before fn() is called, so fn may safely
 * unlink or close the current node (e.g. during graceful shutdown).
 *
 * @param fn Callback applied to every active ClientCtx.
 */
void conn_manager_close_all(void (*fn)(ClientCtx *ctx));

#endif /* CONNECTION_MANAGER_H */
