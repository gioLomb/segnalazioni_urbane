#include "connection_manager.h"
#include "slab_allocator.h"
#include <string.h>

// Number of ClientCtx slots per slab chunk.
#define CONN_CHUNK_CAPACITY 64

/* ── Module state ────────────────────────────────────────────────────── */

static SlabPool clientPool;             // Slab pool for ClientCtx allocation
static ClientCtx *ptrActiveClientsHead; // Head of the doubly-linked active list
static int activeClientsCount;          // Current number of live connections

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int conn_manager_init(void) {
    // Initialise the slab with ClientCtx-sized blocks and 64 slots per chunk.
    return slab_pool_init(&clientPool, sizeof(ClientCtx), CONN_CHUNK_CAPACITY);
}

void conn_manager_destroy(void) {
    slab_pool_destroy(&clientPool);
    ptrActiveClientsHead = NULL;
    activeClientsCount = 0;
}

/* ── Per-connection alloc / release ─────────────────────────────────── */

ClientCtx *conn_manager_alloc(void) {
    ClientCtx *ctx = slab_pool_alloc(&clientPool);
    if (!ctx) return NULL;

    // Explicitly reset all fields even though the slab doesn't guarantee zeroing.
    ctx->closing = false;
    ctx->pendingCloses = 0;
    ctx->totalRead = 0;
    ctx->buffer = NULL;
    ctx->next = NULL;
    ctx->prev = NULL;
    return ctx;
}

void conn_manager_release(ClientCtx *ctx) {
    // Free the receive buffer if it was ever allocated, then recycle the slot.
    free(ctx->buffer);
    ctx->buffer = NULL;
    slab_pool_free(&clientPool, ctx);
}

/* ── Active list ─────────────────────────────────────────────────────── */

void conn_manager_link(ClientCtx *ctx) {
    // O(1) prepend: new node becomes the new head.
    ctx->next = ptrActiveClientsHead;
    ctx->prev = NULL;

    if (ptrActiveClientsHead) ptrActiveClientsHead->prev = ctx;

    ptrActiveClientsHead = ctx;
    activeClientsCount++;
}

void conn_manager_unlink(ClientCtx *ctx) {
    // Patch adjacent nodes or update the head when removing from the front.
    if (ctx->prev){
        ctx->prev->next = ctx->next;
    }else{
        ptrActiveClientsHead = ctx->next;
    }

    if (ctx->next) ctx->next->prev = ctx->prev;

    ctx->next = ctx->prev = NULL;
    activeClientsCount--;
}

int conn_manager_active_count(void) {
    return activeClientsCount;
}

void conn_manager_close_all(void (*fn)(ClientCtx *ctx)) {
    ClientCtx *curr = ptrActiveClientsHead;

    while (curr) {
        // Cache next before calling fn, because fn (e.g. close_client) will
        // trigger conn_manager_unlink, which overwrites curr->next.
        ClientCtx *next = curr->next;
        fn(curr);
        curr = next;
    }
}
