/**
 * @file session.h
 * @brief In-memory session management backed by a hash table.
 *
 * Sessions are identified by a random hex token stored in a browser cookie.
 * Each session embeds the full User struct so route handlers can read role,
 * username and city without an extra database round-trip.
 *
 * Lifecycle: session_init() at startup, session_destroy_all() at shutdown.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"
#include "user.h"

/** Length of the hexadecimal token string, excluding the NUL terminator. */
#define TOKEN_HEX_LEN       32
/** Cookie name used to identify the session in HTTP requests. */
#define SESSION_COOKIE_NAME "sid"
/** Maximum session lifetime: 24 hours in seconds. */
#define SESSION_MAX_AGE     (24 * 60 * 60)

/**
 * @brief Session record stored in the internal hash table.
 *
 * Embeds the full User struct so route handlers can obtain username,
 * city and role from the session without an extra DB lookup.
 */
typedef struct {
    User   user;      /**< Full user record, copied at login time      */
    time_t createdAt; /**< Unix timestamp when the session was created */
} Session;

/**
 * @brief Allocates the internal session hash table.
 *
 * Must be called once at startup before any other session_* function.
 *
 * @post The session table is ready; session_create() can be called.
 * @return 0 on success, -1 on allocation failure.
 */
int session_init(void);

/**
 * @brief Destroys the internal session table and frees all memory.
 *
 * @post All sessions are invalidated; the table pointer is set to NULL.
 */
void session_destroy_all(void);

/**
 * @brief Creates a new session for the given user and writes the token to outToken.
 *
 * Generates a cryptographically random hex token and retries on the rare
 * chance of a collision with an existing session.
 *
 * @pre session_init() has been called.
 * @param user     Pointer to the authenticated User struct (copied in).
 * @param outToken Buffer of at least TOKEN_HEX_LEN + 1 bytes.
 * @return true on success, false on allocation or collision failure.
 */
bool session_create(const User *user, char *outToken);

/**
 * @brief Verifies a token and copies the cached User into outUser if valid.
 *
 * Performs lazy eviction: expired sessions are removed on first access
 * rather than by a background sweep.
 *
 * @param token   NUL-terminated hex token read from the Cookie header.
 * @param outUser Destination for the cached User (may be NULL).
 * @return true if the session exists and has not expired, false otherwise.
 */
bool session_verify(const char *token, User *outUser);

/**
 * @brief Removes a single session from the table (logout).
 * @param token NUL-terminated hex token of the session to remove.
 */
void session_destroy(const char *token);

#endif /* SESSION_H */