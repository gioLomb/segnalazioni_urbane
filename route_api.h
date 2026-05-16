/**
 * @file route_api.h
 * @brief Declarations for JSON API handlers.
 *
 * Internal to the routing subsystem — include only from route_handler.c.
 */

#ifndef ROUTE_API_H
#define ROUTE_API_H

#include "http_types.h"

#define REPORT_ID_PARAM_LEN 24
#define STATUS_PARAM_LEN 4
#define OPERATOR_ID_PARAM_LEN 24

void route_api_cities           (const HttpRequest *req, HttpResponse *resp);
void route_api_reports_active   (const HttpRequest *req, HttpResponse *resp);
void route_api_reports_archived (const HttpRequest *req, HttpResponse *resp);
void route_api_report_status    (const HttpRequest *req, HttpResponse *resp);
void route_api_reports_all      (const HttpRequest *req, HttpResponse *resp);
void route_api_operators        (const HttpRequest *req, HttpResponse *resp);
void route_api_admin_assign     (const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_API_H */
