/**
 * @file route_api.c
 * @brief JSON API handlers.
 *
 * All handlers here produce application/json responses.
 * HTML page handlers live in route_pages.c.
 *
 * Handlers
 * ────────
 *   GET  /api/cities             route_api_cities           (public)
 *   GET  /api/reports/active     route_api_reports_active   (citizen/operator)
 *   GET  /api/reports/archived   route_api_reports_archived (citizen/operator)
 *   GET  /api/reports/all        route_api_reports_all      (admin only)
 *   GET  /api/operators          route_api_operators        (admin only)
 *   POST /api/report/status      route_api_report_status    (operator only)
 *   POST /api/admin/assign       route_api_admin_assign     (admin only)
 */

#include "route_api.h"
#include "route_helpers.h"
#include "report.h"
#include "user.h"
#include "db.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Public endpoints ────────────────────────────────────────────────── */

void route_api_cities(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    // The cities list is a static JSON file served through the template cache.
    const Template *tpl = tpl_get(CITIES_JSON_PATH);
    if (unlikely(!tpl)) {
        resp_json_error(resp, 404, "cities not found");
        return;
    }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->bodyLen = n > 0 ? (size_t)n : 0;
    resp->statusCode = 200;
    resp->contentType = "application/json";
}

/* ── Citizen / operator endpoints ────────────────────────────────────── */

void route_api_reports_active(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) {
        resp_json_error(resp, 401, "unauthorized");
        return;
    }
    // report_get_active_json selects a different query based on isOperator.
    resp->bodyLen = report_get_active_json(resp->body, RESPONSE_BUFFER_SIZE,
                                           u.userId, u.city, user_is_operator(&u));
    resp->statusCode = 200;
}

void route_api_reports_archived(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) {
        resp_json_error(resp, 401, "unauthorized");
        return;
    }
    resp->bodyLen = report_get_archived_json(resp->body, RESPONSE_BUFFER_SIZE,
                                             u.userId, u.city, user_is_operator(&u));
    resp->statusCode = 200;
}

/* ── Operator endpoints ──────────────────────────────────────────────── */

void route_api_report_status(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    // Auth check: must be an authenticated operator.
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    // Parse the two required POST fields from the URL-encoded body.
    char reportIds[REPORT_ID_PARAM_LEN] = {0}, status[STATUS_PARAM_LEN] = {0};
    get_field(req->body, "report_id=", reportIds, sizeof(reportIds));
    get_field(req->body, "status=",    status, sizeof(status));
    if (!reportIds[0] || !status[0]) {
        resp_json_error(resp, 400, "missing params");
        return;
    }

    uint64_t reportId = (uint64_t)strtoull(reportIds, NULL, 10);
    int newStatus = atoi(status);
    Report r;

    // Verify the report exists before checking ownership.
    if (!report_get_by_id(reportId, &r)) {
        resp_json_error(resp, 404, "not found");
        return;
    }
    // City guard: operators may only act on reports in their own city.
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    int rc;
    if (newStatus == STATUS_RESOLVED) {
        // report_resolve checks that the report is STATUS_IN_PROGRESS (2)
        // and assigned to this specific operator.
        rc = report_resolve(reportId, u.userId);
        if (rc == 0) {
            resp_json_error(resp, 403, "not authorized or not in progress");
            return;
        }
    } else {
        // Operators may only mark resolved (status=3).
        // Accept/reject go through /api/report/respond.
        resp_json_error(resp, 400, "invalid status");
        return;
    }

    if (unlikely(rc < 0)) {
        resp_json_error(resp, 500, "db error");
        return;
    }

    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->bodyLen = strlen(resp->body);
    resp->statusCode = 200;
}

/* Accept or reject an assignment (operator action).
 * POST body: report_id=<id>&action=accept|reject
 */
void route_api_report_respond(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    char reportIds[REPORT_ID_PARAM_LEN] = {0}, action[8] = {0};
    get_field(req->body, "report_id=", reportIds, sizeof(reportIds));
    get_field(req->body, "action=",    action,    sizeof(action));
    if (!reportIds[0] || !action[0]) {
        resp_json_error(resp, 400, "missing params");
        return;
    }

    uint64_t reportId = (uint64_t)strtoull(reportIds, NULL, 10);
    Report r;

    if (!report_get_by_id(reportId, &r)) {
        resp_json_error(resp, 404, "not found");
        return;
    }
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    int rc;
    if (strncmp(action, "accept", 6) == 0) {
        rc = report_accept(reportId, u.userId);
        if (rc == 0) { resp_json_error(resp, 409, "not assigned to you or wrong status"); return; }
    } else if (strncmp(action, "reject", 6) == 0) {
        rc = report_reject(reportId, u.userId);
        if (rc == 0) { resp_json_error(resp, 409, "not assigned to you or wrong status"); return; }
    } else {
        resp_json_error(resp, 400, "action must be accept or reject");
        return;
    }

    if (unlikely(rc < 0)) {
        resp_json_error(resp, 500, "db error");
        return;
    }

    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->bodyLen = strlen(resp->body);
    resp->statusCode = 200;
}

