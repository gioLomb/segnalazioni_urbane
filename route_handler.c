/**
 * route_handler.c — HTTP request dispatcher for SegnalaCity
 *
 * Responsibilities:
 *   - Parse the HTTP request line (method + path)
 *   - Extract POST body fields and cookie values
 *   - Verify session cookies → resolve the logged-in User
 *   - Render HTML pages via the template engine (no HTML in C)
 *   - Serve a thin JSON API consumed by client-side JavaScript
 *   - Return appropriate HTTP status codes
 *
 * Adding a new route:
 *   1. Write the handler function (signature: RouteHandler).
 *   2. Add a Route entry to the routes[] table at the bottom of this file.
 *   3. If the route renders a page, add an HTML file to templates/.
 */

#include "route_handler.h"
#include "server_functions.h"
#include "template.h"
#include "user.h"
#include "session.h"
#include "report.h"
#include "geo.h"
#include "http_utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Session helper ─────────────────────────────────────────────────── */

static bool get_session_user(const char *req, User *u) {
    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (!token[0]) return false;

    uint64_t user_id;
    if (!session_verify(g_sessions, token, &user_id)) return false;

    return user_get_by_id(user_id, u);
}

/* ── Output helpers ──────────────────────────────────────────────────── */

static int redirect(RouteExtra *extra, const char *url, const char *cookie) {
    snprintf(extra->location, sizeof(extra->location), "%s", url);
    if (cookie)
        snprintf(extra->set_cookie, sizeof(extra->set_cookie), "%s", cookie);
    return 302;
}

static int json_response(char *json, char *resp, size_t max) {
    if (!json) {
        snprintf(resp, max, "[]");
        return 500;
    }
    size_t jlen = strlen(json);
    if (jlen >= max) {
        free(json);
        snprintf(resp, max, "{\"error\":\"response too large\"}");
        return 500;
    }
    memcpy(resp, json, jlen + 1);
    free(json);
    return 200;
}

/* ── HTML page handlers ──────────────────────────────────────────────── */

/* GET / */
static int route_get_root(const char *req, char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (get_session_user(req, &u))
        return redirect(extra, "/home", NULL);

    const Template *tpl = tpl_get("templates/login.html");
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "ERROR_BLOCK", "" }
    };
    tpl_render(tpl, resp, max, vars, 1);
    return 200;
}

/* GET /register */
static int route_get_register(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)req; (void)extra;

    const Template *tpl = tpl_get("templates/register.html");
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "ERROR_BLOCK", "" }
    };
    tpl_render(tpl, resp, max, vars, 1);
    return 200;
}

/* GET /home */
static int route_get_home(const char *req, char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    const char *tpl_name = user_is_operator(&u) ? "templates/operator_map.html" : "templates/citizen_home.html";
    const Template *tpl = tpl_get(tpl_name);
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "USERNAME", esc_user },
        { "CITY",     esc_city }
    };
    tpl_render(tpl, resp, max, vars, 2);
    return 200;
}

/* GET /submit */
static int route_get_submit(const char *req, char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);
    if (user_is_operator(&u))
        return redirect(extra, "/home", NULL);

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    CityGeo geo = {0};
    char lat_s[32], lon_s[32], bbox_s[128];
    if (geo_lookup(g_geo_table, u.city, &geo)) {
        snprintf(lat_s,  sizeof(lat_s),  "%.6f", geo.centroid_lat);
        snprintf(lon_s,  sizeof(lon_s),  "%.6f", geo.centroid_lon);
        snprintf(bbox_s, sizeof(bbox_s), "[[%.6f,%.6f],[%.6f,%.6f]]",
                 geo.lat_min, geo.lon_min,
                 geo.lat_max, geo.lon_max);
    } else {
        snprintf(lat_s,  sizeof(lat_s),  "41.9");
        snprintf(lon_s,  sizeof(lon_s),  "12.5");
        snprintf(bbox_s, sizeof(bbox_s), "null");
    }

    const Template *tpl = tpl_get("templates/submit.html");
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "USERNAME",    esc_user },
        { "CITY",        esc_city },
        { "ERROR_BLOCK", ""       },
        { "MAP_LAT",     lat_s   },
        { "MAP_LON",     lon_s   },
        { "MAP_BOUNDS",  bbox_s  },
    };
    tpl_render(tpl, resp, max, vars, 6);
    return 200;
}

