/**
 * This module implements a slab-style memory pool for ClientCtx objects.
 * Memory is managed in fixed-size MemoryChunk blocks, each embedding
 * CHUNK_SIZE ClientCtx slots contiguously to avoid per-connection heap
 * fragmentation and reduce allocator pressure under high connection rates.
 *
 * Within each chunk, free slots are linked through a local free list
 * (ClientCtx.next), making alloc and release O(1). Every slot stores a
 * back-pointer to its owning chunk (parentChunk), so client_pool_release()
 * can locate the chunk in O(1) without any global search.
 *
 * Chunks are organised in a doubly-linked list anchored at chunksHead.
 * When a chunk's usedCount reaches zero and it is not the last remaining
 * chunk, it is unlinked and freed immediately, keeping resident memory
 * proportional to the peak concurrent client count. The last chunk is always
 * retained to avoid repeated alloc/free cycles under low load.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include "config.h"
#include <sys/epoll.h>
#include <stdbool.h>

#define CHUNK_SIZE 64 

struct MemoryChunk;

/**
 * Describes a single monitored fd. Embedded (not heap-allocated) inside
 * ClientCtx, so &ctx->sockEv or &ctx->timerEv can be stored directly in
 * epoll_event.data.ptr without any extra malloc.
 * On a fired event the handler casts data.ptr to ConnectionEvent*, reads
 * type to decide the action, and follows parent to reach the owning context.
 */
typedef struct {
    struct ClientCtx *parent;
    int fd;
    int type; 
} ConnectionEvent;

/**
 * Per-connection context. sockEv and timerEv are embedded ConnectionEvent
 * structs registered directly in epoll; no separate allocation is needed.
 * buffer holds the incoming HTTP request for this connection.
 * next/prev link all live contexts in a doubly-linked list anchored in
 * server_loop(), enabling O(n) iteration during graceful shutdown without
 * any auxiliary data structure. Every instance belong to a parent chunk in memory
 */
typedef struct ClientCtx {
    ConnectionEvent sockEv;
    ConnectionEvent timerEv;
    struct ClientCtx *next;
    struct ClientCtx *prev;
    struct MemoryChunk *parentChunk;
    bool closing;
    char buffer[BUFFER_SIZE]; 
} ClientCtx;

/**
 * create the first chunk head.
 * Return 0 for success, -1 otherwise.
 */
int  client_pool_init(void);

/**
 * Looks for an unfull chunk to allocate a new ClientCtx;
 * if all chunks are full, create a new chunk to hold a new client.
 * The ClientCtx instance is extracted from the local free list of the chunk
 * and returned. In case of chunk fail allocation returns NULL.
 */
ClientCtx* client_pool_alloc(void);

/**
 * delete the ClientCtx reference, putting it into the local list
 * of the chunk (taken from the parentChunk member).
 * If the reference counting of the parent chunk is 0,
 * it provides to shrink the chunk memory(unless the last one)
 */
void client_pool_release(ClientCtx *ctx);

/**
 * It frees every memory chunk starting from the head.
 */
void client_pool_destroy(void);

#endif