#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Module-level database handle; NULL when no connection is open.
static sqlite3 *db = NULL;

// Internal cursor wraps the SQLite prepared statement and a row-ready flag.
struct DbCursor {
    sqlite3_stmt *stmt; // Underlying SQLite prepared statement
    bool hasRow;        // true after a successful sqlite3_step(SQLITE_ROW)
};

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int db_init(const char *path) {
    // Close any pre-existing connection before opening a new one.
    if (db) sqlite3_close(db);
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;

    // WAL mode allows concurrent readers; NORMAL sync balances durability and speed.
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-8192;",   NULL, NULL, NULL);

    return 0;
}

void db_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

const char *db_errmsg(void) {
    return db ? sqlite3_errmsg(db) : "no db";
}

int64_t db_last_insert_id(void) {
    return db ? sqlite3_last_insert_rowid(db) : 0;
}

int db_changes(void) {
    return db ? sqlite3_changes(db) : 0;
}

/* ── Private helpers ─────────────────────────────────────────────────── */

// Counts '?' placeholders in sql to validate against the format string length.
static int count_placeholders(const char *sql) {
    int n = 0;
    for (; *sql; sql++){
        if (*sql == '?') n++;
    }
    return n;
}

// Compiles sql into a prepared statement and binds the variadic arguments
// according to fmt. Returns the statement on success, NULL on any error.
static sqlite3_stmt *prepare(const char *restrict sql,const char *restrict fmt, va_list ap) {
    if (unlikely(!db)) return NULL;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;

    // Guard against mismatches between '?' count and format string length.
    int placeholders = count_placeholders(sql);
    int fmtLen = fmt ? (int)strlen(fmt) : 0;

    if (placeholders != fmtLen) {
        fprintf(stderr, "db: bind mismatch — sql has %d '?' but fmt has %d chars\n",placeholders, fmtLen);
        goto error;
    }

    // Bind each argument in order; SQLite indices are 1-based.
    for (int i = 0; i < fmtLen; i++) {
        int rc = SQLITE_OK;
        int idx = i + 1;

        switch (fmt[i]) {
        case 's':
            rc = sqlite3_bind_text(stmt, idx, va_arg(ap, char *), -1, SQLITE_TRANSIENT);
            break;
        case 'i':
            rc = sqlite3_bind_int(stmt, idx, va_arg(ap, int));
            break;
        case 'l':
            rc = sqlite3_bind_int64(stmt, idx, va_arg(ap, int64_t));
            break;
        case 'f':
            rc = sqlite3_bind_double(stmt, idx, va_arg(ap, double));
            break;
        case 'n':
            rc = sqlite3_bind_null(stmt, idx);
            break;
        default:
            fprintf(stderr, "db: unknown fmt char '%c'\n", fmt[i]);
            goto error;
        }

        if (unlikely(rc != SQLITE_OK)) goto error;
    }

    return stmt;

error:
    if (stmt) sqlite3_finalize(stmt);
    return NULL;
}

/* ── Execution API ───────────────────────────────────────────────────── */

int db_exec(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);

    if (!stmt) return -1;

    // For INSERT/UPDATE/DELETE we expect SQLITE_DONE, not SQLITE_ROW.
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── Cursor API ──────────────────────────────────────────────────────── */

DbCursor *db_cursor_open(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);

    if (!stmt) return NULL;

    DbCursor *cursor = malloc(sizeof(*cursor));
    if (unlikely(!cursor)) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    cursor->stmt = stmt;
    cursor->hasRow = false;
    return cursor;
}

bool db_cursor_next(DbCursor *cursor) {
    if (!cursor) return false;
    // Step the SQLite VM; hasRow is true only when a full data row is ready.
    cursor->hasRow = (sqlite3_step(cursor->stmt) == SQLITE_ROW);
    return cursor->hasRow;
}

const char *db_cursor_text(DbCursor *cursor, int col) {
    if (!cursor || !cursor->hasRow) return "";
    const char *res = (const char *)sqlite3_column_text(cursor->stmt, col);
    return res ? res : "";
}

int64_t db_cursor_int64(DbCursor *cursor, int col) {
    if (!cursor || !cursor->hasRow) return 0;
    return sqlite3_column_int64(cursor->stmt, col);
}

double db_cursor_double(DbCursor *cursor, int col) {
    if (!cursor || !cursor->hasRow) return 0.0;
    return sqlite3_column_double(cursor->stmt, col);
}

void db_cursor_close(DbCursor *cursor) {
    if (!cursor) return;
    // Finalise the compiled statement and release the cursor wrapper.
    sqlite3_finalize(cursor->stmt);
    free(cursor);
}