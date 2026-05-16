/**
 * @file route_handler.c
 * @brief Dispatch table and handle_request() entry point.
 *
 * This file contains only the routing logic: a static table that maps
 * (method, path) pairs to handler functions, and handle_request() which
 * walks it.  All business logic lives in route_pages.c and route_api.c.
 */

#include "route_handler.h"
#include "route_pages.h"
#include "route_api.h"
#include "route_helpers.h"
#include <string.h>
#include <stdio.h>

/* ── Dispatch table ──────────────────────────────────────────────────── */

static const Route routes[] = {
    { .method = "GET",  .path = "/",                     .handler = route_get_root             },
    { .method = "GET",  .path = "/home",                 .handler = route_get_home             },
    { .method = "GET",  .path = "/register",             .handler = route_get_register         },
    { .method = "GET",  .path = "/submit",               .handler = route_get_submit           },
    { .method = "GET",  .path = "/logout",               .handler = route_get_logout           },
    { .method = "POST", .path = "/login",                .handler = route_post_login           },
    { .method = "POST", .path = "/register",             .handler = route_post_register        },
    { .method = "POST", .path = "/submit",               .handler = route_post_submit          },
    { .method = "GET",  .path = "/api/cities",           .handler = route_api_cities           },
    { .method = "GET",  .path = "/api/reports/active",   .handler = route_api_reports_active   },
    { .method = "GET",  .path = "/api/reports/archived", .handler = route_api_reports_archived },
    { .method = "GET",  .path = "/api/reports/all",      .handler = route_api_reports_all      },
    { .method = "GET",  .path = "/api/operators",        .handler = route_api_operators        },
    { .method = "POST", .path = "/api/report/status",    .handler = route_api_report_status    },
    { .method = "POST", .path = "/api/admin/assign",     .handler = route_api_admin_assign     },
    { .method = "GET",  .path = "/static/common.css",    .handler = route_static_css           }
};

static const size_t NUM_ROUTES = sizeof(routes) / sizeof(routes[0]);

/* ── Public API ──────────────────────────────────────────────────────── */

// bool route_needs_large(const HttpRequest *req) {
//     for (size_t i = 0; i < NUM_ROUTES; i++) {
//         if (http_is_request_method(req, routes[i].method) &&
//             http_is_request_path(req, routes[i].path))
//             return routes[i].needs_large;
//     }
//     return false;
// }

void handle_request(const HttpRequest *req, HttpResponse *resp) {
    bool path_found = false;

    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (!http_is_request_path(req, routes[i].path)) continue;
        path_found = true;
        if (http_is_request_method(req, routes[i].method)) {
            routes[i].handler(req, resp);
            return;
        }
    }

    if (path_found) {
        resp_html_error(resp, 405, "405 Method Not Allowed");
    } else {
        char path[URL_BUFFER_SIZE + 1];
        size_t n = req->pathLen < URL_BUFFER_SIZE ? req->pathLen : URL_BUFFER_SIZE;
        memcpy(path, req->path, n);
        path[n] = '\0';
        snprintf(resp->body, RESPONSE_BUFFER_SIZE,
                 "<h1>404 Not Found</h1><p><code>%s</code> non esiste.</p>", path);
        resp->status_code = 404;
        resp->body_len    = strlen(resp->body);
    }
}