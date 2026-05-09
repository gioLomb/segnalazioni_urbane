/**
 * @file session.h
 * @brief Session management module using a thread-safe hash table.
 * This module handles the creation, verification, and destruction of user sessions.
 * Each session is identified by a secure 128-bit hex token and has a 
 * fixed expiration time defined by SESSION_MAX_AGE.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "hash_table.h"
#include "config.h"

/** Length of the hexadecimal token string (excluding null terminator) */
#define TOKEN_HEX_LEN 32
/** The key name used for identifying the session in HTTP cookies */
#define SESSION_COOKIE_NAME "sid"
/** Maximum duration of a session: 24 hours in seconds */
#define SESSION_MAX_AGE     24*60*60

/**
 * @brief Structure stored as a value in the session hash table.
 */
typedef struct {
    uint64_t userId;     /**< The identifier of the authenticated user */
    time_t   created_at; /**< Unix timestamp of when the session was initialized */
} Session;

/**
 * @brief Generates a secure token and creates a new session in the table.
 * @pre sessionTable must be an initialized and valid Hash_Table pointer.
 * @pre outToken must point to a buffer of at least TOKEN_HEX_LEN + 1 bytes.
 * @post If successful, a new entry is added to the hash table and outToken is populated.
 * @param sessionTable The hash table where the session will be stored.
 * @param userId The ID of the user associated with this session.
 * @param outToken Destination buffer for the generated hex token string.
 * @return true if the session was created successfully, false otherwise.
 */
bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken);

/**
 * @brief Verifies if a token is valid, active, and has not expired.
 * Includes a "lazy eviction" mechanism: if a token is found but is expired, 
 * it is automatically removed from the table.
 * @pre sessionTable is initialized.
 * @pre token is a non-null string.
 * @post If expired, the session entry is deleted from the table.
 * @param sessionTable The hash table containing active sessions.
 * @param token The session identifier string provided by the client.
 * @param outUserId Pointer to store the user's ID if the session is valid.
 * @return true if the session exists and is valid, false if not found or expired.
 */
bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId);

/**
 * @brief Explicitly removes a session from the table (Logout).
 * @pre sessionTable and token are not NULL.
 * @post The session entry is deleted from the table if it existed.
 * @param sessionTable The session hash table.
 * @param token The token string to be invalidated.
 */
void session_destroy(Hash_Table *sessionTable, const char *token);

/**
 * @brief Removes all sessions from the table.
 * Useful for system shutdown or debugging purposes.
 * @param sessionTable The session hash table.
 */
void session_clear_all(Hash_Table *sessionTable);

#endif /* SESSION_H */