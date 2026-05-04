/**
 * server_functions.h — Public interface of the libuv server core
 *
 * This header exposes only what other translation units need:
 *   - Exported globals (g_sessions, stats counters) for use by route_handler.c
 *   - The two functions called from main() / external code (config_signal_context,
 *     server_loop)
 *   - Shared type definitions (ServerStats, RateEntry)
 *
 * Everything else (connection callbacks, WriteReq, chunk list pointer, etc.)
 * is private to server_functions.c.
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"
#include "client_pool.h"
#include "hash_table.h"
#include "route_handler.h"

/* ── Shared globals ──────────────────────────────────────────────────── */

/**
 * Set to 0 by the SIGINT handler to signal a clean shutdown.
 * Declared volatile sig_atomic_t as required by the C standard for
 * variables written in a signal handler and read in the main flow.
 */
extern volatile sig_atomic_t keepRunning;

/**
 * In-memory session store: maps a session token (hex string, NUL-included
 * as part of the key) to a uint64_t userId.
 *
 * Exported here so that session_* functions in route_handler.c can call
 * ht_get / ht_set / ht_delete on it without a function-pointer indirection.
 * Ownership: allocated in main(), destroyed at shutdown in server_loop().
 */
extern Hash_Table *g_sessions;

/**
 * In-memory city geometry table: maps comune name → CityGeo (bbox + centroid).
 * Populated at startup by geo_load(); used by route handlers for coordinate
 * validation and map centering.
 */
extern Hash_Table *g_geo_table;

/* ── Server statistics ───────────────────────────────────────────────── */

/**
 * Aggregate server statistics.
 * Updated atomically-enough for a single-threaded libuv server.
 * Exposed to route_handler.c via the /api/stats endpoint.
 */
typedef struct {
    int           activeClients;      /* connections currently open */
    unsigned long totalRequests;      /* requests handled since start */
    unsigned long totalConnections;   /* connections accepted since start */
    time_t        startTime;          /* Unix timestamp of server start */
} ServerStats;

extern ServerStats stats;

/*
 * Flat counters mirroring the fields above, kept for compatibility with
 * the /api/stats handler which accesses them through their own externs.
 */
extern unsigned long g_stat_requests;
extern unsigned long g_stat_connections;
extern time_t        g_stat_start;

/* ── Rate-limiter entry ───────────────────────────────────────────────── */

/**
 * Sliding-window rate-limiter state for one unique IP address.
 *
 *  countCurr       — requests counted in the current 1-second window.
 *  countPrev       — requests counted in the previous window (used to
 *                    compute a weighted cross-window estimate).
 *  windowStartTime — Unix timestamp when countCurr was last reset.
 */
typedef struct {
    unsigned long countCurr;
    unsigned long countPrev;
    time_t        windowStartTime;
} RateEntry;

/* ── Functions ───────────────────────────────────────────────────────── */

/**
 * MurmurHash-inspired hash used for every Hash_Table in the application.
 * Accepts arbitrary binary keys; the per-table seed mitigates hash-flooding.
 */
unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);

/**
 * Installs SIG_IGN for SIGPIPE so that writing to a closed socket returns
 * EPIPE rather than killing the process.
 * SIGINT is handled inside the event loop via uv_signal_t.
 */
void config_signal_context(void);

/**
 * Starts the libuv event loop on PORT and blocks until SIGINT is received.
 *
 * @param rateLimitTable  Pre-allocated hash table for per-IP rate limiting.
 *                        Owned by the caller; the loop uses it in-place and
 *                        does not free it.
 */
void server_loop(Hash_Table *rateLimitTable);

#endif /* SERVER_FUNCTIONS_H */