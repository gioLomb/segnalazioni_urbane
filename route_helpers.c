/**
 * @file route_helpers.c
 * @brief Implementation of session, response, and map helpers.
 */

#include "route_helpers.h"
#include <string.h>
#include <stdio.h>

/* ── Session ─────────────────────────────────────────────────────────── */

bool get_session_user(const HttpRequest *req, User *u) {
    char token[TOKEN_HEX_LEN + 1];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (!token[0]) return false;
    return session_verify(token, u);
}

/* ── Response primitives ─────────────────────────────────────────────── */

void redirect(HttpResponse *resp, const char *url, const char *cookie) {
    resp->statusCode = 302;
    snprintf(resp->location, sizeof(resp->location), "%s", url);
    if (cookie)
        snprintf(resp->setCookie, sizeof(resp->setCookie), "%s", cookie);
}

void resp_json_error(HttpResponse *resp, int status, const char *msg) {
    resp->statusCode = status;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"%s\"}", msg);
    resp->bodyLen    = strlen(resp->body);
}

void resp_html_error(HttpResponse *resp, int status, const char *msg) {
    resp->statusCode = status;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>%s</h1>", msg);
    resp->bodyLen    = strlen(resp->body);
}

/* ── Page-level error helpers ────────────────────────────────────────── */

void login_error(HttpResponse *resp, const char *msg) {
    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = { { "ERROR_BLOCK", eb } };
    resp_render_tpl(resp, "templates/login.html", vars, 1);
}

void register_error(HttpResponse *resp, const char *msg) {
    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = { { "ERROR_BLOCK", eb } };
    resp_render_tpl(resp, "templates/register.html", vars, 1);
}

void submit_error(HttpResponse *resp, const User *u, const char *msg) {
    char esc_user[ESCAPED_PARAM_LEN], esc_city[ESCAPED_PARAM_LEN];
    html_escape(u->username, esc_user, sizeof(esc_user));
    html_escape(u->city,     esc_city, sizeof(esc_city));
    MapVars mv;
    build_map_vars(u->city, &mv);
    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = {
        { "USERNAME",    esc_user  }, { "CITY",       esc_city  },
        { "ERROR_BLOCK", eb        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    resp_render_tpl(resp, "templates/submit.html", vars, 6);
}

/* ── Map vars ────────────────────────────────────────────────────────── */

void build_map_vars(const char *city_name, MapVars *mv) {
    CityGeo geo;
    if (geo_lookup(city_name, &geo)) {
        snprintf(mv->lat,    sizeof(mv->lat),    "%.6f", geo.centroidLat);
        snprintf(mv->lon,    sizeof(mv->lon),    "%.6f", geo.centroidLon);
        snprintf(mv->bounds, sizeof(mv->bounds),
                 "[[%.6f,%.6f],[%.6f,%.6f]]",
                 geo.latMin, geo.lonMin, geo.latMax, geo.lonMax);
    } else {
        snprintf(mv->lat,    sizeof(mv->lat),    "41.9");
        snprintf(mv->lon,    sizeof(mv->lon),    "12.5");
        snprintf(mv->bounds, sizeof(mv->bounds), "null");
    }
}