/**
 * @file report.h
 * @brief CRUD operations and JSON serialisation for urban reports.
 *
 * A report represents a civic issue (e.g. a pothole, broken streetlight)
 * submitted by a citizen and optionally assigned to and resolved by an operator.
 *
 * Status lifecycle:
 *   STATUS_ACTIVE → STATUS_ASSIGNED → STATUS_IN_PROGRESS → STATUS_RESOLVED
 *
 * After admin assigns, the operator must explicitly accept (→ IN_PROGRESS)
 * or reject (→ ACTIVE, unassigned) before work begins.
 */

#ifndef REPORT_H
#define REPORT_H

#include "config.h"
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define CITY_LEN  32
#define CAT_LEN   16
#define DESC_LEN  128

/**
 * @brief Lifecycle status of a report.
 */
typedef enum {
    STATUS_ACTIVE      = 0, /**< Submitted, not yet assigned                  */
    STATUS_ASSIGNED    = 1, /**< Assigned by admin, pending operator response  */
    STATUS_IN_PROGRESS = 2, /**< Accepted by operator — work in progress       */
    STATUS_RESOLVED    = 3  /**< Marked as resolved by operator                */
} ReportStatus;

/**
 * @brief Column indices for every SELECT that uses SELECT_COLS.
 *
 * Kept in sync with the column order in the SELECT_COLS macro so that
 * cursor_to_report() can reference columns by name rather than by magic number.
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
    REP_COL_RESOLVED_AT,
    REP_COL_FEEDBACK
} ReportCol;

/**
 * @brief In-memory representation of a single report row.
 *
 * Fields are ordered to minimise padding: 8-byte integers first,
 * followed by fixed-length char arrays and the 4-byte enum last.
 */
typedef struct {
    uint64_t     reportId;
    uint64_t     authorId;
    uint64_t     assignedTo;
    double       lat;
    double       lon;
    time_t       createdAt;
    time_t       assignedAt;
    time_t       resolvedAt;
    char         city[CITY_LEN];
    char         category[CAT_LEN];
    ReportStatus status;
    int          feedback;   
    char         description[DESC_LEN];
} Report;

/* ── Cache ───────────────────────────────────────────────────────────── */

/**
 * @brief Evicts all cache entries that belong to the given city.
 *
 * Called after any write operation (insert / assign / resolve) so that
 * subsequent reads reflect the updated database state.
 *
 * @param city     City whose entries must be invalidated.
 * @param authorId Author ID used to also purge citizen-scoped entries.
 */
void report_cache_invalidate_city(const char *city, uint64_t authorId);

/* ── Setup ───────────────────────────────────────────────────────────── */

/**
 * @brief Creates the reports table and its indexes if they do not exist.
 * @post The table and indexes are present in the database.
 * @return 0 on success, -1 on SQL error.
 */
int report_setup_table(void);

/* ── Write operations ────────────────────────────────────────────────── */

/**
 * @brief Inserts a new report and returns its generated ID.
 *
 * @pre The database is initialised and the reports table exists.
 * @param authorId ID of the submitting citizen.
 * @param lat      Latitude of the issue location.
 * @param lon      Longitude of the issue location.
 * @param city     Municipality name (max CITY_LEN - 1 chars).
 * @param category Issue category (max CAT_LEN - 1 chars).
 * @param desc     Free-text description (max DESC_LEN - 1 chars).
 * @return Generated report ID on success, 0 on error.
 */
uint64_t report_insert(uint64_t authorId, double lat, double lon,
                       const char *city, const char *category, const char *desc);

/**
 * @brief Assigns an active report to an operator (admin action).
 *
 * Sets status to STATUS_ASSIGNED and records the operator; the operator
 * must then call report_accept() or report_reject() to confirm.
 * Only succeeds if the report has status STATUS_ACTIVE and no operator
 * is currently assigned (atomic guard against double-assignment).
 *
 * @param reportId   ID of the report to assign.
 * @param operatorId ID of the operator being assigned.
 * @return 1 if assigned, 0 if the guard condition was not met, -1 on error.
 */
int report_assign(uint64_t reportId, uint64_t operatorId);

