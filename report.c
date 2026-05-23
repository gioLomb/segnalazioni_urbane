#include "report.h"
#include "db.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http_types.h"
#include <stdio.h>

#define CACHE_TTL_SECONDS 5
#define CACHE_MAX_ENTRIES 32

// Results larger than this threshold are not cached but are still served
// from the query — no artificial cap on returned data.
// ~16 KB covers ~15 worst-case reports (full unicode description); normal
// users always fit, users with many reports silently bypass the cache.
// Total footprint: 32 × 16 KB = 512 KB (vs 32 × 256 KB = 8 MB before).
#define CACHE_JSON_MAX (16 * 1024)

// Internal cache entry: keyed on (city, userId, isOperator, isArchived).
typedef struct {
    char     city[CITY_LEN];
    uint64_t userId;
    bool     isOperator;
    bool     isArchived;
    time_t   cachedAt;   // Unix timestamp of insertion; 0 means slot is free
    size_t   jsonLen;    // Length of json[], excluding the NUL terminator
    char     json[CACHE_JSON_MAX];
} CacheEntry;

// Declared at file scope so it is zero-initialised by the C runtime:
// every cachedAt starts at 0, meaning all slots are initially free.
static CacheEntry g_cache[CACHE_MAX_ENTRIES];

/* ── Column order used in every SELECT ────────────────────────────────── */

// Single source of truth for the SELECT column list.
// ReportCol enum values index into this order, so adding a column here
// requires a matching entry in the enum and in cursor_to_report().
#define SELECT_COLS \
    "id, author_id, assigned_to, lat, lon, city, category, description, " \
    "status, created_at, assigned_at, resolved_at, feedback"

/* ── Setup ───────────────────────────────────────────────────────────── */

int report_setup_table(void) {
    if (unlikely(db_exec(
            "CREATE TABLE IF NOT EXISTS reports ("
            "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  author_id   INTEGER NOT NULL,"
            "  assigned_to INTEGER,"          // NULL until assigned
            "  lat         REAL    NOT NULL DEFAULT 0,"
            "  lon         REAL    NOT NULL DEFAULT 0,"
            "  city        TEXT    NOT NULL DEFAULT '',"
            "  category    TEXT    NOT NULL DEFAULT '',"
            "  description TEXT    NOT NULL,"
            "  status      INTEGER NOT NULL DEFAULT 0,"
            "  created_at  INTEGER NOT NULL,"
            "  assigned_at INTEGER,"          // NULL until assigned
            "  resolved_at INTEGER,"          // NULL until resolved
            "  feedback    INTEGER"           // NULL until citizen rates (1-5)
            ");", NULL) != 0))
        return -1;


    // Separate indexes on author, city and status cover the three main
    // query shapes: per-user, per-city (admin) and per-status (operator).
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_author ON reports(author_id);", NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_city ON reports(city);", NULL);
    db_exec("CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(status);", NULL);
    return 0;
}

/* ── Row mapper ──────────────────────────────────────────────────────── */

// Maps the current cursor row to an Report using the fixed SELECT_COLS order.
static void cursor_to_report(DbCursor *c, Report *r) {
    // Zero the struct first so unused padding bytes and any skipped fields
    // don't contain garbage from the slab allocator.
    memset(r, 0, sizeof(*r));

    r->reportId = (uint64_t)db_cursor_int64(c, REP_COL_ID);
    r->authorId = (uint64_t)db_cursor_int64(c, REP_COL_AUTHOR_ID);
    r->assignedTo = (uint64_t)db_cursor_int64(c, REP_COL_ASSIGNED_TO);
    r->lat = db_cursor_double(c, REP_COL_LAT);
    r->lon = db_cursor_double(c, REP_COL_LON);

    // strncpy with (LEN - 1) ensures the buffer is never overrun;
    // the memset above already zeroed the last byte, so NUL termination
    // is guaranteed even if the source string fills the buffer exactly.
    strncpy(r->city,        db_cursor_text(c, REP_COL_CITY),        CITY_LEN - 1);
    strncpy(r->category,    db_cursor_text(c, REP_COL_CATEGORY),    CAT_LEN - 1);
    strncpy(r->description, db_cursor_text(c, REP_COL_DESCRIPTION), DESC_LEN - 1);

    r->status = (ReportStatus)db_cursor_int64(c, REP_COL_STATUS);
    r->createdAt = (time_t)db_cursor_int64(c, REP_COL_CREATED_AT);
    r->assignedAt = (time_t)db_cursor_int64(c, REP_COL_ASSIGNED_AT);
    r->resolvedAt = (time_t)db_cursor_int64(c, REP_COL_RESOLVED_AT);
    r->feedback   = (int)db_cursor_int64(c, REP_COL_FEEDBACK);
}

