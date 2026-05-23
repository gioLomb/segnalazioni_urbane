/**
 * @file route_helpers.c
 * @brief Implementation of session, response, and template helpers.
 */

#include "route_helpers.h"
#include <string.h>
#include <stdio.h>

/* ── Session ─────────────────────────────────────────────────────────── */

bool get_session_user(const HttpRequest *req, User *u) {
    // Extract the session token from the cookie header into a local buffer.
    char token[TOKEN_HEX_LEN + 1];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    // Empty token means the cookie was absent or too short to be valid.
    if (!token[0]) return false;
    return session_verify(token, u);
}

/* ── Response primitives ─────────────────────────────────────────────── */

void redirect(HttpResponse *resp, const char *url, const char *cookie) {
    resp->statusCode = 302;
    snprintf(resp->location, sizeof(resp->location), "%s", url);
    // cookie is optional: set it only when the caller provides one
    // (e.g. to plant the session token on successful login).
    if (cookie) snprintf(resp->setCookie, sizeof(resp->setCookie), "%s", cookie);
}

void resp_json_error(HttpResponse *resp, int status, const char *msg) {
    resp->statusCode = status;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"%s\"}", msg);
    resp->bodyLen = strlen(resp->body);
}

void resp_html_error(HttpResponse *resp, int status, const char *msg) {
    resp->statusCode = status;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>%s</h1>", msg);
    resp->bodyLen = strlen(resp->body);
}

bool resp_render_tpl(HttpResponse *resp, const char *tplName,
                     TplVar *vars, int nVars) {
    const Template *tpl = tpl_get(tplName);
    if (unlikely(!tpl)) {
        resp_html_error(resp, 500, "Server Error");
        return false;
    }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, nVars);
    resp->statusCode = 200;
    resp->bodyLen = n > 0 ? (size_t)n : 0;
    return true;
}

/* ── Page-level error helpers ────────────────────────────────────────── */

void login_error(HttpResponse *resp, const char *msg) {
    // Build the HTML error block once and reuse it as a template variable.
    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = { { "ERROR_BLOCK", eb } };
    resp_render_tpl(resp, TPL_LOGIN, vars, 1);
}

void register_error(HttpResponse *resp, const char *msg) {
    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = { { "ERROR_BLOCK", eb } };
    resp_render_tpl(resp, TPL_REGISTER, vars, 1);
}

void submit_error(HttpResponse *resp, const User *u, const char *msg) {
    // HTML-escape user-controlled strings before embedding them in the page.
    char escUser[ESCAPED_PARAM_LEN], escCity[ESCAPED_PARAM_LEN];
    html_escape(u->username, escUser, sizeof(escUser));
    html_escape(u->city, escCity, sizeof(escCity));

    // Resolve centroid and bounding box for the Leaflet map initialisation.
    MapVars mv;
    build_map_vars(u->city, &mv);

    char eb[ERROR_BLOCK_MAX_LEN];
    make_error_block(msg, eb, sizeof(eb));

    TplVar vars[] = {
        { "USERNAME",    escUser   }, { "CITY",       escCity   },
        { "ERROR_BLOCK", eb        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    resp_render_tpl(resp, TPL_SUBMIT, vars, 6);
}