/**
 * @file route_api.c
 * @brief JSON API handlers.
 *
 * All handlers here produce application/json responses.
 * HTML page handlers live in route_pages.c.
 *
 * Handlers
 * ────────
 *   GET  /api/cities             route_api_cities          (public)
 *   GET  /api/reports/active     route_api_reports_active  (citizen/operator)
 *   GET  /api/reports/archived   route_api_reports_archived(citizen/operator)
 *   GET  /api/reports/all        route_api_reports_all     (admin only)
 *   GET  /api/operators          route_api_operators        (admin only)
 *   POST /api/report/status      route_api_report_status   (operator only)
 *   POST /api/admin/assign       route_api_admin_assign     (admin only)
 */

#include "route_api.h"
#include "route_helpers.h"
#include "report.h"
#include "user.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Public endpoints ────────────────────────────────────────────────── */

void route_api_cities(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    const Template *tpl = tpl_get(CITIES_JSON_PATH);
    if (unlikely(!tpl)) { resp_json_error(resp, 404, "cities not found"); return; }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->body_len     = n > 0 ? (size_t)n : 0;
    resp->status_code  = 200;
    resp->content_type = "application/json";
}

/* ── Citizen / operator endpoints ────────────────────────────────────── */

void route_api_reports_active(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { resp_json_error(resp, 401, "unauthorized"); return; }
    resp->body_len    = report_get_active_json(resp->body, RESPONSE_BUFFER_SIZE,
                                               u.userId, u.city, user_is_operator(&u));
    resp->status_code = 200;
}

void route_api_reports_archived(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { resp_json_error(resp, 401, "unauthorized"); return; }
    resp->body_len    = report_get_archived_json(resp->body, RESPONSE_BUFFER_SIZE,
                                                 u.userId, u.city, user_is_operator(&u));
    resp->status_code = 200;
}

/* ── Operator endpoints ──────────────────────────────────────────────── */

void route_api_report_status(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    char rid_s[REPORT_ID_PARAM_LEN] = {0}, stat_s[STATUS_PARAM_LEN] = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        resp_json_error(resp, 403, "forbidden"); return;
    }

    
    get_field(req->body, "report_id=", rid_s,  sizeof(rid_s));
    get_field(req->body, "status=",    stat_s, sizeof(stat_s));
    if (!rid_s[0] || !stat_s[0]) { resp_json_error(resp, 400, "missing params"); return; }

    uint64_t     report_id  = (uint64_t)strtoull(rid_s, NULL, 10);
    int          new_status = atoi(stat_s);
    ActiveReport r;

    if (!report_get_by_id(report_id, &r))          { resp_json_error(resp, 404, "not found");  return; }
    if (strncmp(r.city, u.city, CITY_LEN) != 0)    { resp_json_error(resp, 403, "forbidden");  return; }

    int rc;
    if (new_status == STATUS_IN_PROGRESS) {
        rc = report_assign(report_id, u.userId);
        if (rc == 0) { resp_json_error(resp, 409, "report already taken"); return; }
    } else if (new_status == STATUS_RESOLVED) {
        rc = report_resolve(report_id, u.userId);
        if (rc == 0) { resp_json_error(resp, 403, "not authorized or already resolved"); return; }
    } else {
        resp_json_error(resp, 400, "invalid status"); return;
    }

    if (unlikely(rc < 0)) { resp_json_error(resp, 500, "db error"); return; }

    //report_cache_invalidate_city(r.city, r.authorId); //TODO: evita qui e metti in report_* qui sopra
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->body_len    = strlen(resp->body);
    resp->status_code = 200;
}

/* ── Admin endpoints ─────────────────────────────────────────────────── */

void route_api_reports_all(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden"); return;
    }
    resp->body_len    = report_get_all_city_json(resp->body, RESPONSE_BUFFER_SIZE, u.city);
    resp->status_code = 200;
}

void route_api_operators(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden"); return;
    }
    resp->body_len    = user_get_operators_json(resp->body, RESPONSE_BUFFER_SIZE, u.city);
    resp->status_code = 200;
}

void route_api_admin_assign(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_admin(&u)) {
        resp_json_error(resp, 403, "forbidden"); return;
    }

    char rid_s[REPORT_ID_PARAM_LEN] = {0}, op_s[OPERATOR_ID_PARAM_LEN] = {0};
    get_field(req->body, "report_id=",   rid_s, sizeof(rid_s));
    get_field(req->body, "operator_id=", op_s,  sizeof(op_s));
    if (!rid_s[0] || !op_s[0]) { resp_json_error(resp, 400, "missing params"); return; }

    uint64_t     report_id   = (uint64_t)strtoull(rid_s, NULL, 10);
    uint64_t     operator_id = (uint64_t)strtoull(op_s,  NULL, 10);
    ActiveReport r;

    if (unlikely(!report_get_by_id(report_id, &r)))   { resp_json_error(resp, 404, "not found");       return; }
    if (strncmp(r.city, u.city, CITY_LEN) != 0)       { resp_json_error(resp, 403, "forbidden");        return; }

    User op = {0};
    if (!user_get_by_id(operator_id, &op)
            || !user_is_operator(&op)
            || strncmp(op.city, u.city, CITY_LEN) != 0) {
        resp_json_error(resp, 400, "invalid operator"); return;
    }

    int rc = report_assign(report_id, operator_id);
    if (rc == 0)          { resp_json_error(resp, 409, "report already taken"); return; }
    if (unlikely(rc < 0)) { resp_json_error(resp, 500, "db error");             return; }

    //report_cache_invalidate_city(r.city, r.authorId);
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->body_len    = strlen(resp->body);
    resp->status_code = 200;
}
