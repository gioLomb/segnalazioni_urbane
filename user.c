#include "user.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

/* ── Salt generation ─────────────────────────────────────────────────── */

/*
 * Fills out[0..SALT_HEX_LEN-1] with 32 cryptographically random hex chars
 * using /dev/urandom.  Falls back to time^pid if the device is unavailable.
 */
static void generate_salt(char *out) {
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[16];  /* 16 raw bytes → 32 hex chars */

    FILE *f = fopen("/dev/urandom", "rb");
    if (f && fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
        fclose(f);
        for (int i = 0; i < 16; i++) {
            out[i * 2]     = hex[buf[i] >> 4];
            out[i * 2 + 1] = hex[buf[i] & 0x0F];
        }
        out[32] = '\0';
        return;
    }
    if (f) fclose(f);

    /* Fallback — not cryptographically secure, acceptable for dev environments. */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 32; i++)
        out[i] = hex[rand() % 16];
    out[32] = '\0';
}

/* ── Password hashing ────────────────────────────────────────────────── */

/*
 * DJB2-derived hash of (salt + plain).
 * Produces exactly 64 hex chars (two 64-bit halves) into dest[PWD_HASH_LEN].
 *
 * NOTE: this is not a cryptographic KDF — for production use bcrypt/Argon2.
 * The salt eliminates rainbow-table and pre-computation attacks even with
 * this fast hash.
 */
static void hash_password(const char *salt, const char *plain, char *dest) {
    uint64_t h = 5381;
    /* Hash salt first, then password — same as hash(salt || plain). */
    for (const char *p = salt;  *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    for (const char *p = plain; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
    snprintf(dest, PWD_HASH_LEN, "%016llx%016llx",
             (unsigned long long)h,
             (unsigned long long)(h ^ 0xDEADBEEFDEADBEEFULL));
}

/* ── Constant-time comparison ────────────────────────────────────────── */

static int constant_time_compare(const char *a, const char *b) {
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    int result = (len_a != len_b);
    for (size_t i = 0; i < len_a && i < len_b; i++)
        result |= (a[i] ^ b[i]);
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
    char salt[SALT_HEX_LEN];
    generate_salt(salt);

    char hash[PWD_HASH_LEN];
    hash_password(salt, plainPassword, hash);

    return db_exec(
        "INSERT INTO users (username, password_hash, role, city, salt)"
        " VALUES (?,?,?,?,?);",
        "ssiss", username, hash, (int)role, city, salt);
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

/*
 * SELECT column order:
 *   0=id  1=username  2=password_hash  3=role  4=city  5=salt
 */
static void cursor_to_user(DbCursor *c, User *u) {
    memset(u, 0, sizeof(*u));
    u->userId = (uint64_t)db_cursor_int64(c, USER_COL_ID);
    strncpy(u->username,     db_cursor_text(c, USER_COL_USERNAME),
            USERNAME_LEN - 1);
    strncpy(u->passwordHash, db_cursor_text(c, USER_COL_PASSWORD_HASH),
            PWD_HASH_LEN - 1);
    u->role = (UserRole)db_cursor_int64(c, USER_COL_ROLE);
    strncpy(u->city, db_cursor_text(c, USER_COL_CITY), CITY_LEN - 1);
    /* salt may be NULL in legacy rows — db_cursor_text returns "" in that case */
    strncpy(u->salt, db_cursor_text(c, USER_COL_SALT), SALT_HEX_LEN - 1);
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

    char login_hash[PWD_HASH_LEN];

    if (out->salt[0] != '\0') {
        /* New path: salted hash */
        hash_password(out->salt, plainPassword, login_hash);
    } else {
        /*
         * Legacy path: unsalted hash for rows registered before the salt
         * migration.  These will keep working until the user re-registers
         * or we force a password reset.
         */
        hash_password("", plainPassword, login_hash);
    }

    return constant_time_compare(out->passwordHash, login_hash);
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