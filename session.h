#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "hash_table.h"

// Genera un token casuale (esadecimale) di lunghezza TOKEN_LEN (senza null)
#define TOKEN_HEX_LEN 32   // 32 caratteri esadecimali = 128 bit

// Crea una nuova sessione: genera token, lo associa a userId nella hash table.
// Restituisce true e copia il token in outToken (buffer di almeno TOKEN_HEX_LEN+1 byte)
bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken);

// Verifica se un token è valido e restituisce l'userId associato.
// Restituisce true se trovato, false altrimenti.
bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId);

// Elimina una sessione (logout).
void session_destroy(Hash_Table *sessionTable, const char *token);

// Pulisce tutte le sessioni (utile per debug o shutdown).
void session_clear_all(Hash_Table *sessionTable);

#endif