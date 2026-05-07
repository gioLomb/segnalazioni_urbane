#include "report.h"
#include "db.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sqlite3.h>

#define SELECT_COLS \
    "id, author_id, assigned_to, lat, lon, city, category, description, " \
    "status, created_at, assigned_at, resolved_at"

/* ── Setup ───────────────────────────────────────────────────────────── */

int report_setup_table(void) {
    if (db_execute(
            "CREATE TABLE IF NOT EXISTS reports ("
            "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  author_id   INTEGER NOT NULL,"
            "  assigned_to INTEGER,"
            "  lat         REAL    NOT NULL DEFAULT 0,"
            "  lon         REAL    NOT NULL DEFAULT 0,"
            "  city        TEXT    NOT NULL DEFAULT '',"
            "  category    TEXT    NOT NULL DEFAULT '',"
            "  description TEXT    NOT NULL,"
            "  status      INTEGER NOT NULL DEFAULT 0,"
            "  created_at  INTEGER NOT NULL,"
            "  assigned_at INTEGER,"
            "  resolved_at INTEGER"
            ");", 0) != 0)
        return -1;

    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_author ON reports(author_id);", 0);
    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_city   ON reports(city);",      0);
    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(status);",    0);
    return 0;
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

static void row_to_report(sqlite3_stmt *stmt, ActiveReport *r) {
    memset(r, 0, sizeof(*r));
    r->reportId   = (uint64_t)sqlite3_column_int64(stmt, 0);
    r->authorId   = (uint64_t)sqlite3_column_int64(stmt, 1);
    r->assignedTo = (uint64_t)sqlite3_column_int64(stmt, 2);
    r->lat        = sqlite3_column_double(stmt, 3);
    r->lon        = sqlite3_column_double(stmt, 4);

    const char *city = (const char *)sqlite3_column_text(stmt, 5);
    const char *cat  = (const char *)sqlite3_column_text(stmt, 6);
    const char *desc = (const char *)sqlite3_column_text(stmt, 7);
    strncpy(r->city,        city ? city : "", CITY_LEN - 1);
    strncpy(r->category,    cat  ? cat  : "", CAT_LEN  - 1);
    strncpy(r->description, desc ? desc : "", DESC_LEN - 1);

    r->status     = (ReportStatus)sqlite3_column_int(stmt, 8);
    r->createdAt  = (time_t)sqlite3_column_int64(stmt, 9);
    r->assignedAt = (time_t)sqlite3_column_int64(stmt, 10);
    r->resolvedAt = (time_t)sqlite3_column_int64(stmt, 11);
}

/* ── JSON builder ────────────────────────────────────────────────────── */

/* Converte un ActiveReport in un oggetto cJSON.
   cJSON_CreateString gestisce automaticamente l'escaping
   di \n, \r, \t, ", \ e altri caratteri speciali. */
static cJSON *report_to_cjson(const ActiveReport *r) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id",          (double)r->reportId);
    cJSON_AddNumberToObject(obj, "author_id",   (double)r->authorId);
    cJSON_AddNumberToObject(obj, "lat",         r->lat);
    cJSON_AddNumberToObject(obj, "lon",         r->lon);
    cJSON_AddStringToObject(obj, "city",        r->city);
    cJSON_AddStringToObject(obj, "category",    r->category);
    cJSON_AddStringToObject(obj, "description", r->description);  /* escaped */
    cJSON_AddNumberToObject(obj, "status",      (double)r->status);
    cJSON_AddNumberToObject(obj, "created_at",  (double)r->createdAt);

    return obj;
}

/* ── Query callbacks ─────────────────────────────────────────────────── */

static int json_array_cb(sqlite3_stmt *stmt, void *userdata) {
    cJSON *array = (cJSON *)userdata;
    ActiveReport r;
    row_to_report(stmt, &r);
    cJSON *obj = report_to_cjson(&r);
    if (!obj) return -1;
    cJSON_AddItemToArray(array, obj);
    return 0;
}

static int single_row_cb(sqlite3_stmt *stmt, void *userdata) {
    row_to_report(stmt, (ActiveReport *)userdata);
    return 0;
}

static int count_cb(sqlite3_stmt *stmt, void *userdata) {
    *(int *)userdata = sqlite3_column_int(stmt, 0);
    return 0;
}

