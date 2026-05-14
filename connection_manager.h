/**
 * @file connection_manager.h
 * @brief Registry and memory manager for active TCP connections.
 *
 * This module manages the lifecycle of ClientCtx structures. It handles:
 * 1. Memory Allocation: Uses an underlying SlabAllocator for efficient O(1) 
 * allocation of connection contexts.
 * 2. Connection Tracking: Maintains a global doubly-linked list of active 
 * connections for monitoring and graceful shutdowns.
 */

#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include "config.h"
#include <uv.h>
#include <stdbool.h>

/**
 * @brief Context for a single client connection.
 *
 * This structure tracks the state, network handles, and buffers for an active session.
 */
typedef struct __attribute__((aligned(64))) ClientCtx {
    uv_tcp_t          handle;         /**< libuv TCP handle */
    uv_timer_t        timer;          /**< Inactivity timeout timer */
    unsigned int closing        : 1;        /**< Flag indicating the connection is being shut down */
    unsigned int pending_closes : 2; /**< Counter for libuv close callbacks (tcp + timer) */
    struct ClientCtx *next;           /**< Next client in the active list */
    struct ClientCtx *prev;           /**< Previous client in the active list */
    size_t            totalRead;      /**< Total bytes read during the session */
    char              buffer[BUFFER_SIZE]; /**< Per-client data buffer */
} ClientCtx;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Initializes the internal slab pool and global state.
 * @pre None.
 * @post The internal slab allocator is ready to provide ClientCtx blocks.
 * @return 0 on success, -1 on allocation failure.
 */
int  conn_manager_init   (void);

/**
 * @brief Releases all memory and destroys the slab pool.
 * @pre Should be called after all connections have been closed.
 * @post The manager is reset; further calls to alloc will fail until re-init.
 */
void conn_manager_destroy(void);

/* ── Per-connection alloc / release ─────────────────────────────────── */

/**
 * @brief Allocates a zero-initialized ClientCtx from the pool.
 * @pre conn_manager_init() was called.
 * @post A valid pointer is returned, or NULL if memory is exhausted.
 * @return Pointer to a new ClientCtx or NULL.
 */
ClientCtx *conn_manager_alloc  (void);

/**
 * @brief Returns a ClientCtx to the slab pool.
 * @pre ctx was allocated via conn_manager_alloc.
 * @pre pending_closes must be 0 (all libuv handles closed).
 * @post The memory is returned to the pool for future reuse.
 * @param ctx Pointer to the context to release.
 */
void        conn_manager_release(ClientCtx *ctx);

/* ── Active list ─────────────────────────────────────────────────────── */

/**
 * @brief Adds a connection context to the active tracking list.
 * @pre ctx is allocated and not already linked.
 * @post ctx is added to the head of the global list; active count increases.
 * @param ctx Pointer to the context to link.
 */
void conn_manager_link         (ClientCtx *ctx);

/**
 * @brief Removes a connection context from the active tracking list.
 * @pre ctx is currently linked in the list.
 * @post ctx is unlinked; active count decreases.
 * @param ctx Pointer to the context to unlink.
 */
void conn_manager_unlink       (ClientCtx *ctx);

/**
 * @brief Returns the number of currently active connections.
 * @return Integer representing the count of linked connections.
 */
int  conn_manager_active_count (void);

/**
 * @brief Iterates over all active connections and applies a function.
 * Safely handles unlinking or modifications during iteration by caching 
 * the 'next' pointer. Used primarily for mass disconnection/shutdown.
 * @param fn Function pointer to apply to each ClientCtx.
 */
void conn_manager_close_all    (void (*fn)(ClientCtx *ctx));

#endif /* CONNECTION_MANAGER_H */