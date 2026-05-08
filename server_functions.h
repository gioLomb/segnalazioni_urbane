/**
 * server_functions.h — Public interface of the libuv server core
 *
 * Exposes only what route_handler.c and other translation units need:
 *   - g_sessions, g_geo_table  — shared in-memory tables
 *   - stats                    — aggregate server counters
 *   - RateEntry                — type used by the rate-limiter hash table
 *   - hash_key()               — hash function shared by every Hash_Table
 *   - config_signal_context()  — SIGPIPE suppression
 *   - server_loop()            — starts the event loop
 *
 * Everything else (WriteReq, connection callbacks, loop handle, rate table)
 * is private to server_functions.c.
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"
#include "client_pool.h"
#include "hash_table.h"
#include "route_handler.h"

/* ── Shared application state ────────────────────────────────────────── */

/**
 * In-memory session store: token → uint64_t userId.
 * Allocated in main(), used by session_* helpers in route_handler.c.
 */
extern Hash_Table *g_sessions;

/**
 * City geometry table: comune name → CityGeo (bbox + centroid).
 * Populated at startup by geo_load(); read-only during request handling.
 */
extern Hash_Table *g_geo_table;

/* ── Server statistics ───────────────────────────────────────────────── */

/**
 * Aggregate counters updated on every connection and request.
 * Single source of truth — no separate g_stat_* mirrors.
 * Exposed to route_handler.c for the /api/stats endpoint.
 */
typedef struct {
    int           activeClients;
    unsigned long totalRequests;
    unsigned long totalConnections;
    time_t        startTime;
} ServerStats;

extern ServerStats stats;

/* ── Rate-limiter entry ──────────────────────────────────────────────── */

/**
 * Sliding-window state for one IP address.
 * countPrev/countCurr implement a weighted cross-window estimate.
 */
typedef struct {
    unsigned long countCurr;
    unsigned long countPrev;
    time_t        windowStartTime;
} RateEntry;

/* ── Functions ───────────────────────────────────────────────────────── */

/** MurmurHash-inspired hash shared by every Hash_Table in the application. */
unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);

/** Installs SIG_IGN for SIGPIPE. */
void config_signal_context(void);

/** Starts the libuv event loop and blocks until SIGINT. */
void server_loop(Hash_Table *rate_limit_table);

#endif /* SERVER_FUNCTIONS_H */