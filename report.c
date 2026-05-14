#include "report.h"
#include "db.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Column order used in every SELECT ────────────────────────────────── */
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

    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_author ON reports(author_id);", NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_city   ON reports(city);",      NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(status);",    NULL);
    return 0;
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

static void cursor_to_report(DbCursor *c, ActiveReport *r) {
    memset(r, 0, sizeof(*r));
    r->reportId   = (uint64_t)db_cursor_int64 (c, REP_COL_ID);
    r->authorId   = (uint64_t)db_cursor_int64 (c, REP_COL_AUTHOR_ID);
    r->assignedTo = (uint64_t)db_cursor_int64 (c, REP_COL_ASSIGNED_TO);
    r->lat        =           db_cursor_double(c, REP_COL_LAT);
    r->lon        =           db_cursor_double(c, REP_COL_LON);
    strncpy(r->city,        db_cursor_text(c, REP_COL_CITY),        CITY_LEN - 1);
    strncpy(r->category,    db_cursor_text(c, REP_COL_CATEGORY),    CAT_LEN  - 1);
    strncpy(r->description, db_cursor_text(c, REP_COL_DESCRIPTION), DESC_LEN - 1);
    r->status     = (ReportStatus)db_cursor_int64(c, REP_COL_STATUS);
    r->createdAt  = (time_t)      db_cursor_int64(c, REP_COL_CREATED_AT);
    r->assignedAt = (time_t)      db_cursor_int64(c, REP_COL_ASSIGNED_AT);
    r->resolvedAt = (time_t)      db_cursor_int64(c, REP_COL_RESOLVED_AT);
}

/* ── JSON builder ────────────────────────────────────────────────────── */

static cJSON *report_to_cjson(const ActiveReport *r) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddNumberToObject(obj, "id",          (double)r->reportId);
    cJSON_AddNumberToObject(obj, "author_id",   (double)r->authorId);
    cJSON_AddNumberToObject(obj, "assigned_to", (double)r->assignedTo);
    cJSON_AddNumberToObject(obj, "lat",         r->lat);
    cJSON_AddNumberToObject(obj, "lon",         r->lon);
    cJSON_AddStringToObject(obj, "city",        r->city);
    cJSON_AddStringToObject(obj, "category",    r->category);
    cJSON_AddStringToObject(obj, "description", r->description);
    cJSON_AddNumberToObject(obj, "status",      (double)r->status);
    cJSON_AddNumberToObject(obj, "created_at",  (double)r->createdAt);
    cJSON_AddNumberToObject(obj, "assigned_at", (double)r->assignedAt);
    cJSON_AddNumberToObject(obj, "resolved_at", (double)r->resolvedAt);

    return obj;
}

static size_t cursor_to_json_array(DbCursor *c, char *buf, size_t max) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        if (max > 0) buf[0] = '\0';
        return 0;
    }

    while (db_cursor_next(c)) {
        ActiveReport r;
        cursor_to_report(c, &r);
        cJSON_AddItemToArray(array, report_to_cjson(&r));
    }

    db_cursor_close(c);

    if (!cJSON_PrintPreallocated(array, buf, (int)max, false)) {
        cJSON_Delete(array);
        if (max > 0) buf[0] = '\0';
        return 0;
    }

    cJSON_Delete(array);
    return strlen(buf);
}

/* ── JSON cache ──────────────────────────────────────────────────────── */

#define CACHE_TTL_SECONDS 5
#define CACHE_MAX_ENTRIES 32

#include "http_types.h"

typedef struct {
    char     city[CITY_LEN];
    uint64_t user_id;
    bool     is_operator;
    bool     is_archived;
    time_t   cached_at;
    char     json[RESPONSE_BUFFER_SIZE];
} CacheEntry;

static CacheEntry g_cache[CACHE_MAX_ENTRIES];

