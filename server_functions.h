/**
 * server_functions.h — Public interface of the libuv server core
 *
 * Exposes only what route_handler.c and other translation units need:
 *   - RateEntry                — type used by the rate-limiter hash table
 *   - hash_key()               — hash function shared by every Hash_Table
 *   - config_signal_context()  — SIGPIPE suppression
 *   - server_loop()            — starts the event loop
 *
 * g_sessions and g_geo_table are no longer exposed here: they are private
 * to session.c and geo.c respectively (singleton pattern).
 */

#ifndef SERVER_FUNCTIONS_H
#define SERVER_FUNCTIONS_H

#include "config.h"
#include "hash_table.h"
#include "route_handler.h"

#define MAX_HEADER_STR_LEN 512
#define MAX_NUMBER_LEN 24

typedef struct {
    unsigned long countCurr;
    unsigned long countPrev;
    time_t        windowStartTime;
} RateEntry;

unsigned long hash_key(const void *key, size_t keySize, unsigned long seed);
void config_signal_context(void);
void server_loop();

#endif /* SERVER_FUNCTIONS_H */