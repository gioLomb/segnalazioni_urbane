/**
 * @file route_helpers.h
 * @brief Internal helpers shared by route_pages.c and route_api.c.
 *
 * Not part of the public API — include only from route_*.c files.
 * The public entry point remains handle_request() in route_handler.h.
 */

#ifndef ROUTE_HELPERS_H
#define ROUTE_HELPERS_H

#include "http_types.h"
#include "http_utils.h"
#include "template.h"
#include "user.h"
#include "session.h"
#include "geo.h"
#include "config.h"

#define ERROR_BLOCK_MAX_LEN 128
#define ESCAPED_PARAM_LEN 64

/* ── Session ─────────────────────────────────────────────────────────── */

/**
 * Reads the session cookie from req, verifies it, and fills *u.
 * Returns true if a valid session was found, false otherwise.
 */
bool get_session_user(const HttpRequest *req, User *u);

/* ── Response primitives ─────────────────────────────────────────────── */

/** HTTP 302 redirect. cookie may be NULL. */
void redirect(HttpResponse *resp, const char *url, const char *cookie);

/** Writes {"error":"<msg>"} into resp with the given status code. */
void resp_json_error(HttpResponse *resp, int status, const char *msg);

/** Writes <h1><msg></h1> into resp with the given status code. */
void resp_html_error(HttpResponse *resp, int status, const char *msg);

/**
 * Renders a template into resp->body and sets status 200.
 * On tpl_get failure writes a 500 and returns false.
 * Inline because it is called in almost every handler and the
 * compiler can eliminate the call overhead entirely.
 */
static inline bool resp_render_tpl(HttpResponse   *resp,
                                   const char     *tpl_name,
                                   TplVar         *vars,
                                   int             n_vars) {
    const Template *tpl = tpl_get(tpl_name);
    if (unlikely(!tpl)) { resp_html_error(resp, 500, "Server Error"); return false; }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, n_vars);
    resp->status_code = 200;
    resp->body_len    = n > 0 ? (size_t)n : 0;
    return true;
}

/* ── Page-level error helpers ────────────────────────────────────────── */

/** Renders login.html with ERROR_BLOCK set to msg. */
void login_error(HttpResponse *resp, const char *msg);

/** Renders register.html with ERROR_BLOCK set to msg. */
void register_error(HttpResponse *resp, const char *msg);

/** Renders submit.html with all vars + ERROR_BLOCK set to msg. */
void submit_error(HttpResponse *resp, const User *u, const char *msg);

/* ── Map vars ────────────────────────────────────────────────────────── */

typedef struct { char lat[32]; char lon[32]; char bounds[128]; } MapVars;

/** Fills mv with geo data for city_name, or falls back to Rome centre. */
void build_map_vars(const char *city_name, MapVars *mv);

#endif /* ROUTE_HELPERS_H */