/* ── JSON builder ────────────────────────────────────────────────────── */

// Converts a single Report to a cJSON object. Caller owns the result.
static cJSON *report_to_cjson(const Report *r) {
    cJSON *obj = cJSON_CreateObject();
    if (unlikely(!obj)) return NULL;

    // IDs are uint64_t but JSON numbers are doubles; values stay exact
    // up to 2^53, which is well beyond any realistic row count.
    cJSON_AddNumberToObject(obj, "id",          (double)r->reportId);
    cJSON_AddNumberToObject(obj, "author_id",   (double)r->authorId);
    cJSON_AddNumberToObject(obj, "assigned_to", (double)r->assignedTo);
    cJSON_AddNumberToObject(obj, "lat",         r->lat);
    cJSON_AddNumberToObject(obj, "lon",         r->lon);
    cJSON_AddStringToObject(obj, "city",        r->city);
    cJSON_AddStringToObject(obj, "category",    r->category);
    cJSON_AddStringToObject(obj, "description", r->description);
    cJSON_AddNumberToObject(obj, "status",      (double)r->status);
    // Timestamps are stored and sent as Unix epoch integers.
    cJSON_AddNumberToObject(obj, "created_at",  (double)r->createdAt);
    cJSON_AddNumberToObject(obj, "assigned_at", (double)r->assignedAt);
    cJSON_AddNumberToObject(obj, "resolved_at", (double)r->resolvedAt);
    // feedback: 0 means not yet rated; values 1-5 are citizen star ratings.
    cJSON_AddNumberToObject(obj, "feedback",    (double)r->feedback);

    return obj;
}

// Drains the cursor into a JSON array, prints it into buf and closes the cursor.
// Returns the number of bytes written, or 0 on error.
static size_t cursor_to_json_array(DbCursor *c, char *buf, size_t max) {
    cJSON *array = cJSON_CreateArray();
    if (unlikely(!array)) {
        if (max > 0) buf[0] = '\0';
        return 0;
    }

    while (db_cursor_next(c)) {
        Report r;
        cursor_to_report(c, &r);
        cJSON_AddItemToArray(array, report_to_cjson(&r));
    }
    db_cursor_close(c);

    // cJSON_PrintPreallocated writes directly into buf without a malloc;
    // it returns false if the output would exceed max bytes, in which case
    // buf is left in an indeterminate state — reset it to an empty string.
    if (!cJSON_PrintPreallocated(array, buf, (int)max, false)) {
        cJSON_Delete(array);
        if (max > 0) buf[0] = '\0';
        return 0;
    }
    cJSON_Delete(array);
    return strlen(buf);
}

/* ── JSON cache ──────────────────────────────────────────────────────── */

// Scans the cache for a valid, non-expired entry matching the given key.
// Evicts stale entries on the fly. Returns the slot index, or -1 on miss.
static int cache_find(const char *city, uint64_t userId,
                      bool isOperator, bool isArchived) {
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cachedAt) continue;  // Free slot, skip immediately

        // Lazily evict expired entries encountered during a lookup
        // rather than running a separate sweep timer.
        if (unlikely(now - e->cachedAt > CACHE_TTL_SECONDS)) {
            e->cachedAt = 0;
            continue;
        }

        // All four key components must match: role and archive flag
        // determine which SQL query was used, so they are part of the key.
        if (e->isOperator == isOperator && e->isArchived == isArchived
            && strncmp(e->city, city, CITY_LEN) == 0 && e->userId == userId)
            return i;
    }
    return -1;
}

// Stores a JSON result in the cache. Silently skips results that exceed
// CACHE_JSON_MAX to avoid evicting useful entries with oversized payloads.
static void cache_store(const char *city, uint64_t userId,
                        bool isOperator, bool isArchived,
                        const char *json, size_t jsonLen) {
    if (jsonLen >= CACHE_JSON_MAX) return;

    int slot = -1;
    int oldestIdx = 0;
    time_t oldestTime = g_cache[0].cachedAt;

    // Find a free slot; simultaneously track the oldest entry as a fallback
    // so that one pass is enough regardless of the outcome.
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!g_cache[i].cachedAt) {
            slot = i;
            break;
        }
        if (g_cache[i].cachedAt < oldestTime) {
            oldestTime = g_cache[i].cachedAt;
            oldestIdx = i;
        }
    }

    // No free slot found: evict the oldest entry (LRU-approximation).
    if (slot == -1) slot = oldestIdx;

    CacheEntry *e = &g_cache[slot];
    strncpy(e->city, city, CITY_LEN - 1);
    e->city[CITY_LEN - 1] = '\0';  // Guarantee NUL termination
    e->userId = userId;
    e->isOperator = isOperator;
    e->isArchived = isArchived;
    e->jsonLen = jsonLen;
    memcpy(e->json, json, jsonLen + 1);  // +1 to copy the NUL terminator
    e->cachedAt = time(NULL);
}

