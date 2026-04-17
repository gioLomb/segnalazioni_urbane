#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_db = NULL;
static const char *g_errmsg = NULL;

int db_init(const char *path) {
    if (g_db) sqlite3_close(g_db);
    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        g_errmsg = sqlite3_errmsg(g_db);
        return -1;
    }
    return 0;
}

void db_close(void) {
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
}

const char *db_errmsg(void) {
    return g_errmsg ? g_errmsg : sqlite3_errmsg(g_db);
}

static int bind_variadic(sqlite3_stmt *stmt, va_list args) {
    int idx = 1;
    while (1) {
        int type = va_arg(args, int); // tipo: 0=fine, 1=int, 2=int64, 3=double, 4=string, 5=null
        if (type == 0) break;
        switch (type) {
            case 1: { int v = va_arg(args, int); sqlite3_bind_int(stmt, idx++, v); break; }
            case 2: { sqlite3_int64 v = va_arg(args, sqlite3_int64); sqlite3_bind_int64(stmt, idx++, v); break; }
            case 3: { double v = va_arg(args, double); sqlite3_bind_double(stmt, idx++, v); break; }
            case 4: { char *v = va_arg(args, char*); sqlite3_bind_text(stmt, idx++, v, -1, SQLITE_STATIC); break; }
            case 5: sqlite3_bind_null(stmt, idx++); break;
            default: return -1;
        }
    }
    return 0;
}

int db_execute(const char *sql, ...) {
    if (!g_db) return -1;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_errmsg = (char*)sqlite3_errmsg(g_db);
        return -1;
    }
    va_list args;
    va_start(args, sql);
    bind_variadic(stmt, args);
    va_end(args);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        g_errmsg = (char*)sqlite3_errmsg(g_db);
        return -1;
    }
    return 0;
}

int db_query_va(const char *sql, db_row_callback callback, void *userdata, va_list args) {
    if (!g_db || !callback) return -1;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_errmsg = (char*)sqlite3_errmsg(g_db);
        return -1;
    }
    bind_variadic(stmt, args);
    int row_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (callback(stmt, userdata) != 0) break; // callback può fermare
        row_count++;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        g_errmsg = (char*)sqlite3_errmsg(g_db);
        return -1;
    }
    return row_count;
}

int db_query(const char *sql, db_row_callback callback, void *userdata, ...) {
    va_list args;
    va_start(args, userdata);
    int ret = db_query_va(sql, callback, userdata, args);
    va_end(args);
    return ret;
}