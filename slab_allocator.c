#include "slab_allocator.h"
#include <stdlib.h>
#include <string.h>

// ── Internal Chunk type ───────────────────────────────────────────────

// Each Chunk is a contiguous slab of memory divided into fixed-size blocks.
// Chunks form a doubly-linked list to allow O(1) unlinking when a chunk is empty.
typedef struct Chunk {
    struct Chunk *next;         // Next chunk in the pool
    struct Chunk *prev;         // Previous chunk in the pool
    void         *local_free;   // Head of this chunk's singly-linked free-list
    int           used_count;   // Number of blocks currently allocated
    int           capacity;     // Total number of blocks in this chunk
    char          data[];       // Flexible array member for raw block data
} Chunk;

// ── Helpers ───────────────────────────────────────────────────────────

// Allocates and initializes a new memory chunk.
// Each block in the chunk includes a hidden back-pointer to the Chunk itself.
static Chunk *create_chunk(SlabPool *pool) {
    size_t data_size = (size_t)pool->chunk_capacity * pool->internal_bsize;
    Chunk *c = calloc(1, sizeof(Chunk) + data_size);
    
    if (!c) return NULL;

    c->capacity   = pool->chunk_capacity;
    c->used_count = 0;

    // Initialize the free-list inside the chunk data area.
    // Each free block stores the address of the next free block at its start.
    for (int i = 0; i < c->capacity; i++) {
        char *block_start = c->data + (i * pool->internal_bsize);
        
        // Set the hidden back-pointer to this Chunk header
        *(Chunk **)block_start = c;

        // Thread the block into the local free-list
        // The pointer returned to the user is at (block_start + sizeof(void*))
        void *user_area = block_start + sizeof(void *);
        *(void **)user_area = c->local_free;
        c->local_free = user_area;
    }

    return c;
}

// ── Public API ────────────────────────────────────────────────────────

int slab_pool_init(SlabPool *pool, size_t block_size, int chunk_capacity) {
    if (!pool) return -1;

    pool->block_size     = block_size;
    // Each block contains a hidden pointer to the owner chunk + the user data
    pool->internal_bsize = sizeof(void *) + block_size;
    pool->chunk_capacity = chunk_capacity;
    
    // Pre-allocate the first chunk to ensure the pool is ready
    pool->chunks_head = create_chunk(pool);
    return pool->chunks_head ? 0 : -1;
}

void slab_pool_destroy(SlabPool *pool) {
    if (!pool) return;

    Chunk *chunk = (Chunk *)pool->chunks_head;
    while (chunk) {
        Chunk *next = chunk->next;
        free(chunk); // Release the entire contiguous block
        chunk = next;
    }
    pool->chunks_head = NULL;
}

void *slab_pool_alloc(SlabPool *pool) {
    if (!pool) return NULL;

    // Search for a chunk that has at least one free slot
    Chunk *chunk = (Chunk *)pool->chunks_head;
    while (chunk && !chunk->local_free)
        chunk = chunk->next;

    // If no chunk has space, we grow the pool by adding a new chunk at the head
    if (!chunk) {
        chunk = create_chunk(pool);
        if (!chunk) return NULL;
        
        chunk->next = (Chunk *)pool->chunks_head;
        if (pool->chunks_head)
            ((Chunk *)pool->chunks_head)->prev = chunk;
        pool->chunks_head = chunk;
    }

    /* Pop the first available block from this chunk's local free-list. */
    void *user_ptr    = chunk->local_free;
    chunk->local_free = *(void **)user_ptr;
    chunk->used_count++;

    /*
     * Do NOT zero the block here.  conn_manager_alloc() already resets
     * every field of ClientCtx explicitly after allocation, so a full
     * memset of 8 KB per connection is pure waste.  If this pool is ever
     * reused for a different type that requires zeroing, add the memset
     * in that allocator's wrapper, not here.
     */
    return user_ptr;
}

void slab_pool_free(SlabPool *pool, void *ptr) {
    if (!ptr) return;

    // The hidden header (Chunk*) is stored just before the user pointer
    Chunk *chunk = *(Chunk **)((char *)ptr - sizeof(void *));

    // Push the block back onto the chunk's free-list
    *(void **)ptr  = chunk->local_free;
    chunk->local_free = ptr;
    chunk->used_count--;

    // Optimization: If a chunk becomes entirely empty, free it to the system.
    // We keep the last chunk to avoid frequent allocation/deallocation (thrashing).
    if (chunk->used_count == 0 && (chunk->next || chunk->prev)) {
        // Unlink the chunk from the doubly-linked list
        if (chunk->prev) chunk->prev->next = chunk->next;
        if (chunk->next) chunk->next->prev = chunk->prev;
        
        // Update pool head if the first chunk was deleted
        if (pool->chunks_head == chunk)
            pool->chunks_head = chunk->next;
            
        free(chunk);
    }
}