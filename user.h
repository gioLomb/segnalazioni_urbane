#ifndef USER_H
#define USER_H

#include <stdint.h>
#include "hash_table.h"
#include "config.h"

#define USERNAME_LEN 32
#define PWD_HASH_LEN 65 // 64 per SHA256 + 1 per \0

typedef enum { ROLE_CITIZEN = 0, ROLE_OPERATOR = 1 } UserRole;

typedef struct {
    uint64_t userId;
    char username[USERNAME_LEN];
    char passwordHash[PWD_HASH_LEN]; 
    UserRole role;
} User;

// Funzione "Costruttore"
User* user_create(uint64_t id, const char* username, const char* plainPassword, UserRole role);

// Funzione "Distruttore"
void user_destroy(User* u);
/**
 * Cerca un utente nella Hash Table dedicata agli utenti.
 * Ritorna 1 se trovato, 0 altrimenti.
 */
int user_get_by_id(Hash_Table *user_table, uint64_t id, User *dest);

/**
 * Verifica se un utente ha i permessi di operatore.
 */
int user_is_operator(const User *u);

/**
 * (Opzionale) Funzione per caricare utenti da un file di configurazione o DB
 * per popolare la Hash Table all'avvio.
 */
int user_load_all(Hash_Table *userTable, const char *sourcePath);

#endif