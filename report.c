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
    strncpy(r->city,        db_cursor_text(c, REP_COL_CITY), CITY_LEN - 1);
    strncpy(r->category,    db_cursor_text(c, REP_COL_CATEGORY), CAT_LEN  - 1);
    strncpy(r->description, db_cursor_text(c, REP_COL_DESCRIPTION), DESC_LEN - 1);
    r->status     = (ReportStatus)db_cursor_int64(c,  REP_COL_STATUS);
    r->createdAt  = (time_t)      db_cursor_int64(c,  REP_COL_CREATED_AT);
    r->assignedAt = (time_t)      db_cursor_int64(c,  REP_COL_ASSIGNED_AT);
    r->resolvedAt = (time_t)      db_cursor_int64(c,  REP_COL_RESOLVED_AT);
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
/* ── JSON cache ─────────────────────────────────────────────────────── */
/*
 * Simple TTL cache for the two most-queried endpoints:
 *   report_get_active_json()   — polled every 30 s by the operator map
 *   report_get_archived_json() — loaded on dashboard open
 *
 * Design
 * ──────
 * Fixed-size array of CacheEntry structs (no heap, no hash table).
 * Each entry is keyed by (city, user_id, is_operator, is_archived).
 * Lookup is O(n) — with at most a handful of active cities this is
 * faster than any hash table in practice.
 *
 * On a cache hit the inline JSON buffer is copied directly into the caller's buffer —
 * caller can free() it unconditionally, matching the behaviour of a fresh
 * query.
 *
 * TTL
 * ───
 * CACHE_TTL_SECONDS controls how stale the data can be.
 * 5 seconds is conservative: an operator refreshing every 30 s will get
 * a live query once every 5 s at worst, cached results the rest of the time.
 * Raise it if your workload allows (e.g. 30 s for archived reports).
 *
 * The server is single-threaded (libuv event loop) so no locking is needed.
 */

#define CACHE_TTL_SECONDS 5
#define CACHE_MAX_ENTRIES 32   /* enough for ~16 cities × 2 query types */

/*
 * JSON responses for the two most-queried endpoints fit comfortably inside
 * RESPONSE_BUFFER_SIZE bytes.  We store the serialised JSON directly in the
 * cache entry so that neither cache_store() nor cache_find() need to call
 * malloc/strdup — zero heap traffic on a hit.
 *
 * cached_at == 0 signals an empty slot (g_cache is static, zero-initialised).
 */
#include "http_types.h"   /* pulls in RESPONSE_BUFFER_SIZE */

typedef struct {
    /* Key */
    char     city[CITY_LEN];
    uint64_t user_id;       /* 0 for operator queries (keyed by city only) */
    bool     is_operator;
    bool     is_archived;
    /* Value */
    time_t   cached_at;     /* 0 = empty slot */
    char     json[RESPONSE_BUFFER_SIZE];   /* inline buffer — no heap */
} CacheEntry;

static CacheEntry g_cache[CACHE_MAX_ENTRIES];

/*
 * Returns the index of a matching live entry, or -1 on miss/expiry.
 * An expired entry is cleared (cached_at reset to 0) on lookup.
 */
static int cache_find(const char *city, uint64_t user_id,
                      bool is_operator, bool is_archived) {
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cached_at) continue;   /* empty slot */

        /* Evict expired entries on the fly */
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

/*
 * Writes json (a NUL-terminated string already in buf) into the cache slot.
 * json_len is strlen(json) — passed in to avoid a second strlen().
 * Evicts the oldest entry if all slots are taken.
 * Does NOT take ownership: the caller's buffer is not touched after return.
 */
static void cache_store(const char *city, uint64_t user_id,
                        bool is_operator, bool is_archived,
                        const char *json, size_t json_len) {
    /* Silently drop if the serialised JSON is too large for the inline buf. */
    if (json_len >= RESPONSE_BUFFER_SIZE) return;

    /* Find an empty slot; track oldest in parallel for eviction fallback */
    int    slot        = -1;
    int    oldest_idx  = 0;
    time_t oldest_time = g_cache[0].cached_at;   /* initialise to first element */

    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!g_cache[i].cached_at) { slot = i; break; }
        if (g_cache[i].cached_at < oldest_time) {
            oldest_time = g_cache[i].cached_at;
            oldest_idx  = i;
        }
    }

    /* All slots full — evict the oldest */
    if (slot == -1) slot = oldest_idx;

    CacheEntry *e  = &g_cache[slot];
    strncpy(e->city, city, CITY_LEN - 1);
    e->city[CITY_LEN - 1] = '\0';
    e->user_id     = user_id;
    e->is_operator = is_operator;
    e->is_archived = is_archived;
    memcpy(e->json, json, json_len + 1);   /* include NUL */
    e->cached_at   = time(NULL);
}

/*
 * Targeted invalidation after a status change on a single report.
 * Only two slots can contain stale data:
 *   1. The operator slot for the city  (user_id=0, is_operator=true)
 *   2. The author's citizen slot       (user_id=authorId, is_operator=false)
 * All other citizen slots are keyed by their own author_id and are
 * unaffected by changes to another user's report.
 */
void report_cache_invalidate_city(const char *city, uint64_t authorId) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cached_at) continue;
        if (strncmp(e->city, city, CITY_LEN) != 0) continue;

        if (e->is_operator                       /* operator slot */
                || e->user_id == authorId)       /* report author slot */
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
    uint64_t cache_uid = isOperator ? 0 : userId;

    /* Cache hit — copy inline buffer straight into the caller's buf */
    int idx = cache_find(city, cache_uid, isOperator, false);
    if (idx >= 0) {
        size_t len = strlen(g_cache[idx].json);
        if (len < max) {
            memcpy(buf, g_cache[idx].json, len + 1);
            return len;
        }
        /* Cached value somehow exceeds caller's buf — treat as miss */
    }

    /* Cache miss: run the query, serialise directly into buf */
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

    size_t len = cursor_to_json_array(c, buf, max);

    /* Populate the cache from the buffer we just filled */
    if (len > 0)
        cache_store(city, cache_uid, isOperator, false, buf, len);

    return len;
}

size_t report_get_archived_json(char *buf, size_t max,
                                uint64_t userId, const char *city, bool isOperator) {
    uint64_t cache_uid = isOperator ? 0 : userId;

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
              "WHERE city = ? AND status = 2 "
              "ORDER BY resolved_at DESC;",
              "s", city)
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
    /* No TTL cache for admin — semplicità; i dati cambiano raramente */
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