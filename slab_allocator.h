/**
 * @file slab_allocator.h
 * @brief Generic fixed-size block allocator (Slab Allocator).
 *
 * This module provides an efficient way to manage many objects of the same size.
 * It reduces fragmentation and improves performance by allocating memory in 
 * large "chunks" and dividing them into fixed-size slots.
 *
 * Memory layout per block (internal):
 * [ Chunk* back-ptr ][ ... block_size bytes of user data ... ]
 * The hidden header (back-ptr) allows O(1) deallocation without requiring 
 * the user to store extra metadata in their structures.
 */

#ifndef SLAB_ALLOCATOR_H
#define SLAB_ALLOCATOR_H

#include <stddef.h>

/**
 * @brief SlabPool structure — manages a collection of memory chunks.
 *
 * This structure should be initialized via slab_pool_init(). It tracks
 * block sizes and the linked list of active memory chunks.
 */
typedef struct {
    size_t block_size;       /**< User-visible size requested at init */
    size_t internal_bsize;   /**< Total size (back-pointer + user data) */
    int    chunk_capacity;   /**< Number of slots per allocated chunk */
    void  *chunks_head;      /**< Opaque pointer to the head of Chunk list */
} SlabPool;

/**
 * @brief Initializes the pool for blocks of a specific size.
 * @pre pool is a valid pointer to a SlabPool structure.
 * @pre block_size > 0 and chunk_capacity > 0.
 * @post The pool is ready for allocations; the first chunk is created.
 * @param pool Pointer to the SlabPool to initialize.
 * @param block_size Size of each individual allocation in bytes.
 * @param chunk_capacity Number of blocks to allocate in each chunk.
 * @return 0 on success, -1 if the initial memory allocation fails.
 */
int  slab_pool_init   (SlabPool *pool, size_t block_size, int chunk_capacity);

/**
 * @brief Releases all memory associated with the pool.
 * @pre pool was previously initialized.
 * @post All chunks are freed; any pointers returned by slab_pool_alloc become dangling.
 * @param pool Pointer to the SlabPool to destroy.
 */
void slab_pool_destroy(SlabPool *pool);

/**
 * @brief Allocates a single block of memory from the pool.
 * The returned memory is zero-initialized and properly aligned.
 * @pre pool is initialized.
 * @post If successful, a pointer to a block of block_size bytes is returned.
 * @param pool Pointer to the SlabPool.
 * @return A pointer to the allocated block, or NULL if out of memory.
 */
void *slab_pool_alloc (SlabPool *pool);

/**
 * @brief Returns a block to the pool for reuse.
 * @pre ptr was returned by a previous call to slab_pool_alloc from the same pool.
 * @post The block is added to the chunk's free-list; empty chunks may be released.
 * @param pool Pointer to the SlabPool.
 * @param ptr Pointer to the block to free.
 */
void slab_pool_free (SlabPool *pool, void *ptr);

#endif /* SLAB_ALLOCATOR_H */