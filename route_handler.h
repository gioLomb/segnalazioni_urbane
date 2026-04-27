#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include "config.h"

#define COOKIE_MAX 512

/*
 * Extra out-of-band data a handler may attach to a response.
 * server_functions.c reads these after handle_request() returns.
 *
 *   set_cookie – non-empty → add "Set-Cookie: <set_cookie>" header
 *   location   – non-empty → 302 redirect; body is ignored
 */
typedef struct {
    char set_cookie[COOKIE_MAX];
    char location[256];
} RouteExtra;

typedef struct {
    const char *method; /* "GET", "POST", or "*" for both */
    const char *path;
    int (*handler)(const char *req, char *resp, size_t respMax, RouteExtra *extra);
} Route;

/*
 * Dispatches the HTTP request in req to the matching route, fills resp,
 * populates extra if needed, and sets *keepAlive.
 * Returns an HTTP status code (200, 302, 400, 401, 404, 405, 500, ...).
 * g_sessions is accessed directly from server_functions.c via extern.
 */
int handle_request(const char *req, char *resp, size_t respMax,
                   RouteExtra *extra, int *keepAlive);

#endif /* ROUTE_HANDLER_H */