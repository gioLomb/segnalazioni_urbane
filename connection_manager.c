#include "connection_manager.h"
#include "slab_allocator.h"
#include <string.h>

/** Capacity of each memory slab for ClientCtx objects */
#define CONN_CHUNK_CAPACITY 64

/* ── Module state ────────────────────────────────────────────────────── */

// Global pool for ClientCtx memory management
static SlabPool   g_pool         = {0};
// Head of the doubly-linked list for active (live) connections
static ClientCtx *g_active_head  = NULL;
// Counter for monitoring current load
static int        g_active_count = 0;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int conn_manager_init(void) {
    // Initialize slab allocator with the size of ClientCtx
    return slab_pool_init(&g_pool, sizeof(ClientCtx), CONN_CHUNK_CAPACITY);
}

void conn_manager_destroy(void) {
    // Release all slabs back to the system
    slab_pool_destroy(&g_pool);
    // Reset tracking state
    g_active_head  = NULL;
    g_active_count = 0;
}

/* ── Per-connection alloc / release ─────────────────────────────────── */

ClientCtx *conn_manager_alloc(void) {
    // Request a block from the slab. Memory is already zeroed by the allocator.
    ClientCtx *ctx = slab_pool_alloc(&g_pool);
    if (!ctx) return NULL;

    /* Slab memory is zeroed; reset explicitly for clarity and safety. */
    ctx->closing        = false;
    ctx->pending_closes = 0;
    ctx->totalRead      = 0;
    ctx->buffer         = NULL;   /* allocated lazily in alloc_cb */
    ctx->next           = NULL;
    ctx->prev           = NULL;
    return ctx;
}

void conn_manager_release(ClientCtx *ctx) {
    /* Free the receive buffer if it was ever allocated, then return the
     * slab block. Pointer becomes invalid for the caller immediately. */
    free(ctx->buffer);
    ctx->buffer = NULL;
    slab_pool_free(&g_pool, ctx);
}

/* ── Active list ─────────────────────────────────────────────────────── */

void conn_manager_link(ClientCtx *ctx) {
    // Standard O(1) prepend operation for doubly-linked list
    ctx->next = g_active_head;
    ctx->prev = NULL;
    
    if (g_active_head) {
        g_active_head->prev = ctx;
    }
    
    g_active_head = ctx;
    g_active_count++;
}

void conn_manager_unlink(ClientCtx *ctx) {
    // Adjust adjacent nodes or the global head if we are at the start
    if (ctx->prev) {
        ctx->prev->next = ctx->next;
    } else {
        g_active_head   = ctx->next;
    }
    
    if (ctx->next) {
        ctx->next->prev = ctx->prev;
    }
    
    // Clean up pointers in the removed node to prevent accidental re-traversal
    ctx->next = ctx->prev = NULL;
    g_active_count--;
}

int conn_manager_active_count(void) {
    return g_active_count;
}

void conn_manager_close_all(void (*fn)(ClientCtx *ctx)) {
    ClientCtx *curr = g_active_head;
    
    // Iterate through all active clients
    while (curr) {
        // Cache next pointer because 'fn' (e.g. server_close_client) 
        // will trigger unlinking, which modifies curr->next.
        ClientCtx *next = curr->next;
        fn(curr);
        curr = next;
    }
}