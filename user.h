/**
 * @file user.h
 * @brief User management: registration, authentication, and role queries.
 *
 * Passwords are stored as DJB2-derived hashes over a random salt.
 * Legacy rows without a salt are supported transparently in user_authenticate().
 */

#ifndef USER_H
#define USER_H

#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define USERNAME_LEN  32
#define PWD_HASH_LEN  32
#define PWD_PLAIN_LEN 128
#define SALT_HEX_LEN  32  // 16 random bytes encoded as 32 hex characters
#define CITY_LEN      32
#define SALT_RAW_LEN 16
/**
 * @brief Role assigned to every registered user.
 */
typedef enum {
    ROLE_CITIZEN  = 0,
    ROLE_OPERATOR = 1,
    ROLE_ADMIN    = 2  /**< Municipal administration — one per city */
} UserRole;

/**
 * @brief Column indices for the SELECT queries in user.c.
 *
 * Must match the SELECT column order exactly. Adding a column requires
 * a matching entry here and in the SELECT statement.
 */
typedef enum {
    USER_COL_ID = 0,
    USER_COL_USERNAME,
    USER_COL_PASSWORD_HASH,
    USER_COL_ROLE,
    USER_COL_CITY,
    USER_COL_SALT  // NULL in legacy rows that pre-date the salt migration
} UserCol;

/**
 * @brief In-memory representation of a user row.
 */
typedef struct {
    uint64_t userId;                      /**< Unique numeric identifier for the user */
    char     username[USERNAME_LEN];      /**< Null-terminated login name */
    char     passwordHash[PWD_HASH_LEN+1];/**< Hex-encoded hash of salt+password */
    char     salt[SALT_HEX_LEN + 1];     /**< Hex-encoded random salt; empty string indicates a legacy row with no salt */
    char     city[CITY_LEN];             /**< City of residence associated with the user account */
    UserRole role;                        /**< Access level: citizen, operator, or admin */
} User;

/**
 * @brief Creates the users table if it does not exist.
 * @post The table is present and ready for reads and writes.
 * @return 0 on success, -1 on SQL error.
 */
int user_setup_table(void);

/**
 * @brief Registers a new user with a randomly salted password hash.
 *
 * Generates a random salt, computes hash(salt + plainPassword), and
 * stores both in the database.
 *
 * @param username      Desired username (must be unique).
 * @param plainPassword Plain-text password.
 * @param city          Municipality the user belongs to.
 * @param role          Role to assign (citizen, operator, or admin).
 * @return 0 on success, -1 on error (e.g. duplicate username).
 */
int user_register(const char *username, const char *plainPassword,
                  const char *city, UserRole role);

/**
 * @brief Authenticates a user against stored credentials.
 *
 * Supports both salted hashes (current) and legacy unsalted hashes
 * transparently. Uses a constant-time comparison to prevent timing attacks.
 *
 * @param username      Username to look up.
 * @param plainPassword Plain-text password to verify.
 * @param out           Populated with the user's data on success.
 * @return true if credentials are valid, false otherwise.
 */
bool user_authenticate(const char *username, const char *plainPassword,
                       User *out);

/**
 * @brief Fetches a user by primary key.
 * @param id  Row ID to look up.
 * @param out Populated with the user's data on success.
 * @return true if found, false otherwise.
 */
bool user_get_by_id(uint64_t id, User *out);

/**
 * @brief Returns true if u has the ROLE_OPERATOR role.
 * @param u Pointer to the user to check (NULL-safe).
 */
bool user_is_operator(const User *u);

/**
 * @brief Returns true if u has the ROLE_ADMIN role.
 * @param u Pointer to the user to check (NULL-safe).
 */
bool user_is_admin(const User *u);

/**
 * @brief Registers a new admin, enforcing the one-admin-per-city constraint.
 *
 * @param username      Desired username.
 * @param plainPassword Plain-text password.
 * @param city          City for which the admin is being registered.
 * @return 0 on success, -1 on error, -2 if an admin already exists for the city.
 */
int user_register_admin(const char *username, const char *plainPassword,
                        const char *city);

/**
 * @brief Serialises all operators for a city into a JSON array.
 *
 * Each element is an object with "id" (number) and "username" (string).
 * Written directly into buf using cJSON_PrintPreallocated to avoid extra allocations.
 *
 * @param buf  Output buffer.
 * @param max  Capacity of buf in bytes.
 * @param city Municipality to filter operators by.
 * @return Number of bytes written, or 0 on error.
 */
size_t user_get_operators_json(char *buf, size_t max, const char *city);

#endif /* USER_H */