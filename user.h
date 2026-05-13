#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stdbool.h>

#define USERNAME_LEN  32
#define PWD_HASH_LEN  32   /* 64 hex chars + NUL */
#define SALT_HEX_LEN  32   /* 32 hex chars + NUL  (16 random bytes → 32 hex) */
#define CITY_LEN      32

typedef enum {
    ROLE_CITIZEN  = 0,
    ROLE_OPERATOR = 1
} UserRole;

/*
 * Column indices for SELECT queries in user.c.
 * Must match the SELECT column order exactly.
 * Adding a column here + in the SELECT is all that's needed.
 */
typedef enum {
    USER_COL_ID = 0,
    USER_COL_USERNAME,
    USER_COL_PASSWORD_HASH,
    USER_COL_ROLE,
    USER_COL_CITY,
    USER_COL_SALT          /* new — NULL for legacy rows (no salt) */
} UserCol;

typedef struct {
    uint64_t userId;
    char     username[USERNAME_LEN];
    char     passwordHash[PWD_HASH_LEN + 1];
    char     salt[SALT_HEX_LEN + 1];   /* empty string = legacy row, no salt */
    char     city[CITY_LEN];
    UserRole role;
} User;

/**
 * Creates the 'users' table if it does not exist, then adds the 'salt'
 * column via ALTER TABLE if it is missing (migration for existing DBs).
 * Returns 0 on success, -1 on error.
 */
int user_setup_table(void);

/**
 * Registers a new user.  Generates a random salt, stores hash(salt+password)
 * and the salt in the DB.
 * Returns 0 on success, -1 on error (e.g. duplicate username).
 */
int user_register(const char *username, const char *plainPassword,
                  const char *city, UserRole role);

/**
 * Authenticates a user.
 * Supports both salted hashes (new) and legacy unsalted hashes (old rows).
 * Fills *out on success.
 * Returns true if credentials are valid, false otherwise.
 */
bool user_authenticate(const char *username, const char *plainPassword,
                       User *out);

/** Fetches a user by primary key. Returns true if found. */
bool user_get_by_id(uint64_t id, User *out);

/** Returns true if u is a municipal operator. */
bool user_is_operator(const User *u);

#endif /* USER_H */