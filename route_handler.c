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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Request parsing helpers
   ══════════════════════════════════════════════════════════════════════
   All helpers operate on the raw HTTP request buffer.  They are pure
   read-only functions with no side effects.
   ══════════════════════════════════════════════════════════════════════ */

/** Converts a single hex character to its 0-15 integer value. */
static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/**
 * URL-decodes one field value from src into dest (NUL-terminated).
 * Stops at '&' (next field boundary), whitespace, or buffer full.
 * Handles %XX sequences and '+' → space.
 * Rejects embedded NULs (%00) by stopping immediately.
 */
static void url_decode(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && *src != '&'
           && *src != ' ' && *src != '\r' && *src != '\n'
           && i < max - 1) {
        if (*src == '%'
            && isxdigit((unsigned char)src[1])
            && isxdigit((unsigned char)src[2])) {
            char c = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            if (c == '\0') break;   /* reject null-embedded strings */
            dest[i++] = c;
            src += 3;
        } else if (*src == '+') {
            dest[i++] = ' ';
            src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

/**
 * Finds a URL-encoded field by name in a query string or POST body.
 * param_name must include the trailing '=' (e.g. "username=").
 * The decoded value is written into dest.
 */
static void get_field(const char *src,
                      const char *param_name,
                      char       *dest,
                      size_t      max) {
    dest[0] = '\0';
    if (!src || !param_name) return;

    size_t      plen = strlen(param_name);
    const char *p    = src;

    while ((p = strstr(p, param_name)) != NULL) {
        /* Ensure we matched a field boundary, not a substring of another key. */
        if (p == src || *(p - 1) == '&') {
            url_decode(p + plen, dest, max);
            return;
        }
        p++;
    }
}

/**
 * Returns a pointer to the POST body (the bytes after "\r\n\r\n"),
 * or NULL if the header terminator is not present.
 */
static const char *post_body(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/**
 * Parses the HTTP request line and extracts method and path.
 * Path stops at '?' (query string), ' ', '\r', or '\n'.
 */
static void parse_request_line(const char *req,
                                char *method, size_t method_max,
                                char *path,   size_t path_max) {
    method[0] = path[0] = '\0';

    /* Method: read until first space. */
    size_t i = 0;
    while (*req && *req != ' ' && i < method_max - 1)
        method[i++] = *req++;
    method[i] = '\0';

    while (*req == ' ') req++;

    /* Path: read until '?', space, or line ending. */
    i = 0;
    while (*req && *req != ' ' && *req != '?'
           && *req != '\r' && *req != '\n'
           && i < path_max - 1)
        path[i++] = *req++;
    path[i] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 2 — Cookie & session helpers
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Extracts the value of a named cookie from the Cookie: header line.
 *
 * The HTTP Cookie header looks like:
 *   Cookie: name1=val1; name2=val2; sid=abc123\r\n
 *
 * Bug fixed: the previous implementation used strchr(h,';') to advance
 * between pairs and missed the cookie when it was the LAST one on the line
 * (no trailing ';' after the final value).  This version walks every pair
 * explicitly, stopping only at \r or \n, so cookie order never matters.
 *
 * Cookie values are URL-decoded before being written into dest.
 */
static void parse_cookie(const char *req,
                          const char *name,
                          char       *dest,
                          size_t      max) {
    dest[0] = '\0';

    const char *h = strstr(req, "Cookie:");
    if (!h) return;
    h += 7;   /* skip "Cookie:" */

    size_t nlen = strlen(name);

    /*
     * Walk every "name=value" pair on the header line.
     * Pairs are separated by "; " (semicolon + optional space).
     * The line ends at \r or \n — we must not go past it.
     */
    while (*h && *h != '\r' && *h != '\n') {
        /* Skip inter-pair separators: semicolons and spaces. */
        while (*h == ' ' || *h == ';') h++;
        if (*h == '\r' || *h == '\n' || *h == '\0') break;

        if (strncmp(h, name, nlen) == 0 && h[nlen] == '=') {
            url_decode(h + nlen + 1, dest, max);
            return;
        }

        /* Not a match — skip past this pair's value to the next ';'. */
        while (*h && *h != ';' && *h != '\r' && *h != '\n') h++;
    }
}

/**
 * Validates the session cookie present in req.
 * On success, fills *u with the user data and returns true.
 * Returns false if the cookie is missing, expired, or invalid.
 */
static bool get_session_user(const char *req, User *u) {
    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (!token[0]) return false;

    uint64_t user_id;
    if (!session_verify(g_sessions, token, &user_id)) return false;

    return user_get_by_id(user_id, u);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 3 — Output helpers
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Simple HTML entity escaping.
 * Replaces < > & " ' with their safe equivalents.
 * Used on every user-controlled string before inserting it into HTML.
 */
static void html_escape(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && i + 7 < max) {   /* 7 = longest entity (&quot;) + NUL */
        switch (*src) {
            case '<':  memcpy(dest + i, "&lt;",   4); i += 4; break;
            case '>':  memcpy(dest + i, "&gt;",   4); i += 4; break;
            case '&':  memcpy(dest + i, "&amp;",  5); i += 5; break;
            case '"':  memcpy(dest + i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dest + i, "&#39;",  5); i += 5; break;
            default:   dest[i++] = *src; break;
        }
        src++;
    }
    dest[i] = '\0';
}

/**
 * Fills extra->location (triggering a 302 redirect in server_functions.c).
 * Optionally sets extra->set_cookie (e.g. after login / logout).
 */
static int redirect(RouteExtra *extra, const char *url, const char *cookie) {
    snprintf(extra->location, sizeof(extra->location), "%s", url);
    if (cookie)
        snprintf(extra->set_cookie, sizeof(extra->set_cookie), "%s", cookie);
    return 302;
}

/**
 * Renders an error alert block for injection into {{ERROR_BLOCK}}.
 * If msg is empty, writes an empty string (no alert rendered).
 */
static void make_error_block(const char *msg, char *dest, size_t max) {
    if (!msg || !msg[0]) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, max, "<div class='alert alert-err'>%s</div>", msg);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 4 — JSON API helpers
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Copies a heap-allocated JSON string into resp and frees it.
 * Returns 200 on success, 500 if json is NULL or too large for resp.
 */
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

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — HTML page handlers  (GET requests that return full pages)
   ══════════════════════════════════════════════════════════════════════ */

/* GET / — login page (or redirect to /home if already logged in) */
static int route_get_root(const char *req,
                           char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (get_session_user(req, &u))
        return redirect(extra, "/home", NULL);

    const Template *tpl = tpl_get("login");
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "ERROR_BLOCK", "" }
    };
    tpl_render(tpl, resp, max, vars, 1);
    return 200;
}

/* GET /register — registration form */
static int route_get_register(const char *req,
                               char *resp, size_t max, RouteExtra *extra) {
    (void)req; (void)extra;

    const Template *tpl = tpl_get("register");
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "ERROR_BLOCK", "" }
    };
    tpl_render(tpl, resp, max, vars, 1);
    return 200;
}

