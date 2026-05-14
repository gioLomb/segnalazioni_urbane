#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Global database handle
static sqlite3 *g_db = NULL;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int db_init(const char *path) {
    // If a connection is already open, close it before re-opening
    if (g_db) sqlite3_close(g_db);
    if (sqlite3_open(path, &g_db) != SQLITE_OK) return -1;

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",       NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;",     NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA cache_size=-8192;",       NULL, NULL, NULL);

    return 0;
}

void db_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

const char *db_errmsg(void) {
    return g_db ? sqlite3_errmsg(g_db) : "no db";
}

int64_t db_last_insert_id(void) {
    return g_db ? sqlite3_last_insert_rowid(g_db) : 0;
}

int db_changes(void) {
    return g_db ? sqlite3_changes(g_db) : 0;
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
static sqlite3_stmt *prepare(const char *sql, const char *fmt, va_list ap) {
    if (unlikely(!g_db)) return NULL;

    sqlite3_stmt *stmt;
    // Compile SQL into a prepared statement
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }

    // Safety check: ensure number of '?' matches the length of the format string
    int placeholders = count_placeholders(sql);
    int fmt_len      = fmt ? (int)strlen(fmt) : 0;

    if (placeholders != fmt_len) {
        fprintf(stderr, "db: bind error, sql has %d markers but fmt has %d\n", 
                placeholders, fmt_len);
        sqlite3_finalize(stmt);
        return NULL;
    }

    // Iterate through fmt string and bind variadic arguments to the statement
    for (int i = 0; i < fmt_len; i++) {
        int rc = SQLITE_OK;
        int idx = i + 1; // SQLite binding indices start at 1

        switch (fmt[i]) {
            case 's': rc = sqlite3_bind_text(stmt, idx, va_arg(ap, char *), -1, SQLITE_TRANSIENT); break;
            case 'i': rc = sqlite3_bind_int(stmt, idx, va_arg(ap, int)); break;
            case 'l': rc = sqlite3_bind_int64(stmt, idx, va_arg(ap, int64_t)); break;
            case 'f': rc = sqlite3_bind_double(stmt, idx, va_arg(ap, double)); break;
            case 'n': rc = sqlite3_bind_null(stmt, idx); break;
            default:
                fprintf(stderr, "db: unknown fmt char '%c'\n", fmt[i]);
                sqlite3_finalize(stmt);
                return NULL;
        }

        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    return stmt;
}

/* ── Execution ───────────────────────────────────────────────────────── */

int db_exec(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    
    if (!stmt) return -1;

    // Execute the statement. Expecting SQLITE_DONE (no rows returned)
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── Cursor ──────────────────────────────────────────────────────────── */

struct DbCursor {
    sqlite3_stmt *stmt;   // Pointer to the underlying SQLite prepared statement
    bool          has_row; // Flag indicating if the last 'step' found a row
};

DbCursor *db_cursor_open(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    
    if (!stmt) return NULL;

    DbCursor *c = malloc(sizeof(*c));
    if (unlikely(!c)) { 
        sqlite3_finalize(stmt); 
        return NULL; 
    }
    
    c->stmt    = stmt;
    c->has_row = false;
    return c;
}

bool db_cursor_next(DbCursor *c) {
    if (!c) return false;
    // Advance SQLite VM. Return true only if a row of data is ready.
    c->has_row = (sqlite3_step(c->stmt) == SQLITE_ROW);
    return c->has_row;
}

const char *db_cursor_text(DbCursor *c, int col) {
    if (!c || !c->has_row) return "";
    const char *res = (const char *)sqlite3_column_text(c->stmt, col);
    return res ? res : "";
}

int64_t db_cursor_int64(DbCursor *c, int col) {
    if (!c || !c->has_row) return 0;
    return sqlite3_column_int64(c->stmt, col);
}

double db_cursor_double(DbCursor *c, int col) {
    if (!c || !c->has_row) return 0.0;
    return sqlite3_column_double(c->stmt, col);
}

void db_cursor_close(DbCursor *c) {
    if (!c) return;
    // Clean up SQLite statement resources and free cursor wrapper
    sqlite3_finalize(c->stmt);
    free(c);
}