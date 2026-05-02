/**
 * route_handler.h — HTTP dispatcher interface
 *
 * handle_request() is the single entry point called by server_functions.c
 * for every complete HTTP request received from a client.
 *
 * Before calling handle_request() the server must call tpl_load_all()
 * (declared in template.h) so that the HTML templates are available.
 */

#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include "config.h"
#include <stdbool.h>

/* Maximum length of a Set-Cookie header value. */
#define COOKIE_MAX 512

/**
 * Out-of-band metadata a route handler may attach to a response.
 * server_functions.c inspects these after handle_request() returns.
 *
 *   set_cookie — non-empty → add "Set-Cookie: <value>" to the response header.
 *   location   — non-empty → send a 302 redirect; the body is ignored.
 */
typedef struct {
    char set_cookie[COOKIE_MAX];
    char location[256];
} RouteExtra;

/**
 * Signature shared by every route handler function.
 *
 * @param req      NUL-terminated raw HTTP request buffer (read-only).
 * @param resp     Output buffer for the response body.
 * @param resp_max Capacity of resp in bytes.
 * @param extra    Output struct for cookies and redirects; pre-zeroed by
 *                 handle_request() before the handler is called.
 * @return         HTTP status code (200, 302, 400, 401, 403, 404, 405, 500…).
 */
typedef int (*RouteHandler)(const char *req,
                            char       *resp,
                            size_t      resp_max,
                            RouteExtra *extra);

/**
 * A single entry in the dispatch table.
 * method is "GET" or "POST"; path is an exact URL path (no wildcards).
 */
typedef struct {
    const char  *method;
    const char  *path;
    RouteHandler handler;
} Route;

/**
 * Dispatches req to the matching route handler, populates resp and extra,
 * and sets *keep_alive based on the Connection header.
 *
 * Returns the HTTP status code produced by the handler, or:
 *   400 — malformed request line
 *   404 — no route matches the path
 *   405 — path matched but wrong HTTP method
 *
 * g_sessions (Hash_Table*) is accessed via the extern declared in
 * server_functions.h; no additional parameter is needed.
 */
int handle_request(const char *req,
                   char       *resp,
                   size_t      resp_max,
                   RouteExtra *extra,
                   int        *keep_alive);

#endif /* ROUTE_HANDLER_H */