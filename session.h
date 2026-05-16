#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"
#include "user.h"      /* User — embedded in Session to avoid per-request DB lookup */

/** Length of the hexadecimal token string (excluding null terminator) */
#define TOKEN_HEX_LEN 32
/** The key name used for identifying the session in HTTP cookies */
#define SESSION_COOKIE_NAME "sid"
/** Maximum duration of a session: 24 hours in seconds */
#define SESSION_MAX_AGE     (24*60*60)

/**
 * Session record stored internally.
 *
 * Embeds the full User struct so that route handlers can obtain username,
 * city and role from the session cache without an extra DB round-trip.
 */
typedef struct {
    User   user;       /**< Full user record, copied at login time     */
    time_t created_at; /**< Unix timestamp when the session was created */
} Session;

/**
 * Allocates the internal session table.
 * Must be called once at startup before any other session_* function.
 * Returns 0 on success, -1 on allocation failure.
 */
int  session_init(void);

/**
 * Destroys the internal session table and frees all memory.
 * Should be called at shutdown.
 */
void session_destroy_all(void);

/**
 * Creates a new session for the given user and writes the token to outToken.
 *
 * @param user      Pointer to the authenticated User struct (copied in).
 * @param outToken  Buffer of at least TOKEN_HEX_LEN + 1 bytes.
 * @return true on success, false on allocation or collision failure.
 */
bool session_create(const User *user, char *outToken);

/**
 * Verifies a token and, if valid, copies the cached User into *outUser.
 *
 * Includes lazy eviction: expired sessions are deleted before returning false.
 *
 * @param token    NUL-terminated hex token from the Cookie header.
 * @param outUser  Destination for the cached User (may be NULL).
 * @return true if the session exists and has not expired, false otherwise.
 */
bool session_verify(const char *token, User *outUser);

/**
 * Removes a session from the table (logout).
 */
void session_destroy(const char *token);

#endif /* SESSION_H */