/* GET /home — citizen dashboard or operator map, based on role */
static int route_get_home(const char *req,
                           char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    const char *tpl_name = user_is_operator(&u) ? "operator_map" : "citizen_home";
    const Template *tpl  = tpl_get(tpl_name);
    if (!tpl) { snprintf(resp, max, "<h1>500 Template missing</h1>"); return 500; }

    TplVar vars[] = {
        { "USERNAME", esc_user },
        { "CITY",     esc_city }
    };
    tpl_render(tpl, resp, max, vars, 2);
    return 200;
}

/* GET /submit — report submission form (citizens only) */
static int route_get_submit(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);
    if (user_is_operator(&u))
        return redirect(extra, "/home", NULL);

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    /* Look up city geometry to center the map and set maxBounds. */
    CityGeo geo = {0};
    char lat_s[32], lon_s[32], bbox_s[128];
    if (geo_lookup(g_geo_table, u.city, &geo)) {
        snprintf(lat_s,  sizeof(lat_s),  "%.6f", geo.centroid_lat);
        snprintf(lon_s,  sizeof(lon_s),  "%.6f", geo.centroid_lon);
        snprintf(bbox_s, sizeof(bbox_s), "[[%.6f,%.6f],[%.6f,%.6f]]",
                 geo.lat_min, geo.lon_min,
                 geo.lat_max, geo.lon_max);
    } else {
        /* Fallback: centre of Italy, no bounds restriction. */
        snprintf(lat_s,  sizeof(lat_s),  "41.9");
        snprintf(lon_s,  sizeof(lon_s),  "12.5");
        snprintf(bbox_s, sizeof(bbox_s), "null");
    }

    const Template *tpl = tpl_get("submit");
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

