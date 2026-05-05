#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "hash_table.h"
#include "config.h"

/* 32 hex characters = 128 bit token */
#define TOKEN_HEX_LEN 32
#define SESSION_COOKIE_NAME "sid"
#define SESSION_MAX_AGE     24*60*60        /* 24 h in seconds */
/*
 * Value stored in the session hash table for each active session.
 * created_at is a Unix timestamp set at session_create() time and
 * checked in session_verify() against SESSION_MAX_AGE.
 */
typedef struct {
    uint64_t userId;
    time_t   created_at;
} Session;

/*
 * Creates a new session: generates a secure random token, associates it
 * with userId in sessionTable.
 * outToken must point to a buffer of at least TOKEN_HEX_LEN+1 bytes.
 * Returns true on success, false on failure.
 */
bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken);

/*
 * Verifies a token. Returns true and fills *outUserId if the token exists
 * and has not exceeded SESSION_MAX_AGE seconds.
 * If the token is expired it is deleted from the table before returning false.
 */
bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId);

/* Removes a session (logout). */
void session_destroy(Hash_Table *sessionTable, const char *token);

/* Removes all sessions (debug / shutdown). Currently a no-op. */
void session_clear_all(Hash_Table *sessionTable);

#endif