#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"
#include "client_pool.h"
#include "hash_table.h"
#include "route_handler.h"

extern volatile sig_atomic_t keepRunning;

/* Session table: lives in server_functions.c, used by route_handler.c */
extern Hash_Table *g_sessions;

typedef struct { int serverFd; int epollFd; } ServerCtx;

typedef struct {
    int           activeClients;
    unsigned long  totalRequests;
    unsigned long  totalConnections;
    time_t         startTime;
} ServerStats;

typedef struct {
    unsigned long countCurr;
    unsigned long countPrev;
    time_t        windowStartTime;
} RateEntry;

typedef enum { TYPE_SOCKET, TYPE_TIMER } EvType;

extern ServerStats stats;

unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);
void          config_signal_context(void);

/*
 * Sends a complete HTTP/1.1 response.
 *   setCookie – if non-NULL, emits a Set-Cookie header
 *   location  – if non-NULL, emits a Location header and uses statusCode 302
 * Content-Type is inferred from the first byte of body:
 *   '<'  → text/html     '{' '['  → application/json    else → text/plain
 */
void send_response(int socketFd, int statusCode,
                   const char *body, size_t bodyLen,
                   const char *setCookie, const char *location,
                   int keepAlive);

/*
 * Main event loop. rateLimitTable is owned and freed by the caller
 * (main) after the loop exits.
 */
void server_loop(ServerCtx sctx, Hash_Table *rateLimitTable);

#endif /* SERVER_FUNCTIONS_H */