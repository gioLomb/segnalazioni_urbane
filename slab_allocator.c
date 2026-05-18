#include "slab_allocator.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal Chunk type ─────────────────────────────────────────────── */

// Each Chunk is a contiguous slab of memory subdivided into fixed-size blocks.
// Chunks form a doubly-linked list so an empty chunk can be unlinked in O(1).
typedef struct __attribute__((aligned(64))) Chunk {
    struct Chunk *next;       // Next chunk in the pool
    struct Chunk *prev;       // Previous chunk in the pool
    void         *localFree;  // Head of this chunk's singly-linked free-list
    int           usedCount;  // Number of blocks currently allocated
    int           capacity;   // Total number of blocks in this chunk
    char          data[];     // Flexible array: raw block storage
} Chunk;

/* ── Private helpers ─────────────────────────────────────────────────── */

// Allocates and initialises a new Chunk. Each block embeds a hidden
// back-pointer to the owning Chunk for O(1) deallocation.
static Chunk *create_chunk(SlabPool *pool) {
    size_t dataSize = (size_t)pool->chunkCapacity * pool->internalBsize;
    Chunk *c = calloc(1, sizeof(Chunk) + dataSize);
    if (!c) return NULL;

    c->capacity = pool->chunkCapacity;
    c->usedCount = 0;

    // Build the free-list by threading each block through its user area.
    for (int i = 0; i < c->capacity; i++) {
        char *blockStart = c->data + (i * pool->internalBsize);

        // Store the Chunk back-pointer in the hidden header slot.
        *(Chunk **)blockStart = c;

        // The pointer returned to callers starts after the hidden header.
        void *userArea = blockStart + sizeof(void *);
        *(void **)userArea = c->localFree;
        c->localFree = userArea;
    }

    return c;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int slab_pool_init(SlabPool *pool, size_t blockSize, int chunkCapacity) {
    if (!pool) return -1;

    pool->blockSize = blockSize;
    // Each block stores a hidden Chunk* before the user-visible data.
    pool->internalBsize = sizeof(void *) + blockSize;
    pool->chunkCapacity = chunkCapacity;

    // Pre-allocate the first chunk so the pool is immediately usable.
    pool->chunksHead = create_chunk(pool);
    return pool->chunksHead ? 0 : -1;
}

void slab_pool_destroy(SlabPool *pool) {
    if (!pool) return;

    Chunk *chunk = (Chunk *)pool->chunksHead;
    while (chunk) {
        Chunk *next = chunk->next;
        free(chunk); // Each chunk is one contiguous allocation.
        chunk = next;
    }
    pool->chunksHead = NULL;
}

void *slab_pool_alloc(SlabPool *pool) {
    if (!pool) return NULL;

    // Find the first chunk with at least one free slot.
    Chunk *chunk = (Chunk *)pool->chunksHead;
    while (chunk && !chunk->localFree)
        chunk = chunk->next;

    // All chunks are full: grow the pool by prepending a new chunk.
    if (!chunk) {
        chunk = create_chunk(pool);
        if (!chunk) return NULL;

        chunk->next = (Chunk *)pool->chunksHead;
        if (pool->chunksHead)
            ((Chunk *)pool->chunksHead)->prev = chunk;
        pool->chunksHead = chunk;
    }

    // Pop the first free block from this chunk's local free-list.
    void *userPtr = chunk->localFree;
    chunk->localFree = *(void **)userPtr;
    chunk->usedCount++;

    // NOTE: memory is NOT zeroed here. client_manager_alloc() resets every
    // ClientCtx field explicitly, so a full memset would be redundant.
    return userPtr;
}

void slab_pool_free(SlabPool * restrict pool, void * restrict ptr) {
    if (!ptr) return;

    // Recover the owning Chunk from the hidden header that precedes the user pointer.
    Chunk *chunk = *(Chunk **)((char *)ptr - sizeof(void *));

    // Push the block back onto the chunk's free-list.
    *(void **)ptr = chunk->localFree;
    chunk->localFree = ptr;
    chunk->usedCount--;

    // Release the chunk when it's entirely empty, but always keep at least
    // one chunk alive to avoid thrashing under low load.
    if (chunk->usedCount == 0 && (chunk->next || chunk->prev)) {
        // Unlink from the doubly-linked list.
        if (chunk->prev) chunk->prev->next = chunk->next;
        if (chunk->next) chunk->next->prev = chunk->prev;

        // Update the pool head if we removed the first chunk.
        if (pool->chunksHead == chunk)
            pool->chunksHead = chunk->next;

        free(chunk);
    }
}
