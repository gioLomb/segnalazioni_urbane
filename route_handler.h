/**
 * route_handler.h — HTTP dispatcher interface
 *
 * handle_request() is the single entry point called by server_functions.c
 * for every complete HTTP request.  It takes a fully-parsed HttpRequest and
 * fills an HttpResponse — no raw buffers, no RouteExtra side-channel.
 */

#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include "http_types.h"
#include "http_utils.h"



/**
 * Signature shared by every route handler.
 * Handlers read from req and write into resp (status_code, body,
 * set_cookie, location, content_type).
 */
typedef void (*RouteHandler)(const HttpRequest *req, HttpResponse *resp);

/** A single entry in the dispatch table. */
typedef struct {
    const char  *method;   /* "GET" or "POST" */
    const char  *path;     /* exact URL path  */
    RouteHandler handler;
} Route;

/**
 * Dispatches req to the matching handler and fills resp.
 * Sets resp->status_code to 404 or 405 when no handler matches.
 */
void handle_request(const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_HANDLER_H */