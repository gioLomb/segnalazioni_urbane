/**
 * route_handler.c — HTTP request dispatcher for SegnalaCity
 *
 * Every handler receives a fully-parsed HttpRequest and writes its result
 * into an HttpResponse.  No raw buffer scanning, no RouteExtra side-channel.
 *
 * File layout
 * ───────────
 *   Section 1 — Session helper
 *   Section 2 — Response helpers   (redirect, json_response)
 *   Section 3 — Submit map vars    (build_map_vars)
 *   Section 4 — Page handlers      (GET)
 *   Section 5 — Form handlers      (POST)
 *   Section 6 — JSON API handlers
 *   Section 7 — Static asset handler
 *   Section 8 — Dispatch table + handle_request()
 */

#include "route_handler.h"
#include "server_functions.h"
#include "template.h"
#include "user.h"
#include "session.h"
#include "report.h"
#include "geo.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Session helper
   ══════════════════════════════════════════════════════════════════════ */

static bool get_session_user(const HttpRequest *req, User *u) {
    char token[TOKEN_HEX_LEN + 2];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (!token[0]) return false;
    uint64_t user_id;
    if (!session_verify(g_sessions, token, &user_id)) return false;
    return user_get_by_id(user_id, u);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 2 — Response helpers
   ══════════════════════════════════════════════════════════════════════ */

static void redirect(HttpResponse *resp, const char *url, const char *cookie) {
    resp->status_code = 302;
    snprintf(resp->location, sizeof(resp->location), "%s", url);
    if (cookie)
        snprintf(resp->set_cookie, sizeof(resp->set_cookie), "%s", cookie);
}

static void json_response(HttpResponse *resp, char *json) {
    if (!json) {
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "[]");
        resp->body_len    = 2;
        resp->status_code = 500;
        return;
    }
    size_t jlen = strlen(json);
    if (jlen >= RESPONSE_BUFFER_SIZE) {
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"response too large\"}");
        resp->body_len    = strlen(resp->body);
        resp->status_code = 500;
    } else {
        memcpy(resp->body, json, jlen + 1);
        resp->body_len    = jlen;
        resp->status_code = 200;
    }
    free(json);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 3 — Submit page map vars builder
   ══════════════════════════════════════════════════════════════════════ */

typedef struct { char lat[32]; char lon[32]; char bounds[128]; } MapVars;

static void build_map_vars(const char *city_name, MapVars *mv) {
    CityGeo geo;
    if (geo_lookup(g_geo_table, city_name, &geo)) {
        snprintf(mv->lat,    sizeof(mv->lat),    "%.6f", geo.centroid_lat);
        snprintf(mv->lon,    sizeof(mv->lon),    "%.6f", geo.centroid_lon);
        snprintf(mv->bounds, sizeof(mv->bounds),
                 "[[%.6f,%.6f],[%.6f,%.6f]]",
                 geo.lat_min, geo.lon_min, geo.lat_max, geo.lon_max);
    } else {
        snprintf(mv->lat,    sizeof(mv->lat),    "41.9");
        snprintf(mv->lon,    sizeof(mv->lon),    "12.5");
        snprintf(mv->bounds, sizeof(mv->bounds), "null");
    }
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 4 — Page handlers (GET)
   ══════════════════════════════════════════════════════════════════════ */

static void route_get_root(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (get_session_user(req, &u)) { redirect(resp, "/home", NULL); return; }
    const Template *tpl = tpl_get("templates/login.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    TplVar vars[] = { { "ERROR_BLOCK", "" } };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 1);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_get_register(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    const Template *tpl = tpl_get("templates/register.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    TplVar vars[] = { { "ERROR_BLOCK", "" } };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 1);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_get_home(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { redirect(resp, "/", NULL); return; }
    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));
    const char *tpl_name = user_is_operator(&u)
                         ? "templates/operator_map.html"
                         : "templates/citizen_home.html";
    const Template *tpl = tpl_get(tpl_name);
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    TplVar vars[] = { { "USERNAME", esc_user }, { "CITY", esc_city } };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 2);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_get_submit(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { redirect(resp, "/",     NULL); return; }
    if (user_is_operator(&u))       { redirect(resp, "/home", NULL); return; }
    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));
    MapVars mv; build_map_vars(u.city, &mv);
    const Template *tpl = tpl_get("templates/submit.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    TplVar vars[] = {
        { "USERNAME",    esc_user  }, { "CITY",       esc_city  },
        { "ERROR_BLOCK", ""        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 6);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_get_logout(const HttpRequest *req, HttpResponse *resp) {
    char token[TOKEN_HEX_LEN + 2];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (token[0]) session_destroy(g_sessions, token);
    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax",
             SESSION_COOKIE_NAME);
    redirect(resp, "/", cookie);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — Form handlers (POST)
   ══════════════════════════════════════════════════════════════════════ */

static void route_post_login(const HttpRequest *req, HttpResponse *resp) {
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    get_field(req->body, "username=", username, sizeof(username));
    get_field(req->body, "password=", password, sizeof(password));

    const Template *tpl = tpl_get("templates/login.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        char eb[256]; make_error_block("Username o password errati.", eb, sizeof(eb));
        TplVar vars[] = { { "ERROR_BLOCK", eb } };
        tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 1);
        resp->status_code = 200;
        goto done;
    }

    char token[TOKEN_HEX_LEN + 2];
    if (!session_create(g_sessions, u.userId, token)) {
        char eb[256]; make_error_block("Errore interno. Riprova.", eb, sizeof(eb));
        TplVar vars[] = { { "ERROR_BLOCK", eb } };
        tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 1);
        resp->status_code = 200;
        goto done;
    }

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax",
             SESSION_COOKIE_NAME, token, SESSION_MAX_AGE);
    redirect(resp, "/home", cookie);
    return;

