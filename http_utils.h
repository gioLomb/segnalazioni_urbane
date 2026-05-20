/**
 * @file http_utils.h
 * @brief HTTP parsing, evaluation, and serialization utility suite.
 *
 * Provides standalone helper functions divided into three operational tiers:
 * 1. Stream Verification & Parsing (picohttpparser integrations).
 * 2. HTTP/1.1 Compliant Response Serialization.
 * 3. Security sanitization and body payload parameter extractions.
 */

#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include "http_types.h"
#include <stdbool.h>

/** Standard maximum length allocation bound for numerical parsing lookups. */
#define MAX_NUMBER_LEN 24

/* ── Request Parsing Tier ────────────────────────────────────────────── */

/**
 * @brief Parses raw network bytes into a structured HttpRequest context.
 * @pre raw buffer contains a valid or partially received HTTP sequence.
 * @param raw Pointer to the raw socket receive buffer.
 * @param rawLen Length of data inside the raw buffer.
 * @param req Pointer to target HttpRequest context destination.
 * @return true if complete metadata parsing succeeded, false otherwise.
 */
bool http_request_parse(const char *raw, size_t rawLen, HttpRequest *req);

/**
 * @brief Case-insensitively locates a specific header value.
 * @param req Pointer to the current parsed request.
 * @param name Target header name (NUL-terminated).
 * @param valueLen Output pointer populated with the header value slice length.
 * @return Pointer to the non-NUL-terminated header value string, or NULL.
 */
const char *http_request_header(const HttpRequest *req,
                                const char *name,
                                size_t *valueLen);

/**
 * @brief Locates and URL-decodes a specific Cookie value from incoming headers.
 * @param req Pointer to the current parsed request.
 * @param name Target cookie identifier token name.
 * @param dest Output buffer target to write decoded contents.
 * @param max Maximum memory safety ceiling boundary for destination buffer.
 */
void http_request_cookie(const HttpRequest *req,
                         const char *name,
                         char *dest,
                         size_t max);

/** @return true if request matches path context exactly. */
bool http_is_request_path(const HttpRequest *req, const char *path);

/** @return true if request matches specified method string exactly. */
bool http_is_request_method(const HttpRequest *req, const char *method);

/** @return true if complete headers (and body for POST) have arrived. */
bool http_is_request_complete(const char *buf, size_t len);

/** @return true if Connection header specifies explicit persistent session. */
bool http_request_contains_keepalive(const HttpRequest *req);

/* ── Response Rendering Tier ─────────────────────────────────────────── */

/**
 * @brief Compiles structural HttpResponse context into valid raw HTTP wire data.
 * @param resp Source configured HttpResponse to serialize.
 * @param keepAlive Connection lifecycle flag indicator.
 * @param out Target output byte tracking array.
 * @param outMax Maximum allocation capability ceiling of target output buffer.
 * @return Total integer network bytes generated, or -1 on sizing constraint failure.
 */
int http_response_render(const HttpResponse *resp, bool keepAlive,
                         char *restrict out, size_t outMax);

/* ── Body & HTML Sanitization Helpers ───────────────────────────────── */

/** @brief Extracts and URL-decodes an individual key variable from an URL-encoded buffer. */
void get_field(const char *src, const char *paramName,
               char *dest, size_t max);

/** @brief Escapes basic dangerous syntax structures to avoid Cross-Site Scripting (XSS). */
void html_escape(const char *restrict src, char *restrict dest, size_t max);

/** @brief Wraps text messages inside stylized template warning blocks. */
void make_error_block(const char *msg, char *dest, size_t max);

#endif /* HTTP_UTILS_H */