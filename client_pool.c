#include "client_pool.h"
#include <stdlib.h>
#include <string.h>

/*
 * MemoryChunk - a fixed-size slab that stores CHUNK_SIZE ClientCtx objects.
 *
 * Slots are allocated as a plain array embedded directly in the struct, keeping
 * them contiguous in memory and avoiding per-slot heap fragmentation.
 * Free slots within this chunk are linked through ClientCtx.next, forming a
 * singly-linked local free list that makes alloc/release O(1).
 *
 * Chunks are organised in a doubly-linked list (next/prev) so that a fully
 * freed chunk can unlink and free itself in O(1) without traversing the list
 */
typedef struct MemoryChunk {
    ClientCtx clients[CHUNK_SIZE];
    ClientCtx *localFreeList;
    int usedCount;
    struct MemoryChunk *next;
    struct MemoryChunk *prev;
} MemoryChunk;

static MemoryChunk *chunksHead = NULL;

/*
 * Allocates and initialises a new MemoryChunk.
 *
 * Uses calloc() so all fields start zeroed. Then walks the clients array to
 * build the local free list: each slot's next pointer is wired to the
 * following slot, and every slot's parentChunk is set to this chunk so that
 * client_pool_release() can locate the owning chunk in O(1).
 * The last slot's next is left NULL to terminate the free list.
 * usedCount starts at 0; next and prev are left NULL (calloc).
 *
 * Returns the new chunk pointer, or NULL if calloc fails.
 */
static MemoryChunk* create_chunk(void) {
    MemoryChunk *c = calloc(1, sizeof(MemoryChunk));
    if (!c) return NULL;

    // link each client
    for (int i = 0; i < CHUNK_SIZE - 1; i++) {
        c->clients[i].next = &c->clients[i + 1];
        c->clients[i].parentChunk = c; 
    }
    c->clients[CHUNK_SIZE - 1].next = NULL;
    c->clients[CHUNK_SIZE - 1].parentChunk = c;

    //all slots are free at the start
    c->localFreeList = &c->clients[0];
    c->usedCount = 0;
    return c;
}

int client_pool_init(void) {
    chunksHead = create_chunk();
    return (chunksHead != NULL) ? 0 : -1;
}

ClientCtx* client_pool_alloc(void) {
    MemoryChunk *curr = chunksHead;

    // find an unfull chunk
    while (curr && curr->usedCount == CHUNK_SIZE) {
        curr = curr->next;
    }

    // create new chunk if noone has free space
    if (!curr) {
        curr = create_chunk();
        if (!curr) return NULL;
        // add in head
        curr->next = chunksHead;
        if (chunksHead) chunksHead->prev = curr;
        chunksHead = curr;
    }

    // extract the ClientCtx object from the chunk
    ClientCtx *ctx = curr->localFreeList;
    curr->localFreeList = ctx->next; 
    curr->usedCount++;

    // zero all fields except parentChunk which must be preserved
    memset(ctx->buffer, 0, BUFFER_SIZE);
    ctx->closing = 0;
    ctx->sockEv  = (ConnectionEvent){0};
    ctx->timerEv = (ConnectionEvent){0};
    ctx->next = ctx->prev = NULL;
    
    return ctx;
}

void client_pool_release(ClientCtx *ctx) {
    if (!ctx) return;
    MemoryChunk *c = (MemoryChunk*)ctx->parentChunk;

    // release the object
    ctx->next = c->localFreeList;
    c->localFreeList = ctx;
    c->usedCount--;

    // shrink chunk if ref count reaches 0, unless it's the last one
    if (c->usedCount == 0 && (c->next || c->prev)) {
        if (c->prev) c->prev->next = c->next;
        if (c->next) c->next->prev = c->prev;
        if (c == chunksHead) chunksHead = c->next;
        free(c);
    }
}

void client_pool_destroy(void) {
    MemoryChunk *curr = chunksHead;
    while (curr) {
        MemoryChunk *next = curr->next;
        free(curr);
        curr = next;
    }
    chunksHead = NULL;
}