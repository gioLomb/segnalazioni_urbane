/**
 * http_types.h — Shared HTTP request/response types
 *
 * This header defines the "contract" between the transport layer
 * (server_functions.c) and the routing/business layer (route_handler.c).
 *
 * HttpRequest
 * ───────────
 * Produced once by http_request_parse() in server_functions.c.
 * All string fields (method, path, header names/values) are NON-NUL-terminated
 * pointers directly into the raw receive buffer — zero copies, zero allocations.
 * Use the corresponding _len fields for all operations.
 *
 * HttpResponse
 * ────────────
 * Filled by a route handler and consumed by http_response_render().
 * body points to a RESPONSE_BUFFER_SIZE slab allocated in read_cb; route
 * handlers write into it with snprintf/tpl_render as before.
 * content_type = NULL means "infer from first byte of body".
 */

#ifndef HTTP_TYPES_H
#define HTTP_TYPES_H

#include "config.h"
#include "picohttpparser.h"
#include <stddef.h>
#include <stdbool.h>

/** Maximum number of HTTP headers stored per request. */
#define HTTP_MAX_HEADERS 32
#define COOKIE_MAX 512


/* ── Request ─────────────────────────────────────────────────────────── */

typedef struct {
    /* Request line — pointers into raw buffer, NOT NUL-terminated */
    const char        *method;
    size_t             method_len;
    const char        *path;
    size_t             path_len;

    /* Parsed headers — direct output of phr_parse_request */
    struct phr_header  headers[HTTP_MAX_HEADERS];
    size_t             num_headers;
    int                minor_version;

    /* Body — NULL if request has no body */
    const char        *body;
    size_t             body_len;
} HttpRequest;

/* ── Response ────────────────────────────────────────────────────────── */

typedef struct {
    int         status_code;

    /*
     * content_type: if NULL, http_response_render() infers from body[0]:
     *   '<'       → text/html; charset=utf-8
     *   '{' | '[' → application/json
     *   else      → text/plain; charset=utf-8
     *
     * Set explicitly for CSS, binary data, or any non-inferrable type.
     */
    const char *content_type;

    /* Body buffer: pre-allocated by read_cb, written by route handlers. */
    char       *body;
    size_t      body_len;

    /* Optional out-of-band fields — empty string = not set */
    char        set_cookie[COOKIE_MAX];
    char        location[256];
} HttpResponse;

#endif /* HTTP_TYPES_H */