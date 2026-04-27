#include "user.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Funzione interna di hashing (DJB2 adattato)
#include <stdint.h>

static void hash_password(const char *plain, char *dest) {
    uint64_t hash = 5381;
    int c;
    while ((c = *plain++))
        hash = ((hash << 5) + hash) + (unsigned char)c;

    /* Two fixed-width 64-bit halves → always 32 hex chars, independent of arch */
    snprintf(dest, PWD_HASH_LEN, "%016llx%016llx",
             (unsigned long long)hash,
             (unsigned long long)(hash ^ 0xDEADBEEFDEADBEEFULL));
}

// Callback interna per mappare una riga SQLite sulla struct User
static int user_row_callback(sqlite3_stmt *stmt, void *userdata) {
    User *u = (User *)userdata;
    if (!u) return 0;

    u->userId = (uint64_t)sqlite3_column_int64(stmt, 0);
    
    const char *uname = (const char *)sqlite3_column_text(stmt, 1);
    if (uname) strncpy(u->username, uname, USERNAME_LEN - 1);
    
    const char *hash = (const char *)sqlite3_column_text(stmt, 2);
    if (hash) strncpy(u->passwordHash, hash, PWD_HASH_LEN - 1);
    
    u->role = (UserRole)sqlite3_column_int(stmt, 3);
    
    const char *city = (const char *)sqlite3_column_text(stmt, 4);
    if (city) strncpy(u->city, city, CITY_LEN - 1);

    return 0; // Trovato (interrompe la query dopo la prima riga)
}

int user_setup_table() {
    const char *sql = "CREATE TABLE IF NOT EXISTS users ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "username TEXT UNIQUE NOT NULL,"
                      "password_hash TEXT NOT NULL,"
                      "role INTEGER NOT NULL,"
                      "city TEXT NOT NULL);";
    return db_execute(sql, 0); // 0 indica fine parametri variadici
}

int user_register(const char *username, const char *plainPassword, const char *city, UserRole role) {
    char hash[PWD_HASH_LEN];
    hash_password(plainPassword, hash);

    const char *sql = "INSERT INTO users (username, password_hash, role, city) VALUES (?, ?, ?, ?);";
    
    // Tipi: 4=string, 1=int, 0=fine
    return db_execute(sql, 4, username, 4, hash, 1, (int)role, 4, city, 0);
}

bool user_authenticate(const char *username, const char *plainPassword, User *outUser) {
    if (!username || !plainPassword || !outUser) return false;

    // 1. Cerchiamo l'utente per username
    const char *sql = "SELECT id, username, password_hash, role, city FROM users WHERE username = ?;";
    int rows = db_query(sql, user_row_callback, outUser, 4, username, 0);

    if (rows <= 0) return false; // Utente non trovato

    // 2. Verifichiamo la password
    char loginHash[PWD_HASH_LEN];
    hash_password(plainPassword, loginHash);

    if (strcmp(outUser->passwordHash, loginHash) == 0) {
        return true;
    }

    return false;
}

bool user_get_by_id(uint64_t id, User *outUser) {
    if (!outUser) return false;

    const char *sql = "SELECT id, username, password_hash, role, city FROM users WHERE id = ?;";
    // Tipo 2 = int64
    int rows = db_query(sql, user_row_callback, outUser, 2, (sqlite3_int64)id, 0);

    return rows > 0;
}

bool user_is_operator(const User *u) {
    return u && u->role == ROLE_OPERATOR;
}