/**
 * @file route_api.h
 * @brief Declarations for the JSON API route handlers.
 *
 * Internal to the routing subsystem — include only from route_handler.c.
 * All handlers write an application/json body into resp->body.
 */

#ifndef ROUTE_API_H
#define ROUTE_API_H

#include "http_types.h"

// Buffer sizes for URL-decoded POST fields. 24 chars is enough for any
// uint64_t in decimal (max 20 digits) plus a NUL terminator and margin.
#define REPORT_ID_PARAM_LEN   24
#define OPERATOR_ID_PARAM_LEN 24
// Status is a single decimal digit ("0", "1" or "2") plus NUL.
#define STATUS_PARAM_LEN      4

/**
 * @brief Returns the list of available cities as a JSON array.
 *
 * Reads the pre-built cities JSON file via the template cache.
 * Access: public (no session required).
 */
void route_api_cities(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Returns active reports for the authenticated user as a JSON array.
 *
 * For operators: reports assigned to them with status IN_PROGRESS.
 * For citizens:  their own reports with status < RESOLVED.
 * Access: any authenticated user.
 */
void route_api_reports_active(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Returns archived (resolved) reports for the authenticated user.
 *
 * For operators: reports they resolved.
 * For citizens:  their own resolved reports.
 * Access: any authenticated user.
 */
void route_api_reports_archived(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Advances the status of a report (resolve only — status=3).
 *
 * Reads report_id and status from the POST body.
 * Operators may only mark a report as resolved via this endpoint.
 * Accept/reject go through /api/report/respond.
 * Access: operators only.
 */
void route_api_report_status(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Accepts or rejects an assignment made by an admin.
 *
 * Reads report_id and action ("accept"|"reject") from the POST body.
 * Accept → STATUS_IN_PROGRESS; reject → STATUS_ACTIVE (unassigned).
 * Access: operators only.
 */
void route_api_report_respond(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Records a citizen's feedback (1-5 stars) on a resolved report.
 *
 * Reads report_id and stars from the POST body.
 * Only the report's author may submit feedback, only once, only when resolved.
 * Access: citizens only.
 */
void route_api_report_feedback(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Returns all reports for the admin's city as a JSON array.
 *
 * Not cached; includes reports of every status.
 * Access: admins only.
 */
void route_api_reports_all(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Returns all operators belonging to the admin's city as a JSON array.
 *
 * Access: admins only.
 */
void route_api_operators(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Assigns a report to a specific operator (admin action).
 *
 * Reads report_id and operator_id from the POST body.
 * Validates that both the report and the operator belong to the admin's city.
 * Access: admins only.
 */
void route_api_admin_assign(const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_API_H */