/**
 * @brief Accepts an assigned report, moving it to STATUS_IN_PROGRESS.
 *
 * Only succeeds if the report has status STATUS_ASSIGNED and is currently
 * assigned to operatorId.
 *
 * @param reportId   ID of the report to accept.
 * @param operatorId ID of the operator accepting the report.
 * @return 1 if accepted, 0 if guard condition not met, -1 on error.
 */
int report_accept(uint64_t reportId, uint64_t operatorId);

/**
 * @brief Rejects an assigned report, returning it to STATUS_ACTIVE.
 *
 * Clears the operator assignment so the admin can reassign.
 * Only succeeds if the report has status STATUS_ASSIGNED and is assigned
 * to operatorId.
 *
 * @param reportId   ID of the report to reject.
 * @param operatorId ID of the operator rejecting the report.
 * @return 1 if rejected, 0 if guard condition not met, -1 on error.
 */
int report_reject(uint64_t reportId, uint64_t operatorId);

/**
 * @brief Marks an in-progress report as resolved.
 *
 * Only succeeds if the report is currently STATUS_IN_PROGRESS and
 * assigned to operatorId (prevents operators from resolving others' work).
 *
 * @param reportId   ID of the report to resolve.
 * @param operatorId ID of the operator resolving the report.
 * @return 1 if resolved, 0 if the guard condition was not met, -1 on error.
 */
int report_resolve(uint64_t reportId, uint64_t operatorId);

/**
 * @brief Records a citizen's feedback (1–5 stars) on a resolved report.
 *
 * Only the author of the report may set feedback, and only when the
 * report has status STATUS_RESOLVED.  Feedback can be set only once
 * (feedback IS NULL guard prevents overwriting).
 *
 * @param reportId ID of the resolved report.
 * @param authorId ID of the citizen submitting the feedback.
 * @param stars    Rating from 1 to 5 inclusive.
 * @return 1 if stored, 0 if guard condition not met, -1 on error.
 */
int report_set_feedback(uint64_t reportId, uint64_t authorId, int stars);

/* ── Read operations ─────────────────────────────────────────────────── */

/**
 * @brief Serialises active reports for a user into a JSON array.
 *
 * For operators: reports with status IN_PROGRESS assigned to userId.
 * For citizens:  reports authored by userId with status < RESOLVED.
 * Results are served from an in-memory TTL cache when available.
 *
 * @param buf        Output buffer.
 * @param max        Capacity of buf in bytes.
 * @param userId     ID of the requesting user.
 * @param city       City context used as cache key.
 * @param isOperator true if the caller is an operator, false for citizens.
 * @return Number of bytes written to buf (0 on error or empty result).
 */
size_t report_get_active_json(char *buf, size_t max,
                              uint64_t userId, const char *city, bool isOperator);

/**
 * @brief Serialises resolved reports for a user into a JSON array.
 *
 * For operators: reports with status RESOLVED assigned to userId.
 * For citizens:  reports authored by userId with status RESOLVED.
 * Results are served from an in-memory TTL cache when available.
 *
 * @param buf        Output buffer.
 * @param max        Capacity of buf in bytes.
 * @param userId     ID of the requesting user.
 * @param city       City context used as cache key.
 * @param isOperator true if the caller is an operator, false for citizens.
 * @return Number of bytes written to buf (0 on error or empty result).
 */
size_t report_get_archived_json(char *buf, size_t max,
                                uint64_t userId, const char *city, bool isOperator);

/**
 * @brief Serialises all reports for a city into a JSON array (admin view).
 *
 * Not cached; returns every report regardless of status or author.
 *
 * @param buf  Output buffer.
 * @param max  Capacity of buf in bytes.
 * @param city Municipality name to filter by.
 * @return Number of bytes written to buf (0 on error or empty result).
 */
size_t report_get_all_city_json(char *buf, size_t max, const char *city);

/**
 * @brief Fetches a single report by its ID.
 *
 * @param reportId ID of the report to fetch.
 * @param out      Populated on success.
 * @return true if found, false otherwise.
 */
bool report_get_by_id(uint64_t reportId, Report *out);

/**
 * @brief Returns the total number of non-resolved reports in the database.
 * @return Count of reports with status < STATUS_RESOLVED.
 */
int report_count_active(void);


#endif /* REPORT_H */