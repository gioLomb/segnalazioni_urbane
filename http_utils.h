/**
 * http_utils.h — HTTP parsing and rendering utilities
 *
 * Three groups of functions:
 *
 *   Request parsing  — http_request_parse(), http_request_header(),
 *                      http_request_cookie(), http_request_path_is(),
 *                      http_request_method_is(), http_request_keep_alive()
 *
 *   Response render  — http_response_render()
 *
 *   Body / HTML      — get_field(), post_body(), html_escape(),
 *                      make_error_block()
 *                      (unchanged — phr does not touch the body)
 */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include "http_types.h"
#include <stdbool.h>

/* ── Request parsing ─────────────────────────────────────────────────── */

/**
 * Parses raw bytes into req using picohttpparser.
 * All string pointers in req point into raw — no allocation, no copy.
 * Returns true on success, false if the request is malformed.
 */
bool http_request_parse(const char *raw, size_t raw_len, HttpRequest *req);

/**
 * Returns the value of the first header matching name (case-insensitive).
 * Writes its length into *value_len if non-NULL.
 * Returns NULL if not found. The returned pointer is NOT NUL-terminated.
 */
const char *http_request_header(const HttpRequest *req,
                                 const char        *name,
                                 size_t            *value_len);

/**
 * Finds the Cookie header and URL-decodes the named cookie into dest.
 * Writes an empty string if not found.
 */
void http_request_cookie(const HttpRequest *req,
                          const char        *name,
                          char              *dest,
                          size_t             max);

/** Returns true if req->path exactly matches path (NUL-terminated). */
bool http_request_path_is(const HttpRequest *req, const char *path);

/** Returns true if req->method exactly matches method (NUL-terminated). */
bool http_request_method_is(const HttpRequest *req, const char *method);

/** Returns true if the Connection header contains "keep-alive". */
bool http_request_keep_alive(const HttpRequest *req);

/* ── Response rendering ──────────────────────────────────────────────── */

/**
 * Serializes resp into a complete HTTP/1.1 response written to out.
 * content_type == NULL → inferred from resp->body[0].
 * Returns bytes written, or -1 if out_max is too small.
 */
int http_response_render(const HttpResponse *resp, bool keep_alive,
                          char *out, size_t out_max);

/* ── Body / HTML helpers ─────────────────────────────────────────────── */

const char *post_body      (const char *req);
void        get_field      (const char *src, const char *param_name,
                             char *dest, size_t max);
void        html_escape    (const char *src, char *dest, size_t max);
void        make_error_block(const char *msg, char *dest, size_t max);

#endif /* HTTP_UTILS_H */