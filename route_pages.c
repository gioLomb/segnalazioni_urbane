/**
 * @file route_pages.c
 * @brief HTML page handlers: GET pages and POST form submissions.
 *
 * Every handler here produces an HTML response (redirect or rendered
 * template).  JSON responses live in route_api.c.
 *
 * Handlers
 * ────────
 *   GET  /           route_get_root
 *   GET  /register   route_get_register
 *   GET  /home       route_get_home
 *   GET  /submit     route_get_submit
 *   GET  /logout     route_get_logout
 *   POST /login      route_post_login
 *   POST /register   route_post_register
 *   POST /submit     route_post_submit
 *   GET  /static/…   route_static_css
 */

#include "route_pages.h"
#include "route_helpers.h"
#include "user.h"
#include "report.h"
#include "geo.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── GET handlers ────────────────────────────────────────────────────── */

void route_get_root(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (get_session_user(req, &u)) { redirect(resp, "/home", NULL); return; }
    TplVar vars[] = { { "ERROR_BLOCK", "" } };
    resp_render_tpl(resp, "templates/login.html", vars, 1);
}

void route_get_register(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    TplVar vars[] = { { "ERROR_BLOCK", "" } };
    resp_render_tpl(resp, "templates/register.html", vars, 1);
}

void route_get_home(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) { redirect(resp, "/", NULL); return; }

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    const char *tpl_name;
    if      (user_is_admin(&u))    tpl_name = "templates/admin_map.html";
    else if (user_is_operator(&u)) tpl_name = "templates/operator_map.html";
    else                           tpl_name = "templates/citizen_home.html";

    TplVar vars[] = { { "USERNAME", esc_user }, { "CITY", esc_city } };
    resp_render_tpl(resp, tpl_name, vars, 2);
}

void route_get_submit(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u))              { redirect(resp, "/",     NULL); return; }
    if (user_is_operator(&u) || user_is_admin(&u)) { redirect(resp, "/home", NULL); return; }

    char esc_user[64], esc_city[64];
    html_escape(u.username, esc_user, sizeof(esc_user));
    html_escape(u.city,     esc_city, sizeof(esc_city));

    MapVars mv;
    build_map_vars(u.city, &mv);

    TplVar vars[] = {
        { "USERNAME",    esc_user  }, { "CITY",       esc_city  },
        { "ERROR_BLOCK", ""        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    resp_render_tpl(resp, "templates/submit.html", vars, 6);
}

void route_get_logout(const HttpRequest *req, HttpResponse *resp) {
    char token[TOKEN_HEX_LEN + 2];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (token[0]) session_destroy(g_sessions, token);

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax",
             SESSION_COOKIE_NAME);
    redirect(resp, "/", cookie);
}

/* ── POST handlers ───────────────────────────────────────────────────── */

void route_post_login(const HttpRequest *req, HttpResponse *resp) {
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    get_field(req->body, "username=", username, sizeof(username));
    get_field(req->body, "password=", password, sizeof(password));

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        login_error(resp, "Username o password errati.");
        return;
    }

    char token[TOKEN_HEX_LEN + 2];
    if (!session_create(g_sessions, &u, token)) {
        login_error(resp, "Errore interno. Riprova.");
        return;
    }

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax",
             SESSION_COOKIE_NAME, token, SESSION_MAX_AGE);
    redirect(resp, "/home", cookie);
}

void route_post_register(const HttpRequest *req, HttpResponse *resp) {
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

    int role_val = role_str[0] ? atoi(role_str) : 0;
    UserRole role = (role_val == 2) ? ROLE_ADMIN
                  : (role_val == 1) ? ROLE_OPERATOR
                  : ROLE_CITIZEN;

    if (role == ROLE_ADMIN) {
        int rc = user_register_admin(username, password, city);
        if (rc == -2) return register_error(resp, "Esiste già un'amministrazione comunale per questa città.");
        if (rc != 0)  return register_error(resp, "Username già in uso. Scegline un altro.");
    } else {
        if (user_register(username, password, city, role) != 0)
            return register_error(resp, "Username già in uso. Scegline un altro.");
    }

    redirect(resp, "/", NULL);
}

void route_post_submit(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u))                { redirect(resp, "/",     NULL); return; }
    if (user_is_operator(&u) || user_is_admin(&u)) { redirect(resp, "/home", NULL); return; }

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

/* ── Static assets ───────────────────────────────────────────────────── */

void route_static_css(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    const Template *tpl = tpl_get("templates/common.css");
    if (unlikely(!tpl)) { resp_html_error(resp, 404, "CSS not found"); return; }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->body_len     = n > 0 ? (size_t)n : 0;
    resp->status_code  = 200;
    resp->content_type = "text/css; charset=utf-8";
}
