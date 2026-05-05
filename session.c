#include "session.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/*
 * Fills out[0..len-1] with cryptographically random hex characters using
 * /dev/urandom. Falls back to rand() (seeded with time^pid) if the device
 * cannot be opened — acceptable for non-production environments.
 */
static void generate_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";

    /* Try /dev/urandom first. */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        /* Read len/2 raw bytes and expand each nibble to a hex character. */
        size_t bytes = len / 2;
        unsigned char buf[TOKEN_HEX_LEN / 2];  /* max we ever need */
        if (bytes > sizeof(buf)) bytes = sizeof(buf);

        if (fread(buf, 1, bytes, f) == bytes) {
            fclose(f);
            for (size_t i = 0; i < bytes; i++) {
                out[i * 2]     = hex[buf[i] >> 4];
                out[i * 2 + 1] = hex[buf[i] & 0x0F];
            }
            out[len] = '\0';
            return;
        }
        fclose(f);
    }

    /* Fallback: rand() — not cryptographically secure. */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = 1;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = hex[(unsigned char)rand() % 16];
    out[len] = '\0';
}

bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken) {
    if (!sessionTable || !outToken) return false;

    /* Generate a token that does not collide with an existing live session. */
    char    token[TOKEN_HEX_LEN + 1];
    uint64_t dummy;
    int      max_tries = 100;
    do {
        generate_token(token, TOKEN_HEX_LEN);
        if (--max_tries <= 0) return false;
    } while (session_verify(sessionTable, token, &dummy));

    Session s;
    s.userId     = userId;
    s.created_at = time(NULL);

    if (!ht_set(sessionTable, token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    strcpy(outToken, token);
    return true;
}

bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId) {
    if (!sessionTable || !token) return false;

    Session s;
    if (!ht_get(sessionTable, (void *)token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    /* Check expiry. */
    if (difftime(time(NULL), s.created_at) > SESSION_MAX_AGE) {
        /* Token has expired: evict it immediately and deny access. */
        printf("sessione expired");
        ht_delete(sessionTable, (void *)token, strlen(token) + 1);
        return false;
    }

    if (outUserId) *outUserId = s.userId;
    return true;
}

void session_destroy(Hash_Table *sessionTable, const char *token) {
    if (sessionTable && token)
        ht_delete(sessionTable, (void *)token, strlen(token) + 1);
}

void session_clear_all(Hash_Table *sessionTable) {
    /* TODO: implement full sweep for debug/shutdown use. */
    (void)sessionTable;
}