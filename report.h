#ifndef REPORT_H
#define REPORT_H

#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define CITY_LEN  32
#define CAT_LEN   16
#define DESC_LEN  128

typedef enum {
    STATUS_ACTIVE      = 0,
    STATUS_IN_PROGRESS = 1,
    STATUS_RESOLVED    = 2
} ReportStatus;

/*
 * Colonne della tabella reports (ordine usato in ogni SELECT)
 * Tiene sincronizzati gli indici in cursor_to_report
 */
typedef enum {
    REP_COL_ID = 0,
    REP_COL_AUTHOR_ID,
    REP_COL_ASSIGNED_TO,
    REP_COL_LAT,
    REP_COL_LON,
    REP_COL_CITY,
    REP_COL_CATEGORY,
    REP_COL_DESCRIPTION,
    REP_COL_STATUS,
    REP_COL_CREATED_AT,
    REP_COL_ASSIGNED_AT,
    REP_COL_RESOLVED_AT
} ReportCol;

typedef struct {
    uint64_t     reportId;
    uint64_t     authorId;
    uint64_t     assignedTo;
    double       lat;
    double       lon;
    time_t       createdAt;     // tutti i campi a 8 byte insieme
    time_t       assignedAt;
    time_t       resolvedAt;
    char         city[32];
    char         category[16];
    ReportStatus status;        // 4 byte
    char         description[128]; // 128 — il padding finale è assorbito
} ActiveReport;

/* ── Setup ───────────────────────────────────────────────────────────── */

int report_setup_table(void);

/* ── Write operations ────────────────────────────────────────────────── */

uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc);

int report_assign(uint64_t reportId, uint64_t operatorId);

int report_resolve(uint64_t reportId, uint64_t operatorId);

/* ── Read operations ─────────────────────────────────────────────────── */

char *report_get_active_json(uint64_t userId, const char *city, bool isOperator);

char *report_get_archived_json(uint64_t userId, const char *city, bool isOperator);

bool report_get_by_id(uint64_t reportId, ActiveReport *out);

int report_count_active(void);

/* ── Utility ─────────────────────────────────────────────────────────── */

bool report_to_json(const ActiveReport *r, char *dest, size_t destSize);

#endif /* REPORT_H */