static int cache_find(const char *city, uint64_t user_id,
                      bool is_operator, bool is_archived) {
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cached_at) continue;
        if (now - e->cached_at > CACHE_TTL_SECONDS) {
            e->cached_at = 0;
            continue;
        }
        if (e->is_operator == is_operator
            && e->is_archived == is_archived
            && strncmp(e->city, city, CITY_LEN) == 0
            && e->user_id == user_id)
            return i;
    }
    return -1;
}

static void cache_store(const char *city, uint64_t user_id,
                        bool is_operator, bool is_archived,
                        const char *json, size_t json_len) {
    if (json_len >= RESPONSE_BUFFER_SIZE) return;

    int    slot       = -1;
    int    oldest_idx = 0;
    time_t oldest_time = g_cache[0].cached_at;

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!g_cache[i].cached_at) { slot = i; break; }
        if (g_cache[i].cached_at < oldest_time) {
            oldest_time = g_cache[i].cached_at;
            oldest_idx  = i;
        }
    }

    if (slot == -1) slot = oldest_idx;

    CacheEntry *e = &g_cache[slot];
    strncpy(e->city, city, CITY_LEN - 1);
    e->city[CITY_LEN - 1] = '\0';
    e->user_id     = user_id;
    e->is_operator = is_operator;
    e->is_archived = is_archived;
    memcpy(e->json, json, json_len + 1);
    e->cached_at   = time(NULL);
}

void report_cache_invalidate_city(const char *city, uint64_t authorId) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cached_at) continue;
        if (strncmp(e->city, city, CITY_LEN) != 0) continue;
        if (e->is_operator || e->user_id == authorId)
            e->cached_at = 0;
    }
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

#include <stdio.h>

size_t report_get_active_json(char *buf, size_t max,
                              uint64_t userId, const char *city, bool isOperator) {
    /*
     * Cache key: sempre userId.
     * Bug precedente: per gli operatori si usava 0 (condiviso tra tutti
     * gli operatori della stessa città) — corretto: ora è per-utente.
     */
    uint64_t cache_uid = userId;

    int idx = cache_find(city, cache_uid, isOperator, false);
    if (idx >= 0) {
        size_t len = strlen(g_cache[idx].json);
        if (len < max) {
            memcpy(buf, g_cache[idx].json, len + 1);
            return len;
        }
    }

    DbCursor *c = isOperator
        /* Operatore: SOLO le segnalazioni assegnate a lui */
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE status = 1 AND assigned_to = ? "
              "ORDER BY assigned_at DESC;",
              "l", (int64_t)userId)
        /* Cittadino: le sue segnalazioni non ancora risolte */
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status < 2 "
              "ORDER BY created_at DESC;",
              "l", (int64_t)userId);

    size_t len = cursor_to_json_array(c, buf, max);

    if (len > 0)
        cache_store(city, cache_uid, isOperator, false, buf, len);

    return len;
}

size_t report_get_archived_json(char *buf, size_t max,
                                uint64_t userId, const char *city, bool isOperator) {
    /* Cache key per-utente, come per active (stesso bug corretto) */
    uint64_t cache_uid = userId;

    int idx = cache_find(city, cache_uid, isOperator, true);
    if (idx >= 0) {
        size_t len = strlen(g_cache[idx].json);
        if (len < max) {
            memcpy(buf, g_cache[idx].json, len + 1);
            return len;
        }
    }

    DbCursor *c = isOperator
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE assigned_to = ? AND status = 2 "
              "ORDER BY resolved_at DESC;",
              "l", (int64_t)userId)
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status = 2 "
              "ORDER BY resolved_at DESC;",
              "l", (int64_t)userId);

    size_t len = cursor_to_json_array(c, buf, max);

    if (len > 0)
        cache_store(city, cache_uid, isOperator, true, buf, len);

    return len;
}

size_t report_get_all_city_json(char *buf, size_t max, const char *city) {
    DbCursor *c = db_cursor_open(
        "SELECT " SELECT_COLS " FROM reports "
        "WHERE city = ? ORDER BY created_at DESC;",
        "s", city);
    return cursor_to_json_array(c, buf, max);
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