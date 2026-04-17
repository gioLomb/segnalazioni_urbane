#include "session.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Generatore di token casuali (usa rand() + time, non crittografico ma sufficiente per demo)
static void generate_token(char *out, size_t len) {
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char r = (unsigned char)(rand() & 0xFF);
        out[i] = hex[r % 16];
    }
    out[len] = '\0';
}

bool session_create(Hash_Table *sessionTable, uint64_t userId, char *outToken) {
    if (!sessionTable || !outToken) return false;

    // Genera token finché non trova una chiave non collisionata
    char token[TOKEN_HEX_LEN + 1];
    uint64_t dummy;
    int max_tries = 100;
    do {
        generate_token(token, TOKEN_HEX_LEN);
        if (max_tries-- <= 0) return false;
    } while (session_verify(sessionTable, token, &dummy));

    // Inserisce nella hash table: chiave = token (stringa), valore = userId
    if (!ht_set(sessionTable, token, strlen(token) + 1, &userId, sizeof(userId)))
        return false;

    strcpy(outToken, token);
    return true;
}

bool session_verify(Hash_Table *sessionTable, const char *token, uint64_t *outUserId) {
    if (!sessionTable || !token) return false;
    return ht_get(sessionTable, (void *)token, strlen(token) + 1, outUserId, sizeof(uint64_t));
}

void session_destroy(Hash_Table *sessionTable, const char *token) {
    if (sessionTable && token) {
        ht_delete(sessionTable, (void *)token, strlen(token) + 1);
    }
}

void session_clear_all(Hash_Table *sessionTable) {
    // TODO: eliminare la table in debug...
    (void)sessionTable;
}