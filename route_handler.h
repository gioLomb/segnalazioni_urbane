/**
 * @file route_handler.h
 * @brief Public interface of the HTTP routing subsystem.
 *
 * handle_request() is the single entry point called by server.c
 * for every complete HTTP request.
 *
 * Callers do not need to include route_pages.h, route_api.h, or
 * route_helpers.h — those are internal to the routing subsystem.
 */

#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include "http_types.h"
#include "http_utils.h"
#include "config.h"
#include <stdbool.h>

/**
 * @brief Signature shared by every route handler.
 *
 * Handlers read from req and write their response into resp.
 * They must always set resp->statusCode, resp->bodyLen and resp->body.
 */
typedef void (*RouteHandler)(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief Single entry in the static dispatch table.
 */
typedef struct {
    const char  *method;   /**< HTTP method string, e.g. "GET" or "POST"  */
    const char  *path;     /**< Exact URL path to match against            */
    RouteHandler handler;  /**< Handler invoked on a full (method, path) match */
} Route;

/**
 * @brief Dispatches a request to the matching handler and fills resp.
 *
 * Walks the dispatch table checking path first, then method, so that an
 * unknown method on a known path returns 405 rather than 404.
 *
 * @pre req->path and req->method are valid null-terminated strings.
 * @post resp->statusCode is always set: handler's value on match,
 *       405 on method mismatch, 404 if the path is not in the table.
 * @param req  Incoming HTTP request (read-only).
 * @param resp Response to populate.
 */
void handle_request(const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_HANDLER_H */