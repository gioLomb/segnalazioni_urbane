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
 * In-memory representation of a report row, used as a return type
 * for single-row lookups and JSON serialisation.
 * assignedTo == 0 means no operator has taken charge yet.
 */
typedef struct {
    uint64_t     reportId;
    uint64_t     authorId;
    uint64_t     assignedTo;    /* 0 = unassigned */
    double       lat;
    double       lon;
    char         city[CITY_LEN];
    char         category[CAT_LEN];
    char         description[DESC_LEN];
    ReportStatus status;
    time_t       createdAt;
    time_t       assignedAt;    /* 0 if not yet assigned */
    time_t       resolvedAt;    /* 0 if not yet resolved */
} ActiveReport;

/* ── Setup ───────────────────────────────────────────────────────────── */

/*
 * Creates the reports table and its indexes if they do not already exist.
 * Must be called once at startup before any other report_* function.
 * Returns 0 on success, -1 on error.
 */
int report_setup_table(void);

/* ── Write operations ────────────────────────────────────────────────── */

/*
 * Inserts a new report (status = ACTIVE, assigned_to = NULL).
 * Returns the auto-incremented report ID on success, 0 on error.
 */
uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc);

/*
 * Operator takes charge of a report (status 0 -> 1).
 * The UPDATE is atomic: it only touches the row when
 * status = 0 AND assigned_to IS NULL, preventing double-assignment.
 *
 * Returns  1 if the claim succeeded,
 *          0 if the report was already taken or not found (-> 409),
 *         -1 on DB error.
 */
int report_assign(uint64_t reportId, uint64_t operatorId);

/*
 * Marks a report as resolved (status -> 2).
 * Only the operator who previously took it in charge may resolve it
 * (assigned_to == operatorId is enforced in the WHERE clause).
 *
 * Returns  1 on success,
 *          0 if the row was not found or does not belong to this operator (-> 403),
 *         -1 on DB error.
 */
int report_resolve(uint64_t reportId, uint64_t operatorId);

/* ── Read operations ─────────────────────────────────────────────────── */

/*
 * Returns a heap-allocated JSON array of non-resolved reports (status < 2).
 *   Citizen  (isOperator = false): filtered by author_id = userId
 *   Operator (isOperator = true) : filtered by city = city
 * Returns "[]" for empty results. Caller must free() the result.
 */
char *report_get_active_json(uint64_t userId, const char *city, bool isOperator);

/*
 * Returns a heap-allocated JSON array of resolved reports (status = 2).
 *   Citizen  (isOperator = false): filtered by author_id = userId
 *   Operator (isOperator = true) : filtered by city = city
 * Returns "[]" for empty results. Caller must free() the result.
 */
char *report_get_archived_json(uint64_t userId, const char *city, bool isOperator);

/*
 * Fetches a single report by ID into *out.
 * Returns true if found, false otherwise.
 * Used by route handlers to perform city-level authorisation checks
 * before delegating to report_assign / report_resolve.
 */
bool report_get_by_id(uint64_t reportId, ActiveReport *out);

/*
 * Returns the total number of non-resolved reports (status < 2).
 * Used by the /api/stats endpoint.
 */
int report_count_active(void);

/* ── Utility ─────────────────────────────────────────────────────────── */

/*
 * Serialises a report to a JSON object in dest (NUL-terminated, destSize bytes).
 * Returns true on success, false if dest is too small or r is NULL.
 */
bool report_to_json(const ActiveReport *r, char *dest, size_t destSize);

#endif /* REPORT_H */