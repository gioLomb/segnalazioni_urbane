#include "http_utils.h"
#include "picohttpparser.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/* Converts a single hex digit character to its numeric value (0–15). */
static inline int hex_val(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/*
 * URL-decodes src into dest, stopping at query-string delimiters
 * ('&', ' ', '\r', '\n') or when dest is full.
 * '%XX' sequences are decoded; '+' is mapped to a space.
 * A decoded NUL byte (%00) terminates the loop as a safety measure.
 */
static void url_decode(const char * restrict src, char * restrict dest, size_t max){
    size_t i = 0;
    while (*src && *src != '&' && *src != ' ' && *src != '\r' && *src != '\n'
           && i < max - 1) {
        if (*src == '%' && isxdigit((unsigned char)src[1])
                        && isxdigit((unsigned char)src[2])) {
            char c = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            if (c == '\0') break; // %00 could be used to bypass string checks
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

/* Returns the standard reason-phrase for the most common HTTP status codes. */
static const char *http_status_msg(int code){
    switch (code) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

/*
 * Determines the Content-Type to use in the response.
 * Returns override directly if it is non-NULL and non-empty;
 * otherwise infers the type from the first byte of body.
 */
static const char *infer_contentType(const char *body, const char *override){
    if (override && override[0])                    return override;
    if (body && body[0] == '<')                     return "text/html; charset=utf-8";
    if (body && (body[0] == '{' || body[0] == '[')) return "application/json";
    return "text/plain; charset=utf-8";
}

/* ── Request parsing ─────────────────────────────────────────────────── */

bool http_is_request_complete(const char *buf, size_t len){
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t numHeaders = HTTP_MAX_HEADERS;
    const char *method, *path;
    size_t methodLen, pathLen;
    int minorVersion;

    int r = phr_parse_request(buf, len,
                              &method, &methodLen,
                              &path,   &pathLen,
                              &minorVersion,
                              headers, &numHeaders,
                              0 /* lastLen: always a fresh parse */);

    if (r == -2) return false; // headers incomplete — wait for more data
    if (r == -1) return true;  // malformed — let the handler return 400

    // For POST requests verify that the body has fully arrived
    if (methodLen != 4 || memcmp(method, "POST", 4) != 0) return true;

    for (size_t i = 0; i < numHeaders; i++) {
        // Fast pre-filter: "content-length" is exactly 14 chars
        if (headers[i].name_len != 14) continue;
        if (strncasecmp(headers[i].name, "content-length", 14) != 0) continue;

        // phr_header values are not NUL-terminated — copy into a local buffer
        // and clamp to MAX_NUMBER_LEN - 1 to leave room for the NUL
        char   val[MAX_NUMBER_LEN] = {0};
        size_t vlen = headers[i].value_len < MAX_NUMBER_LEN - 1
                    ? headers[i].value_len
                    : MAX_NUMBER_LEN - 1;
        memcpy(val, headers[i].value, vlen);

        long cl = strtol(val, NULL, 10);
        if (cl <= 0) break; // malformed or zero-length body — treat as complete

        // r is the byte offset where the body starts (returned by phr_parse_request)
        size_t bodyReceived = len - (size_t)r;
        return bodyReceived >= (size_t)cl;
    }
    return true;
}

bool http_request_parse(const char *raw, size_t rawLen, HttpRequest *req){
    memset(req, 0, sizeof(*req));
    req->numHeaders = HTTP_MAX_HEADERS;

    int r = phr_parse_request(raw, rawLen,
                              &req->method,   &req->methodLen,
                              &req->path,     &req->pathLen,
                              &req->minorVersion,
                              req->headers,   &req->numHeaders,
                              0);
    if (r < 0) return false;

    // Strip the query string from the path (e.g. "/api/x?foo=1" → "/api/x")
    const char *q = memchr(req->path, '?', req->pathLen);
    if (q) req->pathLen = (size_t)(q - req->path);

    // Point body to the bytes that follow the header block
    if (rawLen > (size_t)r) {
        req->body    = raw + r;
        req->bodyLen = rawLen - (size_t)r;
    }

    return true;
}

const char *http_request_header(const HttpRequest *req,
                                const char        *name,
                                size_t            *valueLenOut){
    size_t nlen = strlen(name);
    for (size_t i = 0; i < req->numHeaders; i++) {
        if (req->headers[i].name_len == nlen &&
            strncasecmp(req->headers[i].name, name, nlen) == 0) {
            if (valueLenOut) *valueLenOut = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

void http_request_cookie(const HttpRequest *req, const char *name, char *dest, size_t max){
    dest[0] = '\0';

    size_t cookieLen = 0;
    const char *h = http_request_header(req, "cookie", &cookieLen);
    if (!h) return;

    size_t nameLen = strlen(name);
    const char *end = h + cookieLen;

    while (h < end) {
        // Fast scan for the next occurrence of the cookie name in the remaining buffer
        h = memmem(h, end - h, name, nameLen);
        if (!h) break;

        /*
         * Verify token boundaries:
         *  - must be preceded by ';', ' ', or be at the very start of the value
         *  - must be immediately followed by '='
         */
        bool prefixOk = (h == req->headers[0].value || *(h-1) == ' ' || *(h-1) == ';');
        if (prefixOk && (h + nameLen < end) && h[nameLen] == '=') {
            url_decode(h + nameLen + 1, dest, max);
            return;
        }

        // Not the right cookie — advance one byte and retry
        h++;
    }
}

bool http_is_request_path(const HttpRequest *req, const char *path){
    size_t plen = strlen(path);
    return req->pathLen == plen && memcmp(req->path, path, plen) == 0;
}

bool http_is_request_method(const HttpRequest *req, const char *method){
    size_t mlen = strlen(method);
    return req->methodLen == mlen && memcmp(req->method, method, mlen) == 0;
}

bool http_request_contains_keepalive(const HttpRequest *req){
    size_t vlen = 0;
    const char *val = http_request_header(req, "connection", &vlen);
    return val && vlen >= 10 && strncasecmp(val, "keep-alive", 10) == 0;
}

/* ── Response rendering ──────────────────────────────────────────────── */

/*
 * Appends formatted text to out starting at *pos.
 * Updates *pos with the number of bytes written.
 * Returns false if the buffer is too small.
 * vsnprintf returns the bytes it *would* have written — if >= remaining
 * the output was truncated and we treat that as an error.
 */
static inline bool append_to_buffer(char *out, size_t outMax, size_t *pos, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);

    size_t remaining = outMax - *pos;
    int written = vsnprintf(out + *pos, remaining, fmt, ap);

    va_end(ap);

    if (written < 0 || (size_t)written >= remaining) return false;

    *pos += (size_t)written;
    return true;
}

int http_response_render(const HttpResponse * restrict resp, bool keepAlive,
                         char * restrict out, size_t outMax){
    const char *ct = infer_contentType(resp->body, resp->contentType);

    // Redirect responses carry no body
    size_t bodyLen = (resp->statusCode == 302) ? 0 : resp->bodyLen;
    size_t pos = 0;

    // Mandatory headers: status line, Content-Type, Content-Length, Connection
    if (!append_to_buffer(out, outMax, &pos,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n",
            resp->statusCode, http_status_msg(resp->statusCode),
            ct, bodyLen, keepAlive ? "keep-alive" : "close")) return -1;

    // Optional headers — emitted only when the field is non-empty
    if (resp->setCookie[0]) {
        if (!append_to_buffer(out, outMax, &pos, "Set-Cookie: %s\r\n", resp->setCookie)) return -1;
    }
    if (resp->location[0]) {
        if (!append_to_buffer(out, outMax, &pos, "Location: %s\r\n", resp->location)) return -1;
    }

    // Blank line that terminates the header section (CRLFCRLF)
    if (!append_to_buffer(out, outMax, &pos, "\r\n")) return -1;

    // Body — copied with memcpy because it may contain NUL bytes
    if (pos + bodyLen > outMax) return -1;
    if (bodyLen) {
        memcpy(out + pos, resp->body, bodyLen);
        pos += bodyLen;
    }

    return (int)pos;
}

/* ── Body / HTML helpers ─────────────────────────────────────────────── */



void get_field(const char *src, const char *paramName, char *dest, size_t max){
    dest[0] = '\0';
    if (unlikely(!src || !paramName)) return;

    size_t plen = strlen(paramName);
    const char *p = src;

    while ((p = strstr(p, paramName)) != NULL) {
        // Accept the match only at the start of the string or right after '&'
        if (p == src || *(p-1) == '&') {
            url_decode(p + plen, dest, max);
            return;
        }
        p++;
    }
}

void html_escape(const char * restrict src, char * restrict dest, size_t max){
    size_t i = 0;
    // Reserve enough room for the longest entity (&quot; = 6 bytes) plus NUL
    while (*src && i + 7 < max) {
        switch (*src) {
            case '<':  memcpy(dest+i, "&lt;",   4); i += 4; break;
            case '>':  memcpy(dest+i, "&gt;",   4); i += 4; break;
            case '&':  memcpy(dest+i, "&amp;",  5); i += 5; break;
            case '"':  memcpy(dest+i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dest+i, "&#39;",  5); i += 5; break;
            default:   dest[i++] = *src;             break;
        }
        src++;
    }
    dest[i] = '\0';
}

void make_error_block(const char *msg, char *dest, size_t max){
    if (unlikely(!msg || !msg[0])) { dest[0] = '\0'; return; }
    snprintf(dest, max, "<div class='alert alert-err'>%s</div>", msg);
}