done:
    resp->body_len = strlen(resp->body);
}

static void register_error(HttpResponse *resp, const char *msg) {
    const Template *tpl = tpl_get("templates/register.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    char eb[256]; make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = { { "ERROR_BLOCK", eb } };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 1);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_post_register(const HttpRequest *req, HttpResponse *resp) {
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    char city[CITY_LEN]         = {0};
    char role_str[4]            = {0};
    get_field(req->body, "username=", username, sizeof(username));
    get_field(req->body, "password=", password, sizeof(password));
    get_field(req->body, "city=",     city,     sizeof(city));
    get_field(req->body, "role=",     role_str, sizeof(role_str));

    if (!username[0] || !password[0] || !city[0])
        return register_error(resp, "Tutti i campi sono obbligatori.");
    if (strlen(password) < 6)
        return register_error(resp, "La password deve avere almeno 6 caratteri.");

    CityGeo geo = {0};
    if (!geo_lookup(g_geo_table, city, &geo))
        return register_error(resp, "Comune non riconosciuto. Selezionalo dalla lista.");

    UserRole role = (role_str[0] == '1') ? ROLE_OPERATOR : ROLE_CITIZEN;
    if (user_register(username, password, city, role) != 0)
        return register_error(resp, "Username già in uso. Scegline un altro.");

    redirect(resp, "/", NULL);
}

static void submit_error(HttpResponse *resp, const User *u, const char *msg) {
    const Template *tpl = tpl_get("templates/submit.html");
    if (!tpl) { resp->status_code = 500; snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>500</h1>"); goto done; }
    char esc_user[64], esc_city[64];
    html_escape(u->username, esc_user, sizeof(esc_user));
    html_escape(u->city,     esc_city, sizeof(esc_city));
    MapVars mv; build_map_vars(u->city, &mv);
    char eb[256]; make_error_block(msg, eb, sizeof(eb));
    TplVar vars[] = {
        { "USERNAME",    esc_user  }, { "CITY",       esc_city  },
        { "ERROR_BLOCK", eb        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, vars, 6);
    resp->status_code = 200;
done:
    resp->body_len = strlen(resp->body);
}

static void route_post_submit(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { redirect(resp, "/",     NULL); return; }
    if (user_is_operator(&u))       { redirect(resp, "/home", NULL); return; }

    char category[CAT_LEN] = {0};
    char desc[DESC_LEN]    = {0};
    char lat_s[32]         = {0};
    char lon_s[32]         = {0};
    get_field(req->body, "category=",    category, sizeof(category));
    get_field(req->body, "description=", desc,     sizeof(desc));
    get_field(req->body, "lat=",         lat_s,    sizeof(lat_s));
    get_field(req->body, "lon=",         lon_s,    sizeof(lon_s));

    if (!desc[0])
        return submit_error(resp, &u, "La descrizione è obbligatoria.");

    double  lat = lat_s[0] ? atof(lat_s) : 0.0;
    double  lon = lon_s[0] ? atof(lon_s) : 0.0;
    CityGeo geo = {0};
    if (geo_lookup(g_geo_table, u.city, &geo) && !geo_contains(&geo, lat, lon))
        return submit_error(resp, &u, "Le coordinate non appartengono alla tua città.");

    uint64_t rid = report_insert(u.userId, lat, lon, u.city,
                                 category[0] ? category : "Altro", desc);
    if (rid == 0)
        return submit_error(resp, &u, "Errore interno. Riprova.");

    redirect(resp, "/home", NULL);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 6 — JSON API handlers
   ══════════════════════════════════════════════════════════════════════ */

static void route_api_reports_active(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) {
        resp->status_code = 401;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"unauthorized\"}");
        resp->body_len = strlen(resp->body);
        return;
    }
    json_response(resp, report_get_active_json(u.userId, u.city, user_is_operator(&u)));
}

static void route_api_reports_archived(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) {
        resp->status_code = 401;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"unauthorized\"}");
        resp->body_len = strlen(resp->body);
        return;
    }
    json_response(resp, report_get_archived_json(u.userId, u.city, user_is_operator(&u)));
}

