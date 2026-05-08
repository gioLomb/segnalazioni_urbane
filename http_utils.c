#include "http_utils.h"
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
    size_t i = 0;
    while (*req && *req != ' ' && i < method_max - 1)
        method[i++] = *req++;
    method[i] = '\0';
    while (*req == ' ') req++;
    i = 0;
    while (*req && *req != ' ' && *req != '?' && *req != '\r' && *req != '\n'
           && i < path_max - 1)
        path[i++] = *req++;
    path[i] = '\0';
}

void parse_cookie(const char *req, const char *name, char *dest, size_t max) {
    dest[0] = '\0';
    const char *h = strstr(req, "Cookie:");
    if (!h) return;
    h += 7; /* skip "Cookie:" */
    size_t nlen = strlen(name);
    while (*h && *h != '\r' && *h != '\n') {
        while (*h == ' ' || *h == ';') h++;
        if (*h == '\r' || *h == '\n' || *h == '\0') break;
        if (strncmp(h, name, nlen) == 0 && h[nlen] == '=') {
            url_decode(h + nlen + 1, dest, max);
            return;
        }
        while (*h && *h != ';' && *h != '\r' && *h != '\n') h++;
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
