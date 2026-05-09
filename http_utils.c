#include "http_utils.h"
#include "picohttpparser.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void url_decode(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && *src != '&' && *src != ' ' && *src != '\r' && *src != '\n'
           && i < max - 1) {
        if (*src == '%' && isxdigit((unsigned char)src[1])
                        && isxdigit((unsigned char)src[2])) {
            char c = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            if (c == '\0') break;
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

static const char *http_status_msg(int code) {
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

static const char *infer_content_type(const char *body, const char *override) {
    if (override && override[0])                    return override;
    if (body && body[0] == '<')                     return "text/html; charset=utf-8";
    if (body && (body[0] == '{' || body[0] == '[')) return "application/json";
    return "text/plain; charset=utf-8";
}

/* ── Request parsing ─────────────────────────────────────────────────── */

bool http_request_parse(const char *raw, size_t raw_len, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    req->num_headers = HTTP_MAX_HEADERS;

    int r = phr_parse_request(raw, raw_len,
                               &req->method,   &req->method_len,
                               &req->path,     &req->path_len,
                               &req->minor_version,
                               req->headers,   &req->num_headers,
                               0);
    if (r < 0) return false;

    /* Strip query string from path */
    for (size_t i = 0; i < req->path_len; i++) {
        if (req->path[i] == '?') { req->path_len = i; break; }
    }

    /* Point body past the header block */
    if (raw_len > (size_t)r) {
        req->body     = raw + r;
        req->body_len = raw_len - (size_t)r;
    }

    return true;
}

const char *http_request_header(const HttpRequest *req,
                                 const char        *name,
                                 size_t            *value_len) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == nlen &&
            strncasecmp(req->headers[i].name, name, nlen) == 0) {
            if (value_len) *value_len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    }
    return NULL;
}

void http_request_cookie(const HttpRequest *req,
                          const char        *name,
                          char              *dest,
                          size_t             max) {
    dest[0] = '\0';
    size_t      cookie_len = 0;
    const char *cookie_val = http_request_header(req, "cookie", &cookie_len);
    if (!cookie_val) return;

    size_t      name_len = strlen(name);
    const char *h        = cookie_val;
    const char *end      = h + cookie_len;

    while (h < end) {
        while (h < end && (*h == ' ' || *h == ';')) h++;
        if (h >= end) break;
        if ((size_t)(end - h) > name_len
            && memcmp(h, name, name_len) == 0
            && h[name_len] == '=') {
            url_decode(h + name_len + 1, dest, max);
            return;
        }
        while (h < end && *h != ';') h++;
    }
}

bool http_request_path_is(const HttpRequest *req, const char *path) {
    size_t plen = strlen(path);
    return req->path_len == plen && memcmp(req->path, path, plen) == 0;
}

bool http_request_method_is(const HttpRequest *req, const char *method) {
    size_t mlen = strlen(method);
    return req->method_len == mlen && memcmp(req->method, method, mlen) == 0;
}

bool http_request_keep_alive(const HttpRequest *req) {
    size_t      vlen = 0;
    const char *val  = http_request_header(req, "connection", &vlen);
    return val && vlen >= 10 && strncasecmp(val, "keep-alive", 10) == 0;
}

/* ── Response rendering ──────────────────────────────────────────────── */

int http_response_render(const HttpResponse *resp, bool keep_alive,
                          char *out, size_t out_max) {
    const char *ct = infer_content_type(resp->body, resp->content_type);

    /* For redirects (302) we send no body */
    size_t body_len = (resp->status_code == 302) ? 0 : resp->body_len;

    int n = snprintf(out, out_max,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n",
        resp->status_code, http_status_msg(resp->status_code),
        ct, body_len,
        keep_alive ? "keep-alive" : "close");

    if (resp->set_cookie[0])
        n += snprintf(out + n, out_max - (size_t)n,
                      "Set-Cookie: %s\r\n", resp->set_cookie);
    if (resp->location[0])
        n += snprintf(out + n, out_max - (size_t)n,
                      "Location: %s\r\n", resp->location);

    n += snprintf(out + n, out_max - (size_t)n, "\r\n");

    if ((size_t)n + body_len >= out_max) return -1;
    if (body_len) memcpy(out + n, resp->body, body_len);
    return n + (int)body_len;
}

/* ── Body / HTML helpers ─────────────────────────────────────────────── */

const char *post_body(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

void get_field(const char *src, const char *param_name, char *dest, size_t max) {
    dest[0] = '\0';
    if (!src || !param_name) return;
    size_t      plen = strlen(param_name);
    const char *p    = src;
    while ((p = strstr(p, param_name)) != NULL) {
        if (p == src || *(p - 1) == '&') {
            url_decode(p + plen, dest, max);
            return;
        }
        p++;
    }
}

void html_escape(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && i + 7 < max) {
        switch (*src) {
            case '<':  memcpy(dest + i, "&lt;",   4); i += 4; break;
            case '>':  memcpy(dest + i, "&gt;",   4); i += 4; break;
            case '&':  memcpy(dest + i, "&amp;",  5); i += 5; break;
            case '"':  memcpy(dest + i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dest + i, "&#39;",  5); i += 5; break;
            default:   dest[i++] = *src;               break;
        }
        src++;
    }
    dest[i] = '\0';
}

void make_error_block(const char *msg, char *dest, size_t max) {
    if (!msg || !msg[0]) { dest[0] = '\0'; return; }
    snprintf(dest, max, "<div class='alert alert-err'>%s</div>", msg);
}