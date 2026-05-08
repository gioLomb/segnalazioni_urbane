#include "report.h"
#include "db.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Column order used in every SELECT ──────────────────────────────────
 * 0  id          1  author_id    2  assigned_to
 * 3  lat         4  lon          5  city
 * 6  category    7  description  8  status
 * 9  created_at  10 assigned_at  11 resolved_at
 * ─────────────────────────────────────────────────────────────────────*/
#define SELECT_COLS \
    "id, author_id, assigned_to, lat, lon, city, category, description, " \
    "status, created_at, assigned_at, resolved_at"

/* ── Setup ───────────────────────────────────────────────────────────── */

int report_setup_table(void) {
    if (db_exec(
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
            ");", NULL) != 0)
        return -1;

    /* Non fatali: gli indici sono un'ottimizzazione delle performance. */
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_author ON reports(author_id);", NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_city   ON reports(city);",      NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(status);",    NULL);
    return 0;
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

/* Legge la riga corrente del cursore in una struct ActiveReport. */
static void cursor_to_report(DbCursor *c, ActiveReport *r) {
    memset(r, 0, sizeof(*r));
    r->reportId   = (uint64_t)db_cursor_int64 (c, 0);
    r->authorId   = (uint64_t)db_cursor_int64 (c, 1);
    r->assignedTo = (uint64_t)db_cursor_int64 (c, 2);
    r->lat        =           db_cursor_double(c, 3);
    r->lon        =           db_cursor_double(c, 4);
    strncpy(r->city,        db_cursor_text(c, 5), CITY_LEN - 1);
    strncpy(r->category,    db_cursor_text(c, 6), CAT_LEN  - 1);
    strncpy(r->description, db_cursor_text(c, 7), DESC_LEN - 1);
    r->status     = (ReportStatus)db_cursor_int64(c,  8);
    r->createdAt  = (time_t)      db_cursor_int64(c,  9);
    r->assignedAt = (time_t)      db_cursor_int64(c, 10);
    r->resolvedAt = (time_t)      db_cursor_int64(c, 11);
}

/* ── JSON builder ────────────────────────────────────────────────────── */

/*
 * Converte un ActiveReport in un oggetto cJSON.
 * cJSON_AddStringToObject gestisce automaticamente l'escaping di
 * \n, \r, \t, ", \ e tutti i caratteri di controllo JSON.
 */
static cJSON *report_to_cjson(const ActiveReport *r) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id",          (double)r->reportId);
    cJSON_AddNumberToObject(obj, "author_id",   (double)r->authorId);
    cJSON_AddNumberToObject(obj, "lat",         r->lat);
    cJSON_AddNumberToObject(obj, "lon",         r->lon);
    cJSON_AddStringToObject(obj, "city",        r->city);
    cJSON_AddStringToObject(obj, "category",    r->category);
    cJSON_AddStringToObject(obj, "description", r->description);
    cJSON_AddNumberToObject(obj, "status",      (double)r->status);
    cJSON_AddNumberToObject(obj, "created_at",  (double)r->createdAt);

    return obj;
}

/* Helper: itera il cursore, aggiunge ogni riga all'array cJSON,
   serializza e restituisce la stringa heap-allocated. */
static char *cursor_to_json_array(DbCursor *c) {
    cJSON *array = cJSON_CreateArray();
    while (db_cursor_next(c)) {
        ActiveReport r;
        cursor_to_report(c, &r);
        cJSON_AddItemToArray(array, report_to_cjson(&r));
    }
    db_cursor_close(c);
    char *out = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return out;
}

/* ── Write operations ────────────────────────────────────────────────── */

uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc) {
    int rc = db_exec(
        "INSERT INTO reports "
        "(author_id, lat, lon, city, category, description, status, created_at) "
        "VALUES (?,?,?,?,?,?,0,?);",
        "lffsssl",
        (int64_t)authorId, lat, lon, city, category, desc, (int64_t)time(NULL));
    if (rc != 0) return 0;
    int64_t id = db_last_insert_id();
    return (id > 0) ? (uint64_t)id : 0;
}

int report_assign(uint64_t reportId, uint64_t operatorId) {
    /*
     * Claim atomico: la WHERE garantisce che nessun altro operatore abbia
     * già preso la segnalazione. In caso di race condition vince un solo
     * UPDATE; l'altro ottiene rowcount=0 e riceve 409 nel route handler.
     */
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 1, assigned_to = ?, assigned_at = ? "
        "WHERE id = ? AND status = 0 AND assigned_to IS NULL;",
        "lll",
        (int64_t)operatorId, (int64_t)time(NULL), (int64_t)reportId);
    if (rc != 0) return -1;
    return db_changes() > 0 ? 1 : 0;
}

int report_resolve(uint64_t reportId, uint64_t operatorId) {
    /*
     * Solo l'operatore che ha preso in carico la segnalazione può risolverla:
     * assigned_to = operatorId nella WHERE lo impone.
     */
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 2, resolved_at = ? "
        "WHERE id = ? AND assigned_to = ? AND status = 1;",
        "lll",
        (int64_t)time(NULL), (int64_t)reportId, (int64_t)operatorId);
    if (rc != 0) return -1;
    return db_changes() > 0 ? 1 : 0;
}

/* ── Read operations ─────────────────────────────────────────────────── */

char *report_get_active_json(uint64_t userId, const char *city, bool isOperator) {
    DbCursor *c = isOperator
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE city = ? AND (status = 0 OR (status = 1 AND assigned_to = ?)) "
              "ORDER BY created_at DESC;",
              "sl", city, (int64_t)userId)
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status < 2 "
              "ORDER BY created_at DESC;",
              "l", (int64_t)userId);
    return cursor_to_json_array(c);
}

char *report_get_archived_json(uint64_t userId, const char *city, bool isOperator) {
    DbCursor *c = isOperator
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE city = ? AND status = 2 "
              "ORDER BY resolved_at DESC;",
              "s", city)
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status = 2 "
              "ORDER BY resolved_at DESC;",
              "l", (int64_t)userId);
    return cursor_to_json_array(c);
}

bool report_get_by_id(uint64_t reportId, ActiveReport *out) {
    if (!out) return false;
    DbCursor *c = db_cursor_open(
        "SELECT " SELECT_COLS " FROM reports WHERE id = ? LIMIT 1;",
        "l", (int64_t)reportId);
    bool found = db_cursor_next(c);
    if (found) cursor_to_report(c, out);
    db_cursor_close(c);
    return found;
}

int report_count_active(void) {
    DbCursor *c = db_cursor_open(
        "SELECT COUNT(*) FROM reports WHERE status < 2;", NULL);
    int count = db_cursor_next(c) ? (int)db_cursor_int64(c, 0) : 0;
    db_cursor_close(c);
    return count;
}

/* ── Utility ─────────────────────────────────────────────────────────── */

bool report_to_json(const ActiveReport *r, char *dest, size_t dest_size) {
    if (!r || !dest || dest_size == 0) return false;
    cJSON *obj = report_to_cjson(r);
    if (!obj) return false;
    char  *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str) return false;
    size_t len = strlen(str);
    bool   ok  = len < dest_size;
    if (ok) memcpy(dest, str, len + 1);
    free(str);
    return ok;
}