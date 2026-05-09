#include "session.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/**
 * Internal helper to generate random hex characters.
 * Priority: /dev/urandom for cryptographic safety.
 * Fallback: time-seeded rand() for portability.
 */
static void generate_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";

    // Attempting to use the system's random device for high-entropy data
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        // Every byte of entropy results in two hex characters (one per nibble)
        size_t bytes = len / 2;
        unsigned char buf[TOKEN_HEX_LEN / 2]; 
        if (bytes > sizeof(buf)) bytes = sizeof(buf);

        if (fread(buf, 1, bytes, f) == bytes) {
            fclose(f);
            // Converting raw binary data into a readable hex string
            for (size_t i = 0; i < bytes; i++) {
                out[i * 2]     = hex[buf[i] >> 4];
                out[i * 2 + 1] = hex[buf[i] & 0x0F];
            }
            out[len] = '\0';
            return;
        }
        fclose(f);
    }

    // Fallback mechanism: not cryptographically secure, used if /dev/urandom is unavailable
    static int seeded = 0;
    if (!seeded) {
        // Mixing time and PID to increase seed variability in local environments
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = 1;
    }

    for (size_t i = 0; i < len; i++)
        out[i] = hex[(unsigned char)rand() % 16];
    out[len] = '\0';
}

bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken) {
    if (!sessionTable || !outToken) return false;

    char    token[TOKEN_HEX_LEN + 1];
    uint64_t dummy;
    int      max_tries = 100;

    // Collision avoidance loop: ensure the new token isn't already assigned to a live session
    do {
        generate_token(token, TOKEN_HEX_LEN);
        // Safety break to prevent infinite loops if the table is full or failing
        if (--max_tries <= 0) return false;
    } while (session_verify(sessionTable, token, &dummy));

    Session s;
    s.userId     = userId;
    s.created_at = time(NULL);

    // Store the session record keyed by the token string (including null terminator for exact match)
    if (!ht_set(sessionTable, token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    // Copy the generated token string to the caller's output buffer
    strcpy(outToken, token);
    return true;
}

bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId) {
    if (!sessionTable || !token) return false;

    Session s;
    // Attempt to retrieve the session object from the table
    if (!ht_get(sessionTable, (void *)token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    // Expiration check: calculate the difference between current time and creation time
    if (difftime(time(NULL), s.created_at) > SESSION_MAX_AGE) {
        // Lazy Eviction: Token has expired, delete it from the table immediately and deny access
        printf("sessione expired");
        ht_delete(sessionTable, (void *)token, strlen(token) + 1);
        return false;
    }

    // Session is valid; assign the userId back to the caller
    if (outUserId) *outUserId = s.userId;
    return true;
}

void session_destroy(Hash_Table *sessionTable, const char *token) {
    // Explicit session removal, typically used for logouts
    if (sessionTable && token)
        ht_delete(sessionTable, (void *)token, strlen(token) + 1);
}

void session_clear_all(Hash_Table *sessionTable) {
    // TODO: implement full sweep for debug/shutdown use to clear all entries at once
    (void)sessionTable;
}