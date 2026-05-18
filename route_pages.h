/**
 * @file route_pages.h
 * @brief Declarations for HTML page and form handlers.
 *
 * Internal to the routing subsystem — include only from route_handler.c.
 */

#ifndef ROUTE_PAGES_H
#define ROUTE_PAGES_H

#include "http_types.h"

/**
 * @brief GET / — redirects to /home if logged in, renders login page otherwise.
 */
void route_get_root(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief GET /register — renders the registration page (always public).
 */
void route_get_register(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief GET /home — renders the role-appropriate map/dashboard page.
 *
 * Selects admin_map.html, operator_map.html or citizen_home.html based
 * on the authenticated user's role. Redirects to / if no session.
 */
void route_get_home(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief GET /submit — renders the report submission page for citizens.
 *
 * Operators and admins are redirected to /home; unauthenticated users
 * are redirected to /.
 */
void route_get_submit(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief GET /logout — destroys the session and redirects to /.
 */
void route_get_logout(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief POST /login — authenticates credentials and issues a session cookie.
 *
 * On failure renders login.html with an error block.
 */
void route_post_login(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief POST /register — validates fields, checks the city, and creates a user.
 *
 * Admin registration follows a separate path (user_register_admin) that
 * enforces the one-admin-per-city constraint.
 */
void route_post_register(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief POST /submit — validates and inserts a new civic report.
 *
 * Performs a bounding-box check to ensure the coordinates fall within
 * the submitting citizen's city. On failure re-renders the form with
 * an error block.
 */
void route_post_submit(const HttpRequest *req, HttpResponse *resp);

/**
 * @brief GET /static/common.css — serves the shared stylesheet.
 *
 * Routed through the template cache for consistent mmap-backed delivery.
 */
void route_static_css(const HttpRequest *req, HttpResponse *resp);

#endif /* ROUTE_PAGES_H */