#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stdbool.h>
#include "hash_table.h"

#define USERNAME_LEN 32
#define PWD_HASH_LEN 65   // 64 hex + null
#define CITY_LEN 32

typedef enum {
    ROLE_CITIZEN = 0,
    ROLE_OPERATOR = 1
} UserRole;

typedef struct {
    uint64_t userId;                     // chiave primaria
    char city[CITY_LEN];
    char username[USERNAME_LEN];
    char passwordHash[PWD_HASH_LEN];
    UserRole role;
} User;

// Costruttore/distruttore
User* user_create(uint64_t id, const char* username, const char* plainPassword, UserRole role);
void user_destroy(User* u);

// Lookup (usa la hash table degli utenti)
bool user_get_by_id(Hash_Table *userTable, uint64_t id, User *outUser);
bool user_verify_password(const User *u, const char *plainPassword);
bool user_is_operator(const User *u);

// Caricamento da file CSV o testo all'avvio (opzionale)
int user_load_all(Hash_Table *userTable, const char *sourcePath);

#endif