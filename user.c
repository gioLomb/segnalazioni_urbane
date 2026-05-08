#include "user.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── Password hashing ────────────────────────────────────────────────── */

/*
 * Hash DJB2 adattato: produce sempre 32 caratteri hex (due metà a 64 bit)
 * indipendentemente dall'architettura.
 * NOTA: non è un hash crittografico — adeguato per un progetto didattico,
 * in produzione usare bcrypt / Argon2.
 */
static void hash_password(const char *plain, char *dest) {
    uint64_t h = 5381;
    int c;
    while ((c = *plain++))
        h = ((h << 5) + h) + (unsigned char)c;
    snprintf(dest, PWD_HASH_LEN, "%016llx%016llx",
             (unsigned long long)h,
             (unsigned long long)(h ^ 0xDEADBEEFDEADBEEFULL));
}

/* ── Setup ───────────────────────────────────────────────────────────── */

int user_setup_table(void) {
    return db_exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username      TEXT    UNIQUE NOT NULL,"
        "  password_hash TEXT    NOT NULL,"
        "  role          INTEGER NOT NULL,"
        "  city          TEXT    NOT NULL"
        ");", NULL);
}

/* ── Write operations ────────────────────────────────────────────────── */

int user_register(const char *username, const char *plainPassword,
                  const char *city, UserRole role) {
    char hash[PWD_HASH_LEN];
    hash_password(plainPassword, hash);
    return db_exec(
        "INSERT INTO users (username, password_hash, role, city) VALUES (?,?,?,?);",
        "ssis", username, hash, (int)role, city);
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

static void cursor_to_user(DbCursor *c, User *u) {
    memset(u, 0, sizeof(*u));
    u->userId = (uint64_t)db_cursor_int64(c, 0);
    strncpy(u->username,     db_cursor_text(c, 1), USERNAME_LEN - 1);
    strncpy(u->passwordHash, db_cursor_text(c, 2), PWD_HASH_LEN - 1);
    u->role = (UserRole)db_cursor_int64(c, 3);
    strncpy(u->city,         db_cursor_text(c, 4), CITY_LEN     - 1);
}

/* ── Read operations ─────────────────────────────────────────────────── */

bool user_authenticate(const char *username, const char *plainPassword, User *out) {
    if (!username || !plainPassword || !out) return false;

    DbCursor *c = db_cursor_open(
        "SELECT id, username, password_hash, role, city FROM users WHERE username = ?;",
        "s", username);
    bool found = db_cursor_next(c);
    if (found) cursor_to_user(c, out);
    db_cursor_close(c);

    if (!found) return false;

    char loginHash[PWD_HASH_LEN];
    hash_password(plainPassword, loginHash);
    return strcmp(out->passwordHash, loginHash) == 0;
}

bool user_get_by_id(uint64_t id, User *out) {
    if (!out) return false;
    DbCursor *c = db_cursor_open(
        "SELECT id, username, password_hash, role, city FROM users WHERE id = ?;",
        "l", (int64_t)id);
    bool found = db_cursor_next(c);
    if (found) cursor_to_user(c, out);
    db_cursor_close(c);
    return found;
}

bool user_is_operator(const User *u) {
    return u && u->role == ROLE_OPERATOR;
}