/* GET /logout — destroys the session and redirects to / */
static int route_get_logout(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    (void)resp; (void)max;

    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (token[0])
        session_destroy(g_sessions, token);

    /* Expire the browser cookie by setting Max-Age=0. */
    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax",
             SESSION_COOKIE_NAME);
    return redirect(extra, "/", cookie);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 6 — Form POST handlers
   ══════════════════════════════════════════════════════════════════════ */

/* POST /login — authenticate user, create session, set cookie */
static int route_post_login(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        const Template *tpl = tpl_get("login");
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
        const Template *tpl = tpl_get("login");
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

/* POST /register — validate fields, create user, redirect to login */
static int route_post_register(const char *req,
                                char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    char city[CITY_LEN]         = {0};
    char role_str[4]            = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));
    get_field(body, "city=",     city,     sizeof(city));
    get_field(body, "role=",     role_str, sizeof(role_str));

    /* Re-render the register form with an error message. */
#define REGISTER_ERROR(msg) \
    do { \
        const Template *_t = tpl_get("register"); \
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

    /* Verify the city exists in the geometry table. */
    CityGeo geo = {0};
    if (!geo_lookup(g_geo_table, city, &geo))
        REGISTER_ERROR("Comune non riconosciuto. Selezionalo dalla lista.");

    UserRole role = (role_str[0] == '1') ? ROLE_OPERATOR : ROLE_CITIZEN;
    if (user_register(username, password, city, role) != 0)
        REGISTER_ERROR("Username già in uso. Scegline un altro.");

#undef REGISTER_ERROR

    return redirect(extra, "/", NULL);
}