static void route_api_cities(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    const Template *tpl = tpl_get(CITIES_JSON_PATH);
    if (!tpl) {
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "[]");
        resp->body_len = 2; resp->status_code = 404; return;
    }
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->body_len    = strlen(resp->body);
    resp->status_code = 200;
    resp->content_type = "application/json";
}

static void route_api_stats(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    time_t uptime = time(NULL) - stats.startTime;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE,
        "{\"uptime\":%ld,\"active_reports\":%d,"
        "\"requests\":%lu,\"connections\":%lu}",
        (long)uptime, report_count_active(),
        stats.totalRequests, stats.totalConnections);
    resp->body_len    = strlen(resp->body);
    resp->status_code = 200;
}

static void route_api_report_status(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        resp->status_code = 403;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"forbidden\"}");
        resp->body_len = strlen(resp->body); return;
    }

    char rid_s[24] = {0}, stat_s[4] = {0};
    get_field(req->body, "report_id=", rid_s,  sizeof(rid_s));
    get_field(req->body, "status=",    stat_s, sizeof(stat_s));

    if (!rid_s[0] || !stat_s[0]) {
        resp->status_code = 400;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"missing params\"}");
        resp->body_len = strlen(resp->body); return;
    }

    uint64_t     report_id  = (uint64_t)strtoull(rid_s, NULL, 10);
    int          new_status = atoi(stat_s);
    ActiveReport r;

    if (!report_get_by_id(report_id, &r)) {
        resp->status_code = 404;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"not found\"}");
        resp->body_len = strlen(resp->body); return;
    }
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        resp->status_code = 403;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"forbidden\"}");
        resp->body_len = strlen(resp->body); return;
    }

    int rc;
    if (new_status == STATUS_IN_PROGRESS) {
        rc = report_assign(report_id, u.userId);
        if (rc == 0) {
            resp->status_code = 409;
            snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"report already taken\"}");
            resp->body_len = strlen(resp->body); return;
        }
    } else if (new_status == STATUS_RESOLVED) {
        rc = report_resolve(report_id, u.userId);
        if (rc == 0) {
            resp->status_code = 403;
            snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"not authorized or already resolved\"}");
            resp->body_len = strlen(resp->body); return;
        }
    } else {
        resp->status_code = 400;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"invalid status\"}");
        resp->body_len = strlen(resp->body); return;
    }

    if (rc < 0) {
        resp->status_code = 500;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"error\":\"db error\"}");
        resp->body_len = strlen(resp->body); return;
    }

    resp->status_code = 200;
    snprintf(resp->body, RESPONSE_BUFFER_SIZE, "{\"ok\":true}");
    resp->body_len = strlen(resp->body);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7 — Static asset handler
   ══════════════════════════════════════════════════════════════════════ */

static void route_static_css(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    const Template *tpl = tpl_get("templates/common.css");
    if (!tpl) {
        resp->status_code = 404;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "/* CSS not found */");
        resp->body_len = strlen(resp->body); return;
    }
    tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->body_len     = strlen(resp->body);
    resp->status_code  = 200;
    resp->content_type = "text/css; charset=utf-8";
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 8 — Dispatch table + handle_request()
   ══════════════════════════════════════════════════════════════════════ */

static const Route routes[] = {
    { "GET",  "/",                     route_get_root             },
    { "GET",  "/home",                 route_get_home             },
    { "GET",  "/register",             route_get_register         },
    { "GET",  "/submit",               route_get_submit           },
    { "GET",  "/logout",               route_get_logout           },
    { "POST", "/login",                route_post_login           },
    { "POST", "/register",             route_post_register        },
    { "POST", "/submit",               route_post_submit          },
    { "GET",  "/api/cities",           route_api_cities           },
    { "GET",  "/api/reports/active",   route_api_reports_active   },
    { "GET",  "/api/reports/archived", route_api_reports_archived },
    { "GET",  "/api/stats",            route_api_stats            },
    { "POST", "/api/report/status",    route_api_report_status    },
    { "GET",  "/static/common.css",    route_static_css           },
};
static const size_t NUM_ROUTES = sizeof(routes) / sizeof(routes[0]);

void handle_request(const HttpRequest *req, HttpResponse *resp) {
    bool path_found = false;

    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (!http_request_path_is(req, routes[i].path)) continue;
        path_found = true;
        if (http_request_method_is(req, routes[i].method)) {
            routes[i].handler(req, resp);
            return;
        }
    }

    if (path_found) {
        resp->status_code = 405;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE, "<h1>405 Method Not Allowed</h1>");
    } else {
        /* Build a NUL-terminated copy of path for the error message */
        char path[URL_BUFFER_SIZE];
        size_t n = req->path_len < URL_BUFFER_SIZE - 1 ? req->path_len : URL_BUFFER_SIZE - 1;
        memcpy(path, req->path, n); path[n] = '\0';
        resp->status_code = 404;
        snprintf(resp->body, RESPONSE_BUFFER_SIZE,
                 "<h1>404 Not Found</h1><p><code>%s</code> non esiste.</p>", path);
    }
    resp->body_len = strlen(resp->body);
}