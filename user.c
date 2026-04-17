#include "user.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Hash semplice djb2 (solo demo, non crittografico)
static void hash_password(const char* plain, char* dest) {
    unsigned long hash = 5381;
    int c;
    while ((c = *plain++))
        hash = ((hash << 5) + hash) + c;
    snprintf(dest, PWD_HASH_LEN, "%016lx", hash);
}

User* user_create(uint64_t id, const char* username, const char* plainPassword, UserRole role) {
    User *u = malloc(sizeof(User));
    if (!u) return NULL;

    u->userId = id;
    u->role = role;

    strncpy(u->username, username, USERNAME_LEN - 1);
    u->username[USERNAME_LEN - 1] = '\0';

    hash_password(plainPassword, u->passwordHash);
    return u;
}

void user_destroy(User* u) {
    free(u);
}

bool user_get_by_id(Hash_Table *userTable, uint64_t id, User *outUser) {
    if (!userTable || !outUser) return false;
    return ht_get(userTable, &id, sizeof(id), outUser, sizeof(User));
}

bool user_verify_password(const User *u, const char *plainPassword) {
    char hash[PWD_HASH_LEN];
    hash_password(plainPassword, hash);
    return strcmp(u->passwordHash, hash) == 0;
}

bool user_is_operator(const User *u) {
    return u && u->role == ROLE_OPERATOR;
}

// Caricamento esempio da file CSV: "id,username,password,role\n"
int user_load_all(Hash_Table *userTable, const char *sourcePath) {
    if (!userTable || !sourcePath) return 0;
    FILE *f = fopen(sourcePath, "r");
    if (!f) return 0;

    char line[256];
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t id;
        char username[USERNAME_LEN];
        char password[PWD_HASH_LEN];
        int roleInt;
        if (sscanf(line, "%lu,%31[^,],%64[^,],%d", &id, username, password, &roleInt) == 4) {
            User *u = user_create(id, username, password, (UserRole)roleInt);
            if (u) {
                ht_set(userTable, &id, sizeof(id), u, sizeof(User));
                free(u); // ht_set ha copiato i byte
                loaded++;
            }
        }
    }
    fclose(f);
    return loaded;
}