/* POST /submit — citizen submits a new report */
static int route_post_submit(const char *req,
                              char *resp, size_t max, RouteExtra *extra) {
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

    /* Re-render the submit form with an error message. */
#define SUBMIT_ERROR(msg) \
    do { \
        const Template *_t = tpl_get("submit"); \
        if (!_t) { snprintf(resp, max, "<h1>500</h1>"); return 500; } \
        char esc_user[64], esc_city[64]; \
        html_escape(u.username, esc_user, sizeof(esc_user)); \
        html_escape(u.city,     esc_city, sizeof(esc_city)); \
        char _eb[256]; make_error_block((msg), _eb, sizeof(_eb)); \
        TplVar _v[] = { \
            { "USERNAME",    esc_user }, \
            { "CITY",        esc_city }, \
            { "ERROR_BLOCK", _eb      } \
        }; \
        tpl_render(_t, resp, max, _v, 3); \
        return 200; \
    } while (0)

    if (!desc[0])
        SUBMIT_ERROR("La descrizione è obbligatoria.");

    double lat = lat_s[0] ? atof(lat_s) : 0.0;
    double lon = lon_s[0] ? atof(lon_s) : 0.0;

    /* Server-side coordinate validation against the city bounding box. */
    CityGeo geo = {0};
    if (geo_lookup(g_geo_table, u.city, &geo)) {
        if (!geo_contains(&geo, lat, lon))
            SUBMIT_ERROR("Le coordinate non appartengono alla tua città.");
    }

    uint64_t rid = report_insert(u.userId, lat, lon,
                                 u.city,
                                 category[0] ? category : "Altro",
                                 desc);
    if (rid == 0)
        SUBMIT_ERROR("Errore interno. Riprova.");

#undef SUBMIT_ERROR

    return redirect(extra, "/home", NULL);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7 — JSON API handlers  (consumed by client-side JavaScript)
   ══════════════════════════════════════════════════════════════════════ */

/* GET /api/reports/active */
static int route_api_reports_active(const char *req,
                                    char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    return json_response(
        report_get_active_json(u.userId, u.city, user_is_operator(&u)),
        resp, max);
}

/* GET /api/reports/archived */
static int route_api_reports_archived(const char *req,
                                      char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    return json_response(
        report_get_archived_json(u.userId, u.city, user_is_operator(&u)),
        resp, max);
}

/* GET /api/cities — serve il JSON dei nomi comuni costruito da geo_load() */
static int route_api_cities(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    (void)req;

    const Template *tpl = tpl_get("cities.json");
    if (!tpl) {
        snprintf(resp, max, "[]");
        return 404;
    }
    if (tpl_render(tpl, resp, max, NULL, 0) < 0) {
        snprintf(resp, max, "[]");
        return 500;
    }
    snprintf(extra->content_type, sizeof(extra->content_type),
             "application/json");
    return 200;
}

/* GET /api/stats */
static int route_api_stats(const char *req,
                           char *resp, size_t max, RouteExtra *extra) {
    (void)req; (void)extra;

    extern unsigned long g_stat_requests, g_stat_connections;
    extern time_t        g_stat_start;

    time_t uptime = time(NULL) - g_stat_start;
    snprintf(resp, max,
        "{\"uptime\":%ld,\"active_reports\":%d,"
        "\"requests\":%lu,\"connections\":%lu}",
        (long)uptime, report_count_active(),
        g_stat_requests, g_stat_connections);
    return 200;
}

/* POST /api/report/status — operator advances a report through its lifecycle */
static int route_api_report_status(const char *req,
                                   char *resp, size_t max, RouteExtra *extra) {
    (void)extra;

    User u = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    /* Parse required fields from POST body. */
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

    /* Verify the report exists and belongs to the operator's city. */
    ActiveReport r;
    if (!report_get_by_id(report_id, &r)) {
        snprintf(resp, max, "{\"error\":\"not found\"}");
        return 404;
    }
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    /* Attempt the status transition. */
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

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7b — Static asset handler
   ══════════════════════════════════════════════════════════════════════ */

/* GET /static/common.css — serve the shared stylesheet from memory */
static int route_static_css(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    (void)req;

    const Template *tpl = tpl_get("common.css");
    if (!tpl) {
        snprintf(resp, max, "/* CSS not found */");
        return 404;
    }

    if (tpl_render(tpl, resp, max, NULL, 0) < 0) {
        snprintf(resp, max, "/* CSS too large */");
        return 500;
    }

    snprintf(extra->content_type, sizeof(extra->content_type),
             "text/css; charset=utf-8");

    return 200;
}



/*
 * Route table.  Matched top-to-bottom; the first row where both method
 * and path agree wins.  A second pass detects 405 (path found, wrong method).
 */
static const Route routes[] = {
    /* method   path                        handler                      */
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

/**
 * Main entry point called by server_functions.c for every HTTP request.
 *
 * Dispatch algorithm (two-pass to distinguish 404 from 405):
 *   Pass 1: look for a row where BOTH path and method match → call handler.
 *   Pass 2: if the path was seen with a different method → 405.
 *   Fallthrough: path not found at all → 404.
 */
int handle_request(const char *req,
                   char *resp, size_t resp_max,
                   RouteExtra *extra, int *keep_alive) {
    resp[0] = '\0';
    memset(extra, 0, sizeof(*extra));
    *keep_alive = (strstr(req, "Connection: keep-alive") != NULL) ? 1 : 0;

    char method[8]             = {0};
    char path[URL_BUFFER_SIZE] = {0};
    parse_request_line(req, method, sizeof(method), path, sizeof(path));

    if (!method[0]) {
        snprintf(resp, resp_max, "<h1>400 Bad Request</h1>");
        return 400;
    }

    int path_matched = 0;
    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (strcmp(path, routes[i].path) != 0) continue;
        path_matched = 1;
        if (strcmp(routes[i].method, method) != 0) continue;
        return routes[i].handler(req, resp, resp_max, extra);
    }

    if (path_matched) {
        snprintf(resp, resp_max, "<h1>405 Method Not Allowed</h1>");
        return 405;
    }

    snprintf(resp, resp_max,
             "<h1>404 Not Found</h1><p>La pagina <code>%s</code> non esiste.</p>",
             path);
    return 404;
}