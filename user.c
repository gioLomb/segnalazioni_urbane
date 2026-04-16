#include "user.h"

#include "user.h"
#include <string.h>
#include <stdio.h>

static void hash_password(const char* plain, char* dest) {
    unsigned long hash = 5381;
    int c;
    while ((c = *plain++)) hash = ((hash << 5) + hash) + c;
    snprintf(dest, PWD_HASH_LEN, "%016lx", hash);
}

User* user_create(uint64_t id, const char* username, const char* plainPassword, UserRole role) {
    
    User* newUser = (User*)malloc(sizeof(User));
    if (!newUser) return NULL;

    newUser->userId = id;
    newUser->role = role;

    strncpy(newUser->username, username, USERNAME_LEN - 1);
    newUser->username[USERNAME_LEN - 1] = '\0';

    // Hashiamo la password prima di salvarla nell'oggetto appena allocato
    hash_password(plainPassword, newUser->passwordHash);

    return newUser;
}

void user_destroy(User* u) {
    if (u) {
        free(u);
    }
}

