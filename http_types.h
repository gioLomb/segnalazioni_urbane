/**
 * @file http_types.h
 * @brief Shared HTTP request and response data structures.
 *
 * This module defines the core data representations exchanged between the
 * network transport layer and the application's routing/business logic.
 *
 * Zero-Copy Architecture:
 * The HttpRequest structure relies heavily on non-NUL-terminated string slices
 * pointing directly into the network receive buffer to achieve high performance
 * with zero heap allocations.
 */

#ifndef HTTP_TYPES_H
#define HTTP_TYPES_H

#include "config.h"
#include "picohttpparser.h"
#include <stddef.h>
#include <stdbool.h>

/** Maximum number of HTTP headers tracked per request. */
#define HTTP_MAX_HEADERS 32

/** Maximum size allowed for cookie values. */
#define COOKIE_MAX 512

/**
 * @brief Representation of an incoming HTTP request.
 * @note String fields are NOT null-terminated; use their length fields.
 */
typedef struct{
    const char *method;     /**< HTTP Method string slice (e.g., "GET") */
    size_t methodLen;       /**< Length of the method string slice */
    const char *path;       /**< Resource path string slice (e.g., "/index") */
    size_t pathLen;         /**< Length of the path string slice */

    struct phr_header headers[HTTP_MAX_HEADERS]; /**< Extracted raw headers */
    size_t numHeaders;      /**< Total number of headers successfully parsed */
    int minorVersion;       /**< HTTP minor version (e.g., 1 for HTTP/1.1) */

    const char *body;       /**< Pointer to the start of the body payload */
    size_t bodyLen;         /**< Total byte length of the body payload */
} HttpRequest;


/**
 * @brief Representation of an outgoing HTTP response.
 */
typedef struct{
    int statusCode;         /**< HTTP Status Code (e.g., 200, 404, 302) */
    const char *contentType; /**If left NULL, http_response_render() infers it */
    char *body;             /**< Pre-allocated pointer to the output message body */
    size_t bodyLen;         /**< Byte length of the message body */
    char setCookie[COOKIE_MAX]; /**< Optional out-of-band Set-Cookie header field */
    char location[256];         /**< Optional Location header redirect target field */
} HttpResponse;

#endif /* HTTP_TYPES_H */