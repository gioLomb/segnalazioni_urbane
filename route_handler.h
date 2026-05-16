/**
 * @file route_handler.h
 * @brief Public interface of the HTTP routing subsystem.
 *
 * handle_request() is the single entry point called by server_functions.c
 * for every complete HTTP request.  route_needs_large() is called before
 * dispatch to pick the right response buffer tier.
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

/** Signature shared by every route handler. */
typedef void (*RouteHandler)(const HttpRequest *req, HttpResponse *resp);

/** Single entry in the dispatch table. */
typedef struct {
    const char  *method;      /* "GET" or "POST"                               */
    const char  *path;        /* exact URL path                                */
    RouteHandler handler;
    //bool         needs_large; /* true = response may exceed RESP_SMALL_SIZE    */
} Route;

/**
 * Returns true if the route matching req is expected to produce a response
 * larger than RESP_SMALL_SIZE.  Called by read_cb before dispatch to pick
 * the correct buffer tier from resp_pool.
 */
//bool route_needs_large(const HttpRequest *req);

/**
 * Dispatches req to the matching handler and fills resp.
 * Sets resp->statusCode to 404 or 405 when no handler matches.
 */
void handle_request(const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_HANDLER_H */