/* GET /logout */
static int route_get_logout(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)resp; (void)max;

    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (token[0])
        session_destroy(g_sessions, token);

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax",
             SESSION_COOKIE_NAME);
    return redirect(extra, "/", cookie);
}

/* ── Form POST handlers ──────────────────────────────────────────────── */

/* POST /login */
static int route_post_login(const char *req, char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        const Template *tpl = tpl_get("templates/login.html");
        if (!tpl) { snprintf(resp, max, "<h1>500</h1>"); return 500; }
        TplVar vars[] = {
            { "ERROR_BLOCK",
              "<div class='alert alert-err'>Username o password errati.</div>" }
        };
        tpl_render(tpl, resp, max, vars, 1);
        return 200;
    }

    char token[TOKEN_HEX_LEN + 2];
    if (!session_create(g_sessions, u.userId, token)) {
        const Template *tpl = tpl_get("templates/login.html");
        if (!tpl) { snprintf(resp, max, "<h1>500</h1>"); return 500; }
        TplVar vars[] = {
            { "ERROR_BLOCK",
              "<div class='alert alert-err'>Errore interno. Riprova.</div>" }
        };
        tpl_render(tpl, resp, max, vars, 1);
        return 200;
    }

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax",
             SESSION_COOKIE_NAME, token, SESSION_MAX_AGE);
    return redirect(extra, "/home", cookie);
}

/* POST /register */
static int route_post_register(const char *req, char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    char city[CITY_LEN]         = {0};
    char role_str[4]            = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));
    get_field(body, "city=",     city,     sizeof(city));
    get_field(body, "role=",     role_str, sizeof(role_str));

#define REGISTER_ERROR(msg) \
    do { \
        const Template *_t = tpl_get("templates/register.html"); \
        if (!_t) { snprintf(resp, max, "<h1>500</h1>"); return 500; } \
        char _eb[256]; make_error_block((msg), _eb, sizeof(_eb)); \
        TplVar _v[] = { { "ERROR_BLOCK", _eb } }; \
        tpl_render(_t, resp, max, _v, 1); \
        return 200; \
    } while (0)

    if (!username[0] || !password[0] || !city[0])
        REGISTER_ERROR("Tutti i campi sono obbligatori.");
    if (strlen(password) < 6)
        REGISTER_ERROR("La password deve avere almeno 6 caratteri.");

    CityGeo geo = {0};
    if (!geo_lookup(g_geo_table, city, &geo))
        REGISTER_ERROR("Comune non riconosciuto. Selezionalo dalla lista.");

    UserRole role = (role_str[0] == '1') ? ROLE_OPERATOR : ROLE_CITIZEN;
    if (user_register(username, password, city, role) != 0)
        REGISTER_ERROR("Username già in uso. Scegline un altro.");

#undef REGISTER_ERROR

    return redirect(extra, "/", NULL);
}

/* POST /submit */
static int route_post_submit(const char *req, char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);
    if (user_is_operator(&u))
        return redirect(extra, "/home", NULL);

    const char *body = post_body(req);
    char category[CAT_LEN]  = {0};
    char desc[DESC_LEN]     = {0};
    char lat_s[32]          = {0};
    char lon_s[32]          = {0};
    get_field(body, "category=",    category, sizeof(category));
    get_field(body, "description=", desc,     sizeof(desc));
    get_field(body, "lat=",         lat_s,    sizeof(lat_s));
    get_field(body, "lon=",         lon_s,    sizeof(lon_s));

