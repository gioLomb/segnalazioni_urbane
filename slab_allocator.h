/**
 * @file slab_allocator.h
 * @brief Generic fixed-size block allocator (slab allocator).
 *
 * Manages many objects of the same size efficiently by allocating memory
 * in large "chunks" and subdividing them into fixed-size slots. This
 * reduces heap fragmentation and keeps alloc/free at O(1).
 *
 * Memory layout of each internal block:
 *   [ Chunk* back-ptr ][ ... block_size bytes of user data ... ]
 *
 * The hidden back-pointer enables O(1) deallocation without any extra
 * metadata in the caller's struct.
 */

#ifndef SLAB_ALLOCATOR_H
#define SLAB_ALLOCATOR_H

#include <stddef.h>

/**
 * @brief Manages a collection of fixed-size memory chunks.
 *
 * Initialise with slab_pool_init() before any other call.
 * All fields are managed internally; treat as opaque from outside.
 */
typedef struct {
    size_t blockSize;      /**< User-visible size requested at init      */
    size_t internalBsize;  /**< Total block size (back-ptr + user data)  */
    int    chunkCapacity;  /**< Number of slots per allocated chunk       */
    void  *chunksHead;     /**< Opaque pointer to the head of Chunk list */
} SlabPool;

/**
 * @brief Initialises the pool for blocks of the given size.
 *
 * Allocates the first chunk immediately so the pool is ready for use
 * on return.
 *
 * @pre pool != NULL, blockSize > 0, chunkCapacity > 0.
 * @post The pool is ready; slab_pool_alloc() can be called immediately.
 * @param pool          Pointer to the SlabPool to initialise.
 * @param blockSize     Size in bytes of each individual allocation.
 * @param chunkCapacity Number of blocks to provision per chunk.
 * @return 0 on success, -1 if the initial chunk allocation fails.
 */
int slab_pool_init(SlabPool *pool, size_t blockSize, int chunkCapacity);

/**
 * @brief Releases all memory owned by the pool.
 *
 * @pre pool was previously initialised with slab_pool_init().
 * @post All chunks are freed. Any pointer returned by slab_pool_alloc()
 *       becomes dangling after this call.
 * @param pool Pointer to the SlabPool to destroy.
 */
void slab_pool_destroy(SlabPool *pool);

/**
 * @brief Allocates one block from the pool.
 *
 * The returned memory is NOT zero-initialised; callers that require
 * zeroing must do so themselves.
 *
 * @pre pool is initialised.
 * @post On success, a pointer to exactly blockSize bytes is returned.
 *       The pool may internally allocate a new chunk if all existing
 *       chunks are full.
 * @param pool Pointer to the SlabPool.
 * @return Pointer to the allocated block, or NULL if out of memory.
 */
void *slab_pool_alloc(SlabPool * restrict pool);

/**
 * @brief Returns a block to the pool for reuse.
 *
 * Empty chunks are freed immediately, except the last one, which is
 * retained to avoid allocation thrashing under low load.
 *
 * @pre ptr was returned by slab_pool_alloc() from the same pool.
 * @post The block is pushed onto the chunk's free-list; the chunk
 *       may be freed if it becomes entirely empty.
 * @param pool Pointer to the SlabPool.
 * @param ptr  Pointer to the block to return.
 */
void slab_pool_free(SlabPool * restrict pool, void * restrict ptr);

#endif /* SLAB_ALLOCATOR_H */
