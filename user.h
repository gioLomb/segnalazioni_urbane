#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stdbool.h>

#define USERNAME_LEN 32
#define PWD_HASH_LEN 65   // 64 caratteri hex + null terminator
#define CITY_LEN 32

typedef enum {
    ROLE_CITIZEN = 0,
    ROLE_OPERATOR = 1
} UserRole;

/* Colonne della tabella users (ordine fisso) */
typedef enum {
    USER_COL_ID = 0,
    USER_COL_USERNAME,
    USER_COL_PASSWORD_HASH,
    USER_COL_ROLE,
    USER_COL_CITY
} UserCol;

typedef struct {
    uint64_t userId;
    char username[USERNAME_LEN];
    char passwordHash[PWD_HASH_LEN];
    char city[CITY_LEN];
    UserRole role;
} User;

int user_setup_table();

int user_register(const char *username, const char *plainPassword, const char *city, UserRole role);

bool user_authenticate(const char *username, const char *plainPassword, User *outUser);

bool user_get_by_id(uint64_t id, User *outUser);

bool user_is_operator(const User *u);

#endif
