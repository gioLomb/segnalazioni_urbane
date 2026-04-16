/**
 * This module contains the server entry point and the supporting functions
 * that implement the startup sequence, the epoll event loop, and HTTP
 * response formatting.
 *
 * The architecture is single-threaded and event-driven:
 *   - start_server() creates a non-blocking TCP socket and an epoll instance.
 *   - server_loop() runs epoll_wait() in a tight loop, dispatching each
 *     ready fd to either the accept path (server fd) or dispatch_event()
 *     (client fd), or closes idle connections when their timerfd fires.
 *   - Each accepted client is represented by a ClientCtx that embeds two
 *     ConnectionEvent structs (one for the socket fd, one for the timerfd).
 *     Both are registered in epoll with data.ptr pointing to the respective
 *     ConnectionEvent, so the event loop resolves type and owner with zero
 *     extra lookups and zero extra allocations.
 *   - All live ClientCtx are linked in a doubly-linked list anchored in
 *     server_loop(), which replaces the hash tables used in earlier versions
 *     and enables O(n) graceful shutdown without any auxiliary data structure.
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"
#include "client_pool.h"
#include "hash_table.h"

/* set to 0 by the SIGINT handler; read by server_loop() to exit cleanly */
extern volatile sig_atomic_t keepRunning;

/**
 * Holds the two file descriptors that define the server's I/O context:
 * the listening TCP socket and the epoll instance that monitors it together
 * with all accepted client sockets and their keepalive timerfds.
 */
typedef struct {
    int serverFd;
    int epollFd;
} ServerCtx;

/**
 * Global metrics and state tracking for the server instance.
 * Collects runtime telemetry including connection counters and uptime 
 * to monitor server health and trigger scheduled snapshots.
 */
typedef struct {
    int *activeClientsPtr;
    unsigned long totalRequests;
    unsigned long totalConnections;
    time_t        startTime;
    unsigned long keysModifiedSinceSnapshot;
    time_t        lastSnapshotTime;
} ServerStats;

/**
 * State container for the sliding window rate limiting algorithm.
 * Stores request counters for the current and previous time windows to 
 * calculate a weighted moving average, ensuring smooth traffic enforcement.
 */
typedef struct {
    unsigned long countCurr;
    unsigned long countPrev;
    time_t        windowStartTime;
} RateEntry;

typedef enum { TYPE_SOCKET, TYPE_TIMER } EvType;
extern ServerStats stats;

/**
 * Demonstrative high-performance hash function based on bit-mixing (MurmurHash3 finalizer style).
 * This function provides excellent "avalanche effect," meaning a small change 
 * in the key bits causes a significant change in the hash output. This ensures 
 * uniform distribution across the Hash Table buckets.
 */
unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);

/**
 * Parses argv looking for flags, storing the index of the load file path in
 * idxLoad and the save file path in idxSave (-1 if absent).
 * Prints usage to stderr and exits on malformed or duplicate arguments.
 */
void analyze_args(int argc, char **argv, int *idxLoad, int *idxSave);

/**
 * Installs handle_sigint as the handler for SIGINT via sigaction.
 * Must be called once before the server loop starts.
 */
void config_signal_context(void);

/**
 * Core event loop managing I/O multiplexing and server maintenance.
 * Runs epoll_wait() to dispatch socket events, handle keep-alive timers, and 
 * process incoming HTTP requests with rate limiting (rateLimitTable). Periodically 
 * triggers database snapshots (snapFilePath) to ensure persistence. On SIGINT (Ctrl+c), 
 * performs a graceful shutdown by closing all active clients and system fds.
 */
void server_loop(ServerCtx sctx, Hash_Table *db, Hash_Table *rateLimitTable, const char *snapFilePath);

/**
 * Formats and transmits a complete HTTP/1.1 response.
 * Generates the HTTP header (status line, Content-Length, Connection type) 
 * followed by the message body. Writes the formatted string directly to 
 * socketFd. Handles also the 'Keep-Alive' flag.
 */
void send_response(int socketFd, int statusCode, char *responseMsg, int keepAlive);

#endif /* SERVER_FUNCTIONS_H */