void report_cache_invalidate_city(const char *city, uint64_t authorId) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        CacheEntry *e = &g_cache[i];
        if (!e->cachedAt) continue;
        if (strncmp(e->city, city, CITY_LEN) != 0) continue;

        // Operator-scoped entries must be invalidated because assigning or
        // resolving a report changes the operator's active/archived list.
        // Author-scoped entries must be invalidated because the report's
        // status changed and is no longer accurate for the citizen's view.
        if (e->isOperator || e->userId == authorId) e->cachedAt = 0;
    }
}

/* ── Write operations ────────────────────────────────────────────────── */

uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc) {
    // Format string "lffsssl": l=int64(authorId), f=double(lat), f=double(lon),
    // s=string(city), s=string(category), s=string(desc), l=int64(timestamp).
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
    // The WHERE clause is an atomic guard: status = 0 ensures the report is
    // still active, and assigned_to IS NULL prevents double-assignment in
    // case two admins click "assign" concurrently.
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 1, assigned_to = ?, assigned_at = ? "
        "WHERE id = ? AND status = 0 AND assigned_to IS NULL;",
        "lll",
        (int64_t)operatorId, (int64_t)time(NULL), (int64_t)reportId);
    if (rc != 0) return -1;

    // db_changes() == 0 means the guard condition was not met (already
    // assigned or not found); return 0 to distinguish from a hard error.
    if (db_changes() == 0) return 0;

    // Invalidate here, not in the caller, to keep cache logic centralised.
    Report r;
    if (report_get_by_id(reportId, &r)) report_cache_invalidate_city(r.city, r.authorId);

    return 1;
}

int report_accept(uint64_t reportId, uint64_t operatorId) {
    // Guard: status = 1 (STATUS_ASSIGNED) and assigned to this operator.
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 2 "
        "WHERE id = ? AND assigned_to = ? AND status = 1;",
        "ll",
        (int64_t)reportId, (int64_t)operatorId);
    if (rc != 0) return -1;
    if (db_changes() == 0) return 0;

    Report r;
    if (report_get_by_id(reportId, &r)) report_cache_invalidate_city(r.city, r.authorId);
    return 1;
}

int report_reject(uint64_t reportId, uint64_t operatorId) {
    // Guard: status = 1 (STATUS_ASSIGNED) and assigned to this operator.
    // Clear assignment so the admin can reassign to another operator.
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 0, assigned_to = NULL, assigned_at = NULL "
        "WHERE id = ? AND assigned_to = ? AND status = 1;",
        "ll",
        (int64_t)reportId, (int64_t)operatorId);
    if (rc != 0) return -1;
    if (db_changes() == 0) return 0;

    Report r;
    if (report_get_by_id(reportId, &r)) report_cache_invalidate_city(r.city, r.authorId);
    return 1;
}

int report_resolve(uint64_t reportId, uint64_t operatorId) {
    // The WHERE clause guards: status = 2 ensures the report is in progress,
    // and assigned_to = operatorId prevents an operator from resolving
    // a report that belongs to a different operator.
    int rc = db_exec(
        "UPDATE reports "
        "SET status = 3, resolved_at = ? "
        "WHERE id = ? AND assigned_to = ? AND status = 2;",
        "lll",
        (int64_t)time(NULL), (int64_t)reportId, (int64_t)operatorId);
    if (rc != 0) return -1;
    if (db_changes() == 0) return 0;

    // Invalidate here, not in the caller, to keep cache logic centralised.
    Report r;
    if (report_get_by_id(reportId, &r)) report_cache_invalidate_city(r.city, r.authorId);

    return 1;
}

int report_set_feedback(uint64_t reportId, uint64_t authorId, int stars) {
    if (stars < 1 || stars > 5) return -1;
    // Guard: only the author can rate, only if resolved, and only once.
    int rc = db_exec(
        "UPDATE reports "
        "SET feedback = ? "
        "WHERE id = ? AND author_id = ? AND status = 3 AND feedback IS NULL;",
        "lll",
        (int64_t)stars, (int64_t)reportId, (int64_t)authorId);
    if (rc != 0) return -1;
    if (db_changes() == 0) return 0;

    // Invalidate the archived cache so the new feedback is immediately visible.
    Report r;
    if (report_get_by_id(reportId, &r)) report_cache_invalidate_city(r.city, r.authorId);

    return 1;
}

/* ── Read operations ─────────────────────────────────────────────────── */

