/**
 * @file route_pages.c
 * @brief HTML page handlers: GET pages and POST form submissions.
 *
 * Every handler here produces an HTML response (redirect or rendered
 * template). JSON responses live in route_api.c.
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
    // Already logged in: skip the login page and go straight to the dashboard.
    if (get_session_user(req, &u)) {
        redirect(resp, "/home", NULL);
        return;
    }
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
    if (!get_session_user(req, &u)) {
        redirect(resp, "/", NULL);
        return;
    }

    char escUser[ESCAPED_PARAM_LEN], escCity[ESCAPED_PARAM_LEN];
    html_escape(u.username, escUser, sizeof(escUser));
    html_escape(u.city, escCity, sizeof(escCity));

    // Select the template that matches the user's role.
    const char *tplName;
    if (user_is_admin(&u)) {
        tplName = "templates/admin_map.html";
    } else if (user_is_operator(&u)) {
        tplName = "templates/operator_map.html";
    } else {
        tplName = "templates/citizen_home.html";
    }

    TplVar vars[] = { { "USERNAME", escUser }, { "CITY", escCity } };
    resp_render_tpl(resp, tplName, vars, 2);
}

void route_get_submit(const HttpRequest *req, HttpResponse *resp) {
    User u = {0};
    if (!get_session_user(req, &u)) {
        redirect(resp, "/", NULL);
        return;
    }
    // Only citizens may submit reports; redirect operators and admins away.
    if (user_is_operator(&u) || user_is_admin(&u)) {
        redirect(resp, "/home", NULL);
        return;
    }

    char escUser[ESCAPED_PARAM_LEN], escCity[ESCAPED_PARAM_LEN];
    html_escape(u.username, escUser, sizeof(escUser));
    html_escape(u.city, escCity, sizeof(escCity));

    MapVars mv;
    build_map_vars(u.city, &mv);

    TplVar vars[] = {
        { "USERNAME",    escUser   }, { "CITY",       escCity   },
        { "ERROR_BLOCK", ""        }, { "MAP_LAT",    mv.lat    },
        { "MAP_LON",     mv.lon    }, { "MAP_BOUNDS", mv.bounds },
    };
    resp_render_tpl(resp, "templates/submit.html", vars, 6);
}

void route_get_logout(const HttpRequest *req, HttpResponse *resp) {
    char token[TOKEN_HEX_LEN + 2];
    http_request_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    // Only call session_destroy if a token was actually present in the request.
    if (token[0]) session_destroy(token);

    // Max-Age=0 instructs the browser to delete the cookie immediately.
    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax",
             SESSION_COOKIE_NAME);
    redirect(resp, "/", cookie);
}

/* ── POST handlers ───────────────────────────────────────────────────── */

void route_post_login(const HttpRequest *req, HttpResponse *resp) {
    char username[USERNAME_LEN] = {0};
    char password[PWD_PLAIN_LEN] = {0};
    get_field(req->body, "username=", username, sizeof(username));
    get_field(req->body, "password=", password, sizeof(password));

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        login_error(resp, "Username o password errati.");
        return;
    }

    char token[TOKEN_HEX_LEN + 1];
    if (!session_create(&u, token)) {
        login_error(resp, "Errore interno. Riprova.");
        return;
    }

    // HttpOnly prevents JS access; SameSite=Lax mitigates CSRF on cross-site navigation.
    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax",
             SESSION_COOKIE_NAME, token, SESSION_MAX_AGE);
    redirect(resp, "/home", cookie);
}

void route_post_register(const HttpRequest *req, HttpResponse *resp) {
    char username[USERNAME_LEN] = {0};
    char password[PWD_PLAIN_LEN] = {0};
    char city[CITY_LEN] = {0};
    char roleStr[4] = {0};
    get_field(req->body, "username=", username, sizeof(username));
    get_field(req->body, "password=", password, sizeof(password));
    get_field(req->body, "city=",     city,     sizeof(city));
    get_field(req->body, "role=",     roleStr,  sizeof(roleStr));

    if (!username[0] || !password[0] || !city[0])
        return register_error(resp, "Tutti i campi sono obbligatori.");
    if (strlen(password) < 6)
        return register_error(resp, "La password deve avere almeno 6 caratteri.");

    // Reject city names that are not in the geo table to prevent
    // registrations for non-existent municipalities.
    CityGeo geo = {0};
    if (!geo_lookup(city, &geo))
        return register_error(resp, "Comune non riconosciuto. Selezionalo dalla lista.");

    // Default to ROLE_CITIZEN when no role field is submitted or the value
    // does not match a known role code.
    int roleVal = roleStr[0] ? atoi(roleStr) : 0;
    UserRole role = (roleVal == 2) ? ROLE_ADMIN
                  : (roleVal == 1) ? ROLE_OPERATOR
                  : ROLE_CITIZEN;

    if (role == ROLE_ADMIN) {
        // Admin registration enforces the one-admin-per-city constraint.
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
    if (!get_session_user(req, &u)) {
        redirect(resp, "/", NULL);
        return;
    }
    if (user_is_operator(&u) || user_is_admin(&u)) {
        redirect(resp, "/home", NULL);
        return;
    }

    char category[CAT_LEN] = {0};
    char desc[DESC_LEN] = {0};
    char latS[COORDINATE_STR_LEN] = {0};
    char lonS[COORDINATE_STR_LEN] = {0};
    get_field(req->body, "category=",    category, sizeof(category));
    get_field(req->body, "description=", desc,     sizeof(desc));
    get_field(req->body, "lat=",         latS,     sizeof(latS));
    get_field(req->body, "lon=",         lonS,     sizeof(lonS));

    if (!desc[0])
        return submit_error(resp, &u, "La descrizione è obbligatoria.");

    double lat = latS[0] ? atof(latS) : 0.0;
    double lon = lonS[0] ? atof(lonS) : 0.0;

    // Bounding-box guard: only reject if the city is in the geo table AND
    // the coordinates fall outside it. Missing geo data is not a hard error.
    CityGeo geo = {0};
    if (geo_lookup(u.city, &geo) && !geo_contains(&geo, lat, lon))
        return submit_error(resp, &u, "Le coordinate non appartengono alla tua città.");

    // Fall back to "Altro" when the category field is empty or not submitted.
    uint64_t rid = report_insert(u.userId, lat, lon, u.city,
                                 category[0] ? category : "Altro", desc);
    if (rid == 0)
        return submit_error(resp, &u, "Errore interno. Riprova.");

    redirect(resp, "/home", NULL);
}

/* ── Static assets ───────────────────────────────────────────────────── */

void route_static_css(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    // Served via the template cache so it benefits from mmap-backed delivery
    // and the same hot-path as HTML templates.
    const Template *tpl = tpl_get("templates/common.css");
    if (unlikely(!tpl)) {
        resp_html_error(resp, 404, "CSS not found");
        return;
    }
    int n = tpl_render(tpl, resp->body, RESPONSE_BUFFER_SIZE, NULL, 0);
    resp->bodyLen = n > 0 ? (size_t)n : 0;
    resp->statusCode = 200;
    resp->contentType = "text/css; charset=utf-8";
}