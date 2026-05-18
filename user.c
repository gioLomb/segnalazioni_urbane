#include "user.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

/* ── Salt generation ─────────────────────────────────────────────────── */

// Fills out[0..SALT_HEX_LEN-1] with 32 cryptographically random hex chars
// using /dev/urandom. Falls back to time^pid if the device is unavailable.
static void generate_salt(char *out) {
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[16];  // 16 raw bytes → 32 hex chars

    FILE *f = fopen("/dev/urandom", "rb");
    if (f && fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
        fclose(f);
        for (int i = 0; i < 16; i++) {
            out[i * 2] = hex[buf[i] >> 4];
            out[i * 2 + 1] = hex[buf[i] & 0x0F];
        }
        out[32] = '\0';
        return;
    }
    if (f) fclose(f);

    // Fallback: not cryptographically secure, acceptable for dev environments.
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 32; i++)
        out[i] = hex[rand() % 16];
    out[32] = '\0';
}

/* ── Password hashing ────────────────────────────────────────────────── */

// DJB2-derived hash of (salt + plain), producing 64 hex chars into dest.
// NOTE: not a cryptographic KDF — for production use bcrypt or Argon2.
// The random salt eliminates rainbow-table and pre-computation attacks
// even with this fast hash.
static void hash_password(const char *salt, const char *plain, char *dest) {
    uint64_t h = 5381;
    // Hash salt first, then password — equivalent to hash(salt || plain).
    for (const char *p = salt;  *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    for (const char *p = plain; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    snprintf(dest, PWD_HASH_LEN + 1, "%016llx%016llx",
             (unsigned long long)h,
             (unsigned long long)(h ^ 0xDEADBEEFDEADBEEFULL));
}

/* ── Constant-time comparison ────────────────────────────────────────── */

// XORs every byte of two PWD_HASH_LEN strings and checks that the result
// is zero. The volatile prevents the compiler from short-circuiting the loop,
// which would reintroduce a timing side-channel.
static int constant_time_compare(const char *a, const char *b) {
    volatile int result = 0;
    for (size_t i = 0; i < PWD_HASH_LEN; i++)
        result |= ((unsigned char)a[i] ^ (unsigned char)b[i]);
    return result == 0;
}

/* ── Setup ───────────────────────────────────────────────────────────── */

int user_setup_table(void) {
    return db_exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username      TEXT    UNIQUE NOT NULL,"
        "  password_hash TEXT    NOT NULL,"
        "  role          INTEGER NOT NULL,"
        "  city          TEXT    NOT NULL,"
        "  salt          TEXT    NOT NULL"
        ");", NULL);
}

/* ── Write operations ────────────────────────────────────────────────── */

int user_register(const char *username, const char *plainPassword,
                  const char *city, UserRole role) {
    char salt[SALT_HEX_LEN + 1];
    generate_salt(salt);

    char hash[PWD_HASH_LEN + 1];
    hash_password(salt, plainPassword, hash);

    return db_exec(
        "INSERT INTO users (username, password_hash, role, city, salt)"
        " VALUES (?,?,?,?,?);",
        "ssiss", username, hash, (int)role, city, salt);
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

// Maps the current cursor row to a User.
// SELECT column order: 0=id 1=username 2=password_hash 3=role 4=city 5=salt
static void cursor_to_user(DbCursor *c, User *u) {
    memset(u, 0, sizeof(*u));
    u->userId = (uint64_t)db_cursor_int64(c, USER_COL_ID);
    strncpy(u->username, db_cursor_text(c, USER_COL_USERNAME), USERNAME_LEN - 1);
    strncpy(u->passwordHash, db_cursor_text(c, USER_COL_PASSWORD_HASH), PWD_HASH_LEN);
    u->role = (UserRole)db_cursor_int64(c, USER_COL_ROLE);
    strncpy(u->city, db_cursor_text(c, USER_COL_CITY), CITY_LEN - 1);
    // db_cursor_text returns "" for NULL columns, so legacy rows with no
    // salt produce an empty string rather than undefined behaviour.
    strncpy(u->salt, db_cursor_text(c, USER_COL_SALT), SALT_HEX_LEN);
}

/* ── Read operations ─────────────────────────────────────────────────── */

bool user_authenticate(const char *username, const char *plainPassword,
                       User *out) {
    if (!username || !plainPassword || !out) return false;

    DbCursor *c = db_cursor_open(
        "SELECT id, username, password_hash, role, city, salt"
        " FROM users WHERE username = ?;",
        "s", username);
    bool found = db_cursor_next(c);
    if (found) cursor_to_user(c, out);
    db_cursor_close(c);

    if (!found) return false;

    char loginHash[PWD_HASH_LEN + 1];

    if (out->salt[0] != '\0') {
        // Salted path: current schema.
        hash_password(out->salt, plainPassword, loginHash);
    } else {
        // Legacy path: rows created before the salt column was added.
        hash_password("", plainPassword, loginHash);
    }

    return constant_time_compare(out->passwordHash, loginHash);
}

bool user_get_by_id(uint64_t id, User *out) {
    if (!out) return false;
    DbCursor *c = db_cursor_open(
        "SELECT id, username, password_hash, role, city, salt"
        " FROM users WHERE id = ?;",
        "l", (int64_t)id);
    bool found = db_cursor_next(c);
    if (found) cursor_to_user(c, out);
    db_cursor_close(c);
    return found;
}

bool user_is_operator(const User *u) {
    return u && u->role == ROLE_OPERATOR;
}

bool user_is_admin(const User *u) {
    return u && u->role == ROLE_ADMIN;
}

int user_register_admin(const char *username, const char *plainPassword,
                        const char *city) {
    // Enforce the one-admin-per-city constraint before inserting.
    DbCursor *c = db_cursor_open(
        "SELECT id FROM users WHERE role = 2 AND city = ? LIMIT 1;",
        "s", city);
    bool exists = db_cursor_next(c);
    db_cursor_close(c);
    if (exists) return -2;  // an admin already exists for this city

    return user_register(username, plainPassword, city, ROLE_ADMIN);
}

size_t user_get_operators_json(char *buf, size_t max, const char *city) {
    DbCursor *c = db_cursor_open(
        "SELECT id, username FROM users WHERE role = 1 AND city = ? ORDER BY username;",
        "s", city);

    if (!c) {
        if (max > 2) {
            strcpy(buf, "[]");
            return 2;
        }
        return 0;
    }

    // Build the JSON array by iterating the cursor.
    cJSON *root = cJSON_CreateArray();
    while (db_cursor_next(c)) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double)db_cursor_int64(c, 0));
        cJSON_AddStringToObject(item, "username", db_cursor_text(c, 1));
        cJSON_AddItemToArray(root, item);
    }
    db_cursor_close(c);

    // Write directly into the caller's buffer; the 0 flag means compact
    // (no indentation), which keeps the payload as small as possible.
    bool ok = cJSON_PrintPreallocated(root, buf, (int)max, 0);
    cJSON_Delete(root);

    return ok ? strlen(buf) : 0;
}