size_t report_get_active_json(char *buf, size_t max,
                              uint64_t userId, const char *city, bool isOperator) {
    int idx = cache_find(city, userId, isOperator, false);
    if (idx >= 0) {
        // Cache hit: copy the pre-built JSON directly into the output buffer.
        memcpy(buf, g_cache[idx].json, g_cache[idx].jsonLen + 1);
        return g_cache[idx].jsonLen;
    }

    DbCursor *c = isOperator
        // Operator: reports assigned to them, either pending acceptance (1)
        // or already in progress (2).
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE assigned_to = ? AND status IN (1, 2) "
              "ORDER BY assigned_at DESC;",
              "l", (int64_t)userId)
        // Citizen: their own reports not yet resolved (status 0, 1, or 2).
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status < 3 "
              "ORDER BY created_at DESC;",
              "l", (int64_t)userId);

    size_t len = cursor_to_json_array(c, buf, max);
    // Only cache non-empty results; an empty array could be a transient state.
    if (len > 0) cache_store(city, userId, isOperator, false, buf, len);

    return len;
}

size_t report_get_archived_json(char *buf, size_t max,
                                uint64_t userId, const char *city, bool isOperator) {
    int idx = cache_find(city, userId, isOperator, true);
    if (idx >= 0) {
        memcpy(buf, g_cache[idx].json, g_cache[idx].jsonLen + 1);
        return g_cache[idx].jsonLen;
    }

    DbCursor *c = isOperator
        // Operator: all reports they resolved (status = 3, assigned_to = them).
        ? db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE assigned_to = ? AND status = 3 "
              "ORDER BY resolved_at DESC;",
              "l", (int64_t)userId)
        // Citizen: all their reports that were resolved.
        : db_cursor_open(
              "SELECT " SELECT_COLS " FROM reports "
              "WHERE author_id = ? AND status = 3 "
              "ORDER BY resolved_at DESC;",
              "l", (int64_t)userId);

    size_t len = cursor_to_json_array(c, buf, max);
    if (likely(len > 0)) cache_store(city, userId, isOperator, true, buf, len);

    return len;
}

size_t report_get_all_city_json(char *buf, size_t max, const char *city) {
    // Admin view: JOIN with users to resolve operator name.
    // The extra column operator_name sits after all SELECT_COLS fields.
    DbCursor *c = db_cursor_open(
        "SELECT r.id, r.author_id, r.assigned_to, r.lat, r.lon, r.city, "
        "r.category, r.description, r.status, r.created_at, r.assigned_at, "
        "r.resolved_at, r.feedback, COALESCE(u.username, '') AS operator_name "
        "FROM reports r "
        "LEFT JOIN users u ON u.id = r.assigned_to "
        "WHERE r.city = ? ORDER BY r.created_at DESC;",
        "s", city);

    if (!c) {
        if (max > 2) { buf[0]='['; buf[1]=']'; buf[2]='\0'; }
        return 0;
    }

    cJSON *array = cJSON_CreateArray();
    if (!array) { if (max > 0) buf[0] = '\0'; return 0; }

    while (db_cursor_next(c)) {
        Report r;
        cursor_to_report(c, &r);
        cJSON *obj = report_to_cjson(&r);
        if (!obj) continue;
        // Column 13: operator_name (extra, not in SELECT_COLS).
        const char *opName = db_cursor_text(c, 13);
        cJSON_AddStringToObject(obj, "operator_name", opName ? opName : "");
        cJSON_AddItemToArray(array, obj);
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

bool report_get_by_id(uint64_t reportId, Report *out) {
    if (!out) return false;
    DbCursor *c = db_cursor_open(
        "SELECT " SELECT_COLS " FROM reports WHERE id = ? LIMIT 1;",
        "l", (int64_t)reportId);
    bool found = db_cursor_next(c);
    if (likely(found)) cursor_to_report(c, out);
    db_cursor_close(c);
    return found;
}

int report_count_active(void) {
    DbCursor *c = db_cursor_open(
        "SELECT COUNT(*) FROM reports WHERE status < 3;", NULL);
    int count = db_cursor_next(c) ? (int)db_cursor_int64(c, 0) : 0;
    db_cursor_close(c);
    return count;
}

/* ── Utility ─────────────────────────────────────────────────────────── */

bool report_to_json(const Report *r, char *dest, size_t destSize) {
    if (!r || !dest || destSize == 0) return false;
    cJSON *obj = report_to_cjson(r);
    if (!obj) return false;
    char *str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!str) return false;
    size_t len = strlen(str);
    // Strict less-than: we need len + 1 bytes to fit the NUL terminator.
    bool ok = len < destSize;
    if (ok) memcpy(dest, str, len + 1);
    free(str);
    return ok;
}