#define SUBMIT_ERROR(msg) \
    do { \
        const Template *_t = tpl_get("templates/submit.html"); \
        if (!_t) { snprintf(resp, max, "<h1>500</h1>"); return 500; } \
        char esc_user[64], esc_city[64]; \
        html_escape(u.username, esc_user, sizeof(esc_user)); \
        html_escape(u.city,     esc_city, sizeof(esc_city)); \
        CityGeo _geo; \
        char _lat_s[32], _lon_s[32], _bbox_s[128]; \
        if (geo_lookup(g_geo_table, u.city, &_geo)) { \
            snprintf(_lat_s,  sizeof(_lat_s),  "%.6f", _geo.centroid_lat); \
            snprintf(_lon_s,  sizeof(_lon_s),  "%.6f", _geo.centroid_lon); \
            snprintf(_bbox_s, sizeof(_bbox_s), "[[%.6f,%.6f],[%.6f,%.6f]]", \
                     _geo.lat_min, _geo.lon_min, \
                     _geo.lat_max, _geo.lon_max); \
        } else { \
            snprintf(_lat_s,  sizeof(_lat_s),  "41.9"); \
            snprintf(_lon_s,  sizeof(_lon_s),  "12.5"); \
            snprintf(_bbox_s, sizeof(_bbox_s), "null"); \
        } \
        char _eb[256]; make_error_block((msg), _eb, sizeof(_eb)); \
        TplVar _v[] = { \
            { "USERNAME",    esc_user }, \
            { "CITY",        esc_city }, \
            { "ERROR_BLOCK", _eb      }, \
            { "MAP_LAT",     _lat_s   }, \
            { "MAP_LON",     _lon_s   }, \
            { "MAP_BOUNDS",  _bbox_s  } \
        }; \
        tpl_render(_t, resp, max, _v, 6); \
        return 200; \
    } while (0)

    if (!desc[0])
        SUBMIT_ERROR("La descrizione è obbligatoria.");

    double lat = lat_s[0] ? atof(lat_s) : 0.0;
    double lon = lon_s[0] ? atof(lon_s) : 0.0;

    CityGeo geo = {0};
    if (geo_lookup(g_geo_table, u.city, &geo)) {
        if (!geo_contains(&geo, lat, lon))
            SUBMIT_ERROR("Le coordinate non appartengono alla tua città.");
    }

    uint64_t rid = report_insert(u.userId, lat, lon, u.city,
                                 category[0] ? category : "Altro", desc);
    if (rid == 0)
        SUBMIT_ERROR("Errore interno. Riprova.");

#undef SUBMIT_ERROR

    return redirect(extra, "/home", NULL);
}

/* ── JSON API handlers ───────────────────────────────────────────────── */

/* GET /api/reports/active */
static int route_api_reports_active(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    return json_response(report_get_active_json(u.userId, u.city, user_is_operator(&u)), resp, max);
}

/* GET /api/reports/archived */
static int route_api_reports_archived(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    return json_response(report_get_archived_json(u.userId, u.city, user_is_operator(&u)), resp, max);
}

/* GET /api/cities */
static int route_api_cities(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)req;

    const Template *tpl = tpl_get(CITIES_JSON_PATH);
    if (!tpl) {
        snprintf(resp, max, "[]");
        return 404;
    }
    if (tpl_render(tpl, resp, max, NULL, 0) < 0) {
        snprintf(resp, max, "[]");
        return 500;
    }
    snprintf(extra->content_type, sizeof(extra->content_type), "application/json");
    return 200;
}

/* GET /api/stats */
static int route_api_stats(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)req; (void)extra;
    
    time_t uptime = time(NULL) - stats.startTime;
    snprintf(resp, max,
        "{\"uptime\":%ld,\"active_reports\":%d,"
        "\"requests\":%lu,\"connections\":%lu}",
        (long)uptime, report_count_active(),
        stats.totalRequests, stats.totalConnections);
    return 200;
}

