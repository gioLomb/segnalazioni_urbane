#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Global database handle
static sqlite3 *db = NULL;

struct DbCursor {
    sqlite3_stmt *stmt;   // Pointer to the underlying SQLite prepared statement
    bool          hasRow; // Flag indicating if the last 'step' found a row
};

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int db_init(const char *path) {
    // If a connection is already open, close it before re-opening
    if (db) sqlite3_close(db);
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",       NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;",     NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-8192;",       NULL, NULL, NULL);

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

/* ── Private Helpers ─────────────────────────────────────────────────── */

// Simple parser to count '?' placeholders to prevent binding mismatches
static int count_placeholders(const char *sql) {
    int n = 0;
    for (; *sql; sql++)
        if (*sql == '?') n++;
    return n;
}

// Internal engine to prepare and bind parameters to a statement
static sqlite3_stmt *prepare(const char *restrict sql, const char *restrict fmt, va_list ap) {
    if (unlikely(!db)) return NULL;

    sqlite3_stmt *stmt = NULL;
    // Compile SQL into a prepared statement
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    // Safety check: ensure number of '?' matches the length of the format string
    int placeholders = count_placeholders(sql);
    int fmtLen      = fmt ? (int)strlen(fmt) : 0;

    if (placeholders != fmtLen) {
        fprintf(stderr, "db: bind error, sql has %d markers but fmt has %d\n", 
                placeholders, fmtLen);
        goto error;
    }

    // Iterate through fmt string and bind variadic arguments to the statement
    for (int i = 0; i < fmtLen; i++) {
        int result = SQLITE_OK;
        int idx = i + 1; // SQLite binding indices start at 1

        switch (fmt[i]) {
            case 's': result = sqlite3_bind_text(stmt, idx, va_arg(ap, char *), -1, SQLITE_TRANSIENT); break;
            case 'i': result = sqlite3_bind_int(stmt, idx, va_arg(ap, int)); break;
            case 'l': result = sqlite3_bind_int64(stmt, idx, va_arg(ap, int64_t)); break;
            case 'f': result = sqlite3_bind_double(stmt, idx, va_arg(ap, double)); break;
            case 'n': result = sqlite3_bind_null(stmt, idx); break;
            default:
                fprintf(stderr, "db: unknown fmt char '%c'\n", fmt[i]);
                goto error;
        }

        if (unlikely(result != SQLITE_OK)) {
            goto error;
        }
    }

    return stmt;

error:
    if (stmt) sqlite3_finalize(stmt);
    return NULL;
}

/* ── Execution ───────────────────────────────────────────────────────── */

int db_exec(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    
    if (!stmt) return -1;

    // Execute the statement. Expecting SQLITE_DONE (no rows returned)
    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (result == SQLITE_DONE) ? 0 : -1;
}

/* ── Cursor ──────────────────────────────────────────────────────────── */


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
    
    cursor->stmt    = stmt;
    cursor->hasRow = false;
    return cursor;
}

bool db_cursor_next(DbCursor *cursor) {
    if (!cursor) return false;
    // Advance SQLite VM. Return true only if a row of data is ready.
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
    // Clean up SQLite statement resouresultes and free cursor wrapper
    sqlite3_finalize(cursor->stmt);
    free(cursor);
}