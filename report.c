#include "report.h"
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sqlite3.h>

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

    /* Errors here are non-fatal: indexes are a performance optimisation. */
    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_author "
               "ON reports(author_id);", 0);
    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_city "
               "ON reports(city);", 0);
    db_execute("CREATE INDEX IF NOT EXISTS idx_reports_status "
               "ON reports(status);", 0);
    return 0;
}

/* ── Row mapper (shared by all query callbacks) ──────────────────────── */

static void row_to_report(sqlite3_stmt *stmt, ActiveReport *r) {
    memset(r, 0, sizeof(*r));
    r->reportId   = (uint64_t)sqlite3_column_int64(stmt, 0);
    r->authorId   = (uint64_t)sqlite3_column_int64(stmt, 1);
    r->assignedTo = (uint64_t)sqlite3_column_int64(stmt, 2); /* 0 when NULL */
    r->lat        = sqlite3_column_double(stmt, 3);
    r->lon        = sqlite3_column_double(stmt, 4);

    const char *city = (const char *)sqlite3_column_text(stmt, 5);
    const char *cat  = (const char *)sqlite3_column_text(stmt, 6);
    const char *desc = (const char *)sqlite3_column_text(stmt, 7);
    strncpy(r->city,        city ? city : "", CITY_LEN - 1);
    strncpy(r->category,    cat  ? cat  : "", CAT_LEN  - 1);
    strncpy(r->description, desc ? desc : "", DESC_LEN - 1);
    r->city[CITY_LEN - 1]        = '\0';
    r->category[CAT_LEN - 1]     = '\0';
    r->description[DESC_LEN - 1] = '\0';

    r->status     = (ReportStatus)sqlite3_column_int(stmt, 8);
    r->createdAt  = (time_t)sqlite3_column_int64(stmt, 9);
    r->assignedAt = (time_t)sqlite3_column_int64(stmt, 10);
    r->resolvedAt = (time_t)sqlite3_column_int64(stmt, 11);
}

/* ── JSON builder ────────────────────────────────────────────────────── */

typedef struct { char *buf; size_t len; size_t cap; int ok; } JsonBuf;

static int jbuf_init(JsonBuf *j) {
    j->cap = 4096;
    j->buf = malloc(j->cap);
    if (!j->buf) { j->ok = 0; return 0; }
    j->buf[0] = '[';
    j->len    = 1;
    j->ok     = 1;
    return 1;
}

static void jbuf_append(JsonBuf *j, const ActiveReport *r) {
    if (!j->ok) return;

    char tmp[512 + DESC_LEN + CITY_LEN + CAT_LEN];
    int n = snprintf(tmp, sizeof(tmp),
        "%s{\"id\":%llu,\"author_id\":%llu,"
        "\"lat\":%.6f,\"lon\":%.6f,"
        "\"city\":\"%s\",\"category\":\"%s\","
        "\"description\":\"%s\","
        "\"status\":%d,\"created_at\":%lld}",
        (j->len == 1) ? "" : ",",          /* skip comma before first item */
        (unsigned long long)r->reportId,
        (unsigned long long)r->authorId,
        r->lat, r->lon,
        r->city, r->category, r->description,
        (int)r->status,
        (long long)r->createdAt);

    if (n < 0) { j->ok = 0; return; }

    while (j->len + (size_t)n + 2 > j->cap) {
        char *t = realloc(j->buf, j->cap * 2);
        if (!t) { j->ok = 0; return; }
        j->buf  = t;
        j->cap *= 2;
    }
    memcpy(j->buf + j->len, tmp, (size_t)n);
    j->len += (size_t)n;
}

static char *jbuf_close(JsonBuf *j) {
    if (!j->ok) { free(j->buf); return NULL; }
    if (j->len + 2 > j->cap) {
        char *t = realloc(j->buf, j->cap + 2);
        if (!t) { free(j->buf); return NULL; }
        j->buf = t;
    }
    j->buf[j->len++] = ']';
    j->buf[j->len]   = '\0';
    return j->buf;
}

/* ── Query callbacks ─────────────────────────────────────────────────── */

static int json_row_cb(sqlite3_stmt *stmt, void *userdata) {
    JsonBuf *j = (JsonBuf *)userdata;
    ActiveReport r;
    row_to_report(stmt, &r);
    jbuf_append(j, &r);
    return j->ok ? 0 : -1; /* -1 aborts the query loop */
}

static int single_row_cb(sqlite3_stmt *stmt, void *userdata) {
    row_to_report(stmt, (ActiveReport *)userdata);
    return 0; /* returning 0 continues, but there should be only one row */
}

static int count_cb(sqlite3_stmt *stmt, void *userdata) {
    *(int *)userdata = sqlite3_column_int(stmt, 0);
    return 0;
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
    /*
     * Atomic claim: the WHERE clause ensures no other operator has already
     * taken this report. If two operators race, only one UPDATE wins;
     * the other gets rowcount=0 and receives a 409 in the route handler.
     */
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
    /*
     * Only the operator who took it in charge can resolve it.
     * assigned_to = operatorId in the WHERE clause enforces this.
     */
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
    JsonBuf j;
    if (!jbuf_init(&j)) return NULL;

    if (isOperator) {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE city = ? AND (status = 0 OR (status = 1 AND assigned_to = ?)) "
            "ORDER BY created_at DESC;",
            json_row_cb, &j,
            4, (char *)city,               // parametro 1: city (stringa)
            2, (sqlite3_int64)userId,      // parametro 2: userId (int64)
            0);
    } else {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE author_id = ? AND status < 2 ORDER BY created_at DESC;",
            json_row_cb, &j,
            2, (sqlite3_int64)userId,
            0);
    }

    return jbuf_close(&j);
}

char *report_get_archived_json(uint64_t userId, const char *city, bool isOperator) {
    JsonBuf j;
    if (!jbuf_init(&j)) return NULL;

    if (isOperator) {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE city = ? AND status = 2 ORDER BY resolved_at DESC;",
            json_row_cb, &j, 4, (char *)city, 0);
    } else {
        db_query(
            "SELECT " SELECT_COLS " FROM reports "
            "WHERE author_id = ? AND status = 2 ORDER BY resolved_at DESC;",
            json_row_cb, &j, 2, (sqlite3_int64)userId, 0);
    }

    return jbuf_close(&j);
}

bool report_get_by_id(uint64_t reportId, ActiveReport *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    int rows = db_query(
        "SELECT " SELECT_COLS " FROM reports WHERE id = ? LIMIT 1;",
        single_row_cb, out, 2, (sqlite3_int64)reportId, 0);
    return rows > 0;
}

int report_count_active(void) {
    int count = 0;
    db_query("SELECT COUNT(*) FROM reports WHERE status < 2;",
             count_cb, &count, 0);
    return count;
}

/* ── Utility ─────────────────────────────────────────────────────────── */

bool report_to_json(const ActiveReport *r, char *dest, size_t destSize) {
    if (!r || !dest || destSize == 0) return false;
    int n = snprintf(dest, destSize,
        "{\"id\":%llu,\"author_id\":%llu,"
        "\"lat\":%.6f,\"lon\":%.6f,"
        "\"city\":\"%s\",\"category\":\"%s\","
        "\"description\":\"%s\","
        "\"status\":%d,\"created_at\":%lld}",
        (unsigned long long)r->reportId, (unsigned long long)r->authorId,
        r->lat, r->lon, r->city, r->category, r->description,
        (int)r->status, (long long)r->createdAt);
    return n > 0 && (size_t)n < destSize;
}