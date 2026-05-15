/**
 * @file route_pages.h
 * @brief Declarations for HTML page and form handlers.
 *
 * These are internal to the routing subsystem — include only from
 * route_handler.c (the dispatch table).
 */

#ifndef ROUTE_PAGES_H
#define ROUTE_PAGES_H

#include "http_types.h"

void route_get_root      (const HttpRequest *req, HttpResponse *resp);
void route_get_register  (const HttpRequest *req, HttpResponse *resp);
void route_get_home      (const HttpRequest *req, HttpResponse *resp);
void route_get_submit    (const HttpRequest *req, HttpResponse *resp);
void route_get_logout    (const HttpRequest *req, HttpResponse *resp);
void route_post_login    (const HttpRequest *req, HttpResponse *resp);
void route_post_register (const HttpRequest *req, HttpResponse *resp);
void route_post_submit   (const HttpRequest *req, HttpResponse *resp);
void route_static_css    (const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_PAGES_H */
