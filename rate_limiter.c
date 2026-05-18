/**
 * @file rate_limiter.c
 * @brief Sliding-window per-IP rate limiter implementation.
 */

#include "rate_limiter.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Internal state ──────────────────────────────────────────────────── */

// Singleton hash table: IPv4 string → RateEntry.
static Hash_Table *s_rate_table = NULL;

/* ── Public API ──────────────────────────────────────────────────────── */

int rate_limiter_init(void) {
    extern unsigned long hash_key(const void *, size_t, unsigned long);
    s_rate_table = ht_create(1024, hash_key);
    if (!s_rate_table) {
        fprintf(stderr, "Fatal: rate limiter table allocation failed\n");
        return -1;
    }
    return 0;
}

void rate_limiter_destroy(void) {
    if (s_rate_table) {
        ht_destroy(s_rate_table, NULL);
        s_rate_table = NULL;
    }
}

int rate_limit_check(const char *ip) {
    if (unlikely(!ip || !ip[0] || !s_rate_table)) return 1;

    // Recycle the table when it grows too large; brief amnesia on all IPs
    // is acceptable compared to unbounded memory growth.
    if (unlikely(s_rate_table->size > 10000)) {
        extern unsigned long hash_key(const void *, size_t, unsigned long);
        ht_destroy(s_rate_table, NULL);
        s_rate_table = ht_create(1024, hash_key);
        if (!s_rate_table) return 1;
    }

    RateEntry e = {0};
    time_t now = time(NULL);
    ht_get(s_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));

    // Rotate windows when a full second has elapsed.
    if (unlikely(now - e.windowStartTime >= 1)) {
        e.countPrev = e.countCurr;
        e.countCurr = 0;
        e.windowStartTime = now;
    }

    // Weight the previous window by the fraction not yet elapsed,
    // then add the current count to estimate the sliding-window rate.
    double elapsed = difftime(now, e.windowStartTime);
    double estimated = e.countPrev * (1.0 - elapsed) + e.countCurr;
    int allowed = likely(estimated < RATE_LIMIT_RPS);

    if (allowed) e.countCurr++;
    ht_set(s_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
    return allowed;
}