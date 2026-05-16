#include "session.h"
#include "hash_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ── Internal state ──────────────────────────────────────────────────── */

static Hash_Table *s_sessions = NULL;

/* ── Private helpers ─────────────────────────────────────────────────── */

/**
 * Generates a random hex token of exactly `len` characters.
 * Uses /dev/urandom for cryptographic safety; falls back to
 * time-seeded rand() if the device is unavailable.
 */
static void generate_token(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";

    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t        bytes = len / 2;
        unsigned char buf[TOKEN_HEX_LEN / 2];
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

    /* Fallback: not cryptographically secure */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = 1;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = hex[(unsigned char)rand() % 16];
    out[len] = '\0';
}

/* ── Public API ──────────────────────────────────────────────────────── */

int session_init(void) {
    extern unsigned long hash_key(const void *, size_t, unsigned long);
    s_sessions = ht_create(0, hash_key);
    if (!s_sessions) {
        fprintf(stderr, "Fatal: session table allocation failed\n");
        return -1;
    }
    return 0;
}

void session_destroy_all(void) {
    if (s_sessions) {
        ht_destroy(s_sessions, NULL);
        s_sessions = NULL;
    }
}

bool session_create(const User *user, char *outToken) {
    if (!s_sessions || !user || !outToken) return false;

    char token[TOKEN_HEX_LEN + 1];
    User dummy;
    int  max_tries = 100;

    /* Collision avoidance: ensure the token is not already in use. */
    do {
        generate_token(token, TOKEN_HEX_LEN);
        if (--max_tries <= 0) return false;
    } while (session_verify(token, &dummy));

    Session s;
    s.user       = *user;
    s.created_at = time(NULL);

    if (!ht_set(s_sessions, token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    strcpy(outToken, token);
    return true;
}

bool session_verify(const char *token, User *outUser) {
    if (!s_sessions || !token) return false;

    Session s;
    if (!ht_get(s_sessions, (void *)token, strlen(token) + 1, &s, sizeof(s)))
        return false;

    /* Lazy eviction: delete expired sessions on first access. */
    if (difftime(time(NULL), s.created_at) > SESSION_MAX_AGE) {
        ht_delete(s_sessions, (void *)token, strlen(token) + 1);
        return false;
    }

    if (outUser) *outUser = s.user;
    return true;
}

void session_destroy(const char *token) {
    if (s_sessions && token)
        ht_delete(s_sessions, (void *)token, strlen(token) + 1);
}