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

// Maximum length for the HTML error block injected into page templates.
#define ERROR_BLOCK_MAX_LEN 128
// Maximum length for an HTML-escaped username or city name.
#define ESCAPED_PARAM_LEN   64

/* ── Session ─────────────────────────────────────────────────────────── */

/**
 * @brief Extracts and verifies the session cookie from the request.
 *
 * Reads SESSION_COOKIE_NAME from the Cookie header, then delegates to
 * session_verify() which looks up the token in the session table.
 *
 * @param req Incoming HTTP request.
 * @param u   Populated with the authenticated user's data on success.
 * @return true if a valid session exists, false if missing or expired.
 */
bool get_session_user(const HttpRequest *req, User *u);

/* ── Response primitives ─────────────────────────────────────────────── */

/**
 * @brief Issues an HTTP 302 redirect.
 *
 * @param resp   Response to populate.
 * @param url    Destination URL written into resp->location.
 * @param cookie Optional Set-Cookie header value; pass NULL to omit it.
 */
void redirect(HttpResponse *resp, const char *url, const char *cookie);

/**
 * @brief Writes a JSON error body {"error":"<msg>"} with the given status.
 *
 * @param resp   Response to populate.
 * @param status HTTP status code (e.g. 400, 401, 403, 404).
 * @param msg    Error message string (not HTML-escaped).
 */
void resp_json_error(HttpResponse *resp, int status, const char *msg);

/**
 * @brief Writes a minimal HTML error body <h1><msg></h1> with the given status.
 *
 * @param resp   Response to populate.
 * @param status HTTP status code.
 * @param msg    Error message string.
 */
void resp_html_error(HttpResponse *resp, int status, const char *msg);

/**
 * @brief Renders a named template into resp->body and sets status 200.
 *
 * @pre tplName refers to a file accessible via tpl_get().
 * @param resp    Response to populate.
 * @param tplName Path to the template file (used as the cache key).
 * @param vars    Array of template variable bindings.
 * @param nVars   Number of elements in vars.
 * @return true on success, false if the template is not found (sets 500).
 */
bool resp_render_tpl(HttpResponse *resp, const char *tplName,
                     TplVar *vars, int nVars);

/* ── Page-level error helpers ────────────────────────────────────────── */

/**
 * @brief Renders login.html with ERROR_BLOCK injected.
 * @param resp Response to populate.
 * @param msg  Error message to display inside the page.
 */
void login_error(HttpResponse *resp, const char *msg);

/**
 * @brief Renders register.html with ERROR_BLOCK injected.
 * @param resp Response to populate.
 * @param msg  Error message to display inside the page.
 */
void register_error(HttpResponse *resp, const char *msg);

/**
 * @brief Renders submit.html with all map variables and ERROR_BLOCK injected.
 *
 * @param resp Response to populate.
 * @param u    Authenticated user; provides USERNAME, CITY and map coordinates.
 * @param msg  Error message to display inside the page.
 */
void submit_error(HttpResponse *resp, const User *u, const char *msg);

#endif /* ROUTE_HELPERS_H */