/* ── Helpers per le query che restituiscono JSON ─────────────────────── */

/* Esegue query, raccoglie le righe in un array cJSON,
   lo serializza e restituisce la stringa heap-allocated. */
static char *query_to_json(const char *sql, ...) {
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;

    /* Passiamo l'array come userdata al callback */
    /* Nota: db_query accetta variadic, qui usiamo una versione semplificata
       — adatta la chiamata alla firma reale di db_query del tuo progetto */
    // db_query(sql, json_array_cb, array, ...);   ← chiamata nel sito d'uso

    char *out = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return out;  /* può essere NULL se OOM */
}

/* ── Write operations ────────────────────────────────────────────────── */

uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc) {
    int rc = db_execute(
        "INSERT INTO reports "
        "(author_id, lat, lon, city, category, description, status, created_at) "
        "VALUES (?,?,?,?,?,?,0,?);",
        2, (sqlite3_int64)authorId,
        3, lat,
        3, lon,
        4, (char *)city,
        4, (char *)category,
        4, (char *)desc,
        2, (sqlite3_int64)time(NULL),
        0);
    if (rc != 0) return 0;
    sqlite3_int64 id = db_last_insert_id();
    return (id > 0) ? (uint64_t)id : 0;
}

int report_assign(uint64_t reportId, uint64_t operatorId) {
    int rc = db_execute(
        "UPDATE reports "
        "SET status = 1, assigned_to = ?, assigned_at = ? "
        "WHERE id = ? AND status = 0 AND assigned_to IS NULL;",
        2, (sqlite3_int64)operatorId,
        2, (sqlite3_int64)time(NULL),
        2, (sqlite3_int64)reportId,
        0);
    if (rc != 0) return -1;
    return db_changes() > 0 ? 1 : 0;
}

int report_resolve(uint64_t reportId, uint64_t operatorId) {
    int rc = db_execute(
        "UPDATE reports "
        "SET status = 2, resolved_at = ? "
        "WHERE id = ? AND assigned_to = ? AND status = 1;",
        2, (sqlite3_int64)time(NULL),
        2, (sqlite3_int64)reportId,
        2, (sqlite3_int64)operatorId,
        0);
    if (rc != 0) return -1;
    return db_changes() > 0 ? 1 : 0;
}

/* ── Read operations ─────────────────────────────────────────────────── */

char *report_get_active_json(uint64_t userId, const char *city, bool isOperator) {
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;

    if (isOperator) {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE city = ? AND (status = 0 OR (status = 1 AND assigned_to = ?)) "
            "ORDER BY created_at DESC;",
            json_array_cb, array,
            4, (char *)city,
            2, (sqlite3_int64)userId,
            0);
    } else {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE author_id = ? AND status < 2 ORDER BY created_at DESC;",
            json_array_cb, array,
            2, (sqlite3_int64)userId,
            0);
    }

    char *out = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return out;
}

char *report_get_archived_json(uint64_t userId, const char *city, bool isOperator) {
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;

    if (isOperator) {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE city = ? AND status = 2 ORDER BY resolved_at DESC;",
            json_array_cb, array,
            4, (char *)city,
            0);
    } else {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE author_id = ? AND status = 2 ORDER BY resolved_at DESC;",
            json_array_cb, array,
            2, (sqlite3_int64)userId,
            0);
    }

    char *out = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return out;
}

bool report_get_by_id(uint64_t reportId, ActiveReport *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    int rows = db_query(
        "SELECT " SELECT_COLS " FROM reports WHERE id = ? LIMIT 1;",
        single_row_cb, out,
        2, (sqlite3_int64)reportId,
        0);
    return rows > 0;
}

int report_count_active(void) {
    int count = 0;
    db_query("SELECT COUNT(*) FROM reports WHERE status < 2;",
             count_cb, &count, 0);
    return count;
}

/* ── Utility ─────────────────────────────────────────────────────────── */

bool report_to_json(const ActiveReport *r, char *dest, size_t dest_size) {
    if (!r || !dest || dest_size == 0) return false;

    cJSON *obj = report_to_cjson(r);
    if (!obj) return false;

    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str) return false;

    size_t len = strlen(str);
    bool   ok  = len < dest_size;
    if (ok) memcpy(dest, str, len + 1);
    free(str);
    return ok;
}