void route_api_report_feedback(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    // Only authenticated citizens may submit feedback.
    if (!get_session_user(req, &u) || user_is_operator(&u) || user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    char reportIds[REPORT_ID_PARAM_LEN] = {0}, starsStr[4] = {0};
    get_field(req->body, "report_id=", reportIds, sizeof(reportIds));
    get_field(req->body, "stars=",     starsStr, sizeof(starsStr));
    if (!reportIds[0] || !starsStr[0]) {
        resp_json_error(resp, 400, "missing params");
        return;
    }

    uint64_t reportId = (uint64_t)strtoull(reportIds, NULL, 10);
    int stars = atoi(starsStr);
    if (stars < 1 || stars > 5) {
        resp_json_error(resp, 400, "stars must be 1-5");
        return;
    }

    int rc = report_set_feedback(reportId, u.userId, stars);
    if (rc == 0) {
        resp_json_error(resp, 409, "feedback already given or report not resolved");
        return;
    }
    if (rc < 0) {
        resp_json_error(resp, 500, "db error");
        return;
    }

    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->bodyLen = strlen(resp->body);
    resp->statusCode = 200;
}

/* ── Admin endpoints ─────────────────────────────────────────────────── */

void route_api_reports_all(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }
    // No cache: admin view must always reflect the current database state.
    resp->bodyLen = report_get_all_city_json(resp->body, RESPONSE_BUFFER_SIZE, u.city);
    resp->statusCode = 200;
}

void route_api_operators(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }
    resp->bodyLen = user_get_operators_json(resp->body, RESPONSE_BUFFER_SIZE, u.city);
    resp->statusCode = 200;
}

void route_api_admin_assign(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    char reportIds[REPORT_ID_PARAM_LEN] = {0}, operatorIds[OPERATOR_ID_PARAM_LEN] = {0};
    get_field(req->body, "report_id=",   reportIds, sizeof(reportIds));
    get_field(req->body, "operator_id=", operatorIds,  sizeof(operatorIds));
    if (!reportIds[0] || !operatorIds[0]) {
        resp_json_error(resp, 400, "missing params");
        return;
    }

    uint64_t reportId = (uint64_t)strtoull(reportIds, NULL, 10);
    uint64_t operatorId = (uint64_t)strtoull(operatorIds, NULL, 10);
    Report r;

    if (unlikely(!report_get_by_id(reportId, &r))) {
        resp_json_error(resp, 404, "not found");
        return;
    }
    // City guard: admins may only assign reports within their own city.
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        resp_json_error(resp, 403, "forbidden");
        return;
    }

    // Three-way operator validation: must exist, be an operator role,
    // and belong to the same city as the admin performing the action.
    User op = {0};
    if (!user_get_by_id(operatorId, &op)
            || !user_is_operator(&op)
            || strncmp(op.city, u.city, CITY_LEN) != 0) {
        resp_json_error(resp, 400, "invalid operator");
        return;
    }

    int rc = report_assign(reportId, operatorId);
    // rc == 0: the report was not STATUS_ACTIVE (guard failed: already in progress or resolved).
    if (rc == 0) {
        // If it's STATUS_ASSIGNED (1), force-reassign by resetting to ACTIVE first.
        Report rr;
        if (report_get_by_id(reportId, &rr) && rr.status == STATUS_ASSIGNED) {
            // Reset to active then assign to the new operator.
            db_exec("UPDATE reports SET status=0, assigned_to=NULL, assigned_at=NULL WHERE id=? AND status=1;",
                    "l", (int64_t)reportId);
            rc = report_assign(reportId, operatorId);
        }
        if (rc == 0) {
            resp_json_error(resp, 409, "report already in progress or resolved");
            return;
        }
    }
    if (unlikely(rc < 0)) {
        resp_json_error(resp, 500, "db error");
        return;
    }

    // Cache invalidation is handled inside report_assign.
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->bodyLen = strlen(resp->body);
    resp->statusCode = 200;
}