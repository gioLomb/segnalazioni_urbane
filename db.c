#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static sqlite3 *g_db = NULL;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int db_init(const char *path) {
    if (g_db) sqlite3_close(g_db);
    if (sqlite3_open(path, &g_db) != SQLITE_OK) return -1;
    return 0;
}

void        db_close         (void) { if (g_db) { sqlite3_close(g_db); g_db = NULL; } }
const char *db_errmsg        (void) { return g_db ? sqlite3_errmsg(g_db) : "no db"; }
int64_t     db_last_insert_id(void) { return g_db ? sqlite3_last_insert_rowid(g_db) : 0; }
int         db_changes       (void) { return g_db ? sqlite3_changes(g_db) : 0; }

/* ── Binding ─────────────────────────────────────────────────────────── */

static int bind_fmt(sqlite3_stmt *stmt, const char *fmt, va_list ap) {
    if (!fmt) return 0;
    for (int i = 0; fmt[i]; i++) {
        int col = i + 1;
        int rc;
        switch (fmt[i]) {
            case 's': rc = sqlite3_bind_text  (stmt, col, va_arg(ap, const char *), -1, SQLITE_TRANSIENT); break;
            case 'i': rc = sqlite3_bind_int   (stmt, col, va_arg(ap, int));                                break;
            case 'l': rc = sqlite3_bind_int64 (stmt, col, va_arg(ap, int64_t));                            break;
            case 'f': rc = sqlite3_bind_double(stmt, col, va_arg(ap, double));                             break;
            case 'n': rc = sqlite3_bind_null  (stmt, col);                                                 break;
            default:
                fprintf(stderr, "db: unknown fmt char '%c'\n", fmt[i]);
                return -1;
        }
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db: bind failed at col %d: %s\n", col, sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return -1;
        }
    }
    return 0;
}

static sqlite3_stmt *prepare(const char *sql, const char *fmt, va_list ap) {
    if (!g_db) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    if (bind_fmt(stmt, fmt, ap) != 0) { sqlite3_finalize(stmt); return NULL; }
    return stmt;
}

/* ── db_exec ─────────────────────────────────────────────────────────── */

int db_exec(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    if (!stmt) return -1;

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── Cursore ─────────────────────────────────────────────────────────── */

struct DbCursor {
    sqlite3_stmt *stmt;
    bool          has_row;
};

DbCursor *db_cursor_open(const char *sql, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    if (!stmt) return NULL;

    DbCursor *c = malloc(sizeof(*c));
    if (!c) { sqlite3_finalize(stmt); return NULL; }
    c->stmt    = stmt;
    c->has_row = false;
    return c;
}

bool db_cursor_next(DbCursor *c) {
    if (!c) return false;
    c->has_row = (sqlite3_step(c->stmt) == SQLITE_ROW);
    return c->has_row;
}

const char *db_cursor_text(DbCursor *c, int col) {
    if (!c || !c->has_row) return "";
    const char *s = (const char *)sqlite3_column_text(c->stmt, col);
    return s ? s : "";
}

int64_t db_cursor_int64(DbCursor *c, int col) {
    return (c && c->has_row) ? sqlite3_column_int64(c->stmt, col) : 0;
}

double db_cursor_double(DbCursor *c, int col) {
    return (c && c->has_row) ? sqlite3_column_double(c->stmt, col) : 0.0;
}

void db_cursor_close(DbCursor *c) {
    if (!c) return;
    sqlite3_finalize(c->stmt);
    free(c);
}
