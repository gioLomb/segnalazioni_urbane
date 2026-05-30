/**
 * @file rate_limiter.h
 * @brief Sliding-window per-IP rate limiter.
 *
 * Backed by a hash table keyed on IPv4 address strings. Uses two
 * counters (current and previous window) to estimate the request rate
 * over the last second without storing individual timestamps.
 *
 * Lifecycle: rate_limiter_init() at startup, rate_limiter_destroy() at shutdown.
 */

#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include "config.h"
#include "hash_table.h"

#define RATE_LIMIT_RPS    100    /* max requests/sec per IP */
#define MAX_SAVED_IP      10000
#define ENABLE_RATE_LIMIT  1        /* set to 1 to enforce rate limiting   */


/**
 * @brief Per-IP state stored in the rate-limit hash table.
 */
typedef struct {
    unsigned long countCurr;       /**< Requests in the current 1-second window  */
    unsigned long countPrev;       /**< Requests in the previous 1-second window */
    time_t        windowStartTime; /**< Unix timestamp when countCurr was reset  */
} RateEntry;

/**
 * @brief Allocates the internal rate-limit hash table.
 *
 * Must be called once at startup before any call to rate_limit_check().
 *
 * @post The internal table is ready; rate_limit_check() can be called.
 * @return 0 on success, -1 on allocation failure.
 */
int rate_limiter_init(void);

/**
 * @brief Frees the internal rate-limit table.
 *
 * @post The table pointer is set to NULL; rate_limit_check() returns 1
 *       (allow all) until re-initialised.
 */
void rate_limiter_destroy(void);

/**
 * @brief Sliding-window check — returns 1 if the request is allowed, 0 to reject.
 *
 * The table is recycled automatically when it exceeds 10 000 entries to
 * bound memory usage. Always returns 1 when ENABLE_RATE_LIMIT == 0.
 *
 * @param ip NUL-terminated IPv4 address string of the client.
 * @return 1 if within the rate limit, 0 if the request should be rejected.
 */
int rate_limit_check(const char *ip);

#endif /* RATE_LIMITER_H */