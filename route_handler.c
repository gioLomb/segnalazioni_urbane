/**
 * @file route_handler.c
 * @brief Dispatch table and handle_request() entry point.
 *
 * This file contains only the routing logic: a static table that maps
 * (method, path) pairs to handler functions, and handle_request() which
 * walks it. All business logic lives in route_pages.c and route_api.c.
 */

#include "route_handler.h"
#include "route_pages.h"
#include "route_api.h"
#include "route_helpers.h"
#include <string.h>
#include <stdio.h>

#define NUM_ROUTES (sizeof(routes) / sizeof(routes[0]))
/* ── Dispatch table ──────────────────────────────────────────────────── */

// Each entry maps an exact (method, path) pair to a handler function.
// Order does not affect correctness but placing frequent routes first
// reduces the average number of comparisons per request.
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
    { .method = "POST", .path = "/api/report/respond",   .handler = route_api_report_respond   },
    { .method = "POST", .path = "/api/report/feedback",  .handler = route_api_report_feedback  },
    { .method = "POST", .path = "/api/admin/assign",     .handler = route_api_admin_assign     },
    { .method = "GET",  .path = "/static/common.css",    .handler = route_static_css           },
    { .method = "GET",  .path = "/api/docs",             .handler = route_static_docs          },
};


/* ── Public API ──────────────────────────────────────────────────────── */

void handle_request(const HttpRequest *req, HttpResponse *resp) {
    // Two-phase matching: check path first, then method.
    // This ensures a POST to a GET-only route returns 405, not 404.
    bool pathFound = false;

    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (!http_is_request_path(req, routes[i].path)) continue;
        pathFound = true;
        if (http_is_request_method(req, routes[i].method)) {
            routes[i].handler(req, resp);
            return;
        }
    }

    if (pathFound) {
        // Path exists but the method is not registered for it.
        resp_html_error(resp, 405, "405 Method Not Allowed");
    } else {
        // Copy the path into a NUL-terminated buffer for the error message,
        // clamping to URL_BUFFER_SIZE to prevent overrun.
        char path[URL_BUFFER_SIZE + 1];
        size_t n = req->pathLen < URL_BUFFER_SIZE ? req->pathLen : URL_BUFFER_SIZE;
        memcpy(path, req->path, n);
        path[n] = '\0';
        snprintf(resp->body, RESPONSE_BUFFER_SIZE,
                 "<h1>404 Not Found</h1><p><code>%s</code> does not exist.</p>", path);
        resp->statusCode = 404;
        resp->bodyLen = strlen(resp->body);
    }
}