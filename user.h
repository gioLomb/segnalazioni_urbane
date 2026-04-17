#ifndef USER_H
#define USER_H

#include <stdint.h>
#include <stdbool.h>

#define USERNAME_LEN 32
#define PWD_HASH_LEN 65   // 64 caratteri hex + null terminator
#define CITY_LEN 32

// Definizione dei ruoli
typedef enum {
    ROLE_CITIZEN = 0,
    ROLE_OPERATOR = 1
} UserRole;

// Struttura Utente Unica
typedef struct {
    uint64_t userId;
    char username[USERNAME_LEN];
    char passwordHash[PWD_HASH_LEN];
    char city[CITY_LEN];
    UserRole role;
} User;

/**
 * Crea la tabella 'users' nel database se non esiste.
 * Ritorna 0 in caso di successo, -1 altrimenti.
 */
int user_setup_table();

/**
 * Registra un nuovo utente nel database.
 * La password viene hashata automaticamente prima del salvataggio.
 */
int user_register(const char *username, const char *plainPassword, const char *city, UserRole role);

/**
 * Autentica un utente verificando username e password sul database.
 * Se l'autenticazione ha successo, riempie la struct 'outUser'.
 * Ritorna true se autenticato, false altrimenti.
 */
bool user_authenticate(const char *username, const char *plainPassword, User *outUser);

/**
 * Recupera un utente dal database tramite il suo ID.
 * Utile per le operazioni post-login (es. verifica sessione).
 */
bool user_get_by_id(uint64_t id, User *outUser);

/**
 * Utility per controllare se l'utente è un operatore.
 */
bool user_is_operator(const User *u);

#endif