#include "http_utils.h"
#include "picohttpparser.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

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
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
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

void get_field(const char *src, const char *param_name, char *dest, size_t max) {
    dest[0] = '\0';
    if (!src || !param_name) return;
    size_t plen = strlen(param_name);
    const char *p = src;
    while ((p = strstr(p, param_name)) != NULL) {
        if (p == src || *(p - 1) == '&') {
            url_decode(p + plen, dest, max);
            return;
        }
        p++;
    }
}

const char *post_body(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

void parse_request_line(const char *req, char *method, size_t method_max,
                        char *path, size_t path_max) {
    method[0] = path[0] = '\0';

    const char       *m, *p;
    size_t            ml, pl;
    int               minor_version;
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t            num_headers = HTTP_MAX_HEADERS;

    if (phr_parse_request(req, strlen(req),
                           &m, &ml, &p, &pl,
                           &minor_version,
                           headers, &num_headers, 0) < 0)
        return;

    size_t n = ml < method_max - 1 ? ml : method_max - 1;
    memcpy(method, m, n);
    method[n] = '\0';

    /* Strip query string */
    size_t pl_clean = pl;
    for (size_t i = 0; i < pl; i++) {
        if (p[i] == '?') { pl_clean = i; break; }
    }
    n = pl_clean < path_max - 1 ? pl_clean : path_max - 1;
    memcpy(path, p, n);
    path[n] = '\0';
}

void parse_cookie(const char *req, const char *name, char *dest, size_t max) {
    dest[0] = '\0';

    const char       *m, *p;
    size_t            ml, pl;
    int               minor_version;
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t            num_headers = HTTP_MAX_HEADERS;

    if (phr_parse_request(req, strlen(req),
                           &m, &ml, &p, &pl,
                           &minor_version,
                           headers, &num_headers, 0) < 0)
        return;

    size_t name_len = strlen(name);

    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len != 6) continue;
        if (strncasecmp(headers[i].name, "cookie", 6) != 0) continue;

        const char *h   = headers[i].value;
        const char *end = h + headers[i].value_len;

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
        break;
    }
}

void html_escape(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && i + 7 < max) {
        switch (*src) {
            case '<':  memcpy(dest + i, "&lt;", 4); i += 4; break;
            case '>':  memcpy(dest + i, "&gt;", 4); i += 4; break;
            case '&':  memcpy(dest + i, "&amp;", 5); i += 5; break;
            case '"':  memcpy(dest + i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dest + i, "&#39;", 5); i += 5; break;
            default:   dest[i++] = *src; break;
        }
        src++;
    }
    dest[i] = '\0';
}

void make_error_block(const char *msg, char *dest, size_t max) {
    if (!msg || !msg[0]) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, max, "<div class='alert alert-err'>%s</div>", msg);
}