/* POST /api/report/status */
static int route_api_report_status(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)extra;

    User u = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    const char *body = post_body(req);
    char rid_s[24]   = {0};
    char stat_s[4]   = {0};
    get_field(body, "report_id=", rid_s,  sizeof(rid_s));
    get_field(body, "status=",    stat_s, sizeof(stat_s));

    if (!rid_s[0] || !stat_s[0]) {
        snprintf(resp, max, "{\"error\":\"missing params\"}");
        return 400;
    }

    uint64_t report_id = (uint64_t)strtoull(rid_s, NULL, 10);
    int      new_status = atoi(stat_s);

    ActiveReport r;
    if (!report_get_by_id(report_id, &r)) {
        snprintf(resp, max, "{\"error\":\"not found\"}");
        return 404;
    }
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    int rc;
    if (new_status == STATUS_IN_PROGRESS) {
        rc = report_assign(report_id, u.userId);
        if (rc == 0) {
            snprintf(resp, max, "{\"error\":\"report already taken\"}");
            return 409;
        }
    } else if (new_status == STATUS_RESOLVED) {
        rc = report_resolve(report_id, u.userId);
        if (rc == 0) {
            snprintf(resp, max, "{\"error\":\"not authorized or already resolved\"}");
            return 403;
        }
    } else {
        snprintf(resp, max, "{\"error\":\"invalid status\"}");
        return 400;
    }

    if (rc < 0) {
        snprintf(resp, max, "{\"error\":\"db error\"}");
        return 500;
    }

    snprintf(resp, max, "{\"ok\":true}");
    return 200;
}

/* GET /static/common.css */
static int route_static_css(const char *req, char *resp, size_t max, RouteExtra *extra) {
    (void)req;

    const Template *tpl = tpl_get("templates/common.css");
    if (!tpl) {
        snprintf(resp, max, "/* CSS not found */");
        return 404;
    }

    if (tpl_render(tpl, resp, max, NULL, 0) < 0) {
        snprintf(resp, max, "/* CSS too large */");
        return 500;
    }

    snprintf(extra->content_type, sizeof(extra->content_type), "text/css; charset=utf-8");
    return 200;
}

/* ── Route table ─────────────────────────────────────────────────────── */

static const Route routes[] = {
    { "GET",  "/",                       route_get_root              },
    { "GET",  "/home",                   route_get_home              },
    { "GET",  "/register",               route_get_register          },
    { "GET",  "/submit",                 route_get_submit            },
    { "GET",  "/logout",                 route_get_logout            },
    { "POST", "/login",                  route_post_login            },
    { "POST", "/register",               route_post_register         },
    { "POST", "/submit",                 route_post_submit           },
    { "GET",  "/api/cities",             route_api_cities            },
    { "GET",  "/api/reports/active",     route_api_reports_active    },
    { "GET",  "/api/reports/archived",   route_api_reports_archived  },
    { "GET",  "/api/stats",              route_api_stats             },
    { "POST", "/api/report/status",      route_api_report_status     },
    { "GET",  "/static/common.css",      route_static_css            },
};
static const size_t NUM_ROUTES = sizeof(routes) / sizeof(routes[0]);

/* ── Dispatcher (single pass) ────────────────────────────────────────── */

int handle_request(const char *req, char *resp, size_t resp_max,
                   RouteExtra *extra, int *keep_alive) {
    resp[0] = '\0';
    memset(extra, 0, sizeof(*extra));
    *keep_alive = strstr(req, "Connection: keep-alive") ? 1 : 0;

    char method[8]             = {0};
    char path[URL_BUFFER_SIZE] = {0};
    parse_request_line(req, method, sizeof(method), path, sizeof(path));

    if (!method[0]) {
        snprintf(resp, resp_max, "<h1>400 Bad Request</h1>");
        return 400;
    }

    int path_found = 0;
    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (strcmp(path, routes[i].path) != 0) continue;
        path_found = 1;
        if (strcmp(routes[i].method, method) == 0)
            return routes[i].handler(req, resp, resp_max, extra);
    }

    if (path_found) {
        snprintf(resp, resp_max, "<h1>405 Method Not Allowed</h1>");
        return 405;
    }

    snprintf(resp, resp_max,
             "<h1>404 Not Found</h1><p>La pagina <code>%s</code> non esiste.</p>", path);
    return 404;
}
