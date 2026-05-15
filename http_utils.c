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

static void url_decode(const char * restrict src, char * restrict dest, size_t max) {
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
    const char *q = memchr(req->path, '?', req->path_len);
    if (q) {
        req->path_len = (size_t)(q - req->path);
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

void http_request_cookie(const HttpRequest *req, const char *name, char *dest, size_t max) {
    dest[0] = '\0';
    size_t cookie_len = 0;
    const char *h = http_request_header(req, "cookie", &cookie_len);
    if (!h) return;

    size_t name_len = strlen(name);
    const char *end = h + cookie_len;

    while (h < end) {
        // Cerca la prossima occorrenza del nome del cookie nel buffer rimanente
        h = memmem(h, end - h, name, name_len);
        if (!h) break;

        // Controllo dei confini:
        // 1. Deve essere preceduto da ';' o ' ' (o essere all'inizio)
        // 2. Deve essere seguito da '='
        bool prefix_ok = (h == req->headers[0].value || *(h - 1) == ' ' || *(h - 1) == ';');
        if (prefix_ok && (h + name_len < end) && h[name_len] == '=') {
            url_decode(h + name_len + 1, dest, max);
            return;
        }
        
        // Se non era quello giusto, avanza di un byte e ricomincia la ricerca veloce
        h++;
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

static inline bool append_to_buffer(char *out, size_t out_max, size_t *pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    // Calcola lo spazio rimanente
    size_t remaining = out_max - *pos;
    int written = vsnprintf(out + *pos, remaining, fmt, ap);
    
    va_end(ap);

    // Errore di scrittura o buffer insufficiente (vsnprintf restituisce i byte necessari)
    if (written < 0 || (size_t)written >= remaining) {
        return false;
    }

    *pos += (size_t)written;
    return true;
}

int http_response_render(const HttpResponse * restrict resp, bool keep_alive,
                          char * restrict out, size_t out_max) {
    const char *ct = infer_content_type(resp->body, resp->content_type);
    size_t body_len = (resp->status_code == 302) ? 0 : resp->body_len;
    size_t pos = 0;

    // 1. Scrittura degli header principali
    if (!append_to_buffer(out, out_max, &pos, 
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n",
            resp->status_code, http_status_msg(resp->status_code),
            ct, body_len, keep_alive ? "keep-alive" : "close")) return -1;

    // 2. Header opzionali
    if (resp->set_cookie[0]) {
        if (!append_to_buffer(out, out_max, &pos, "Set-Cookie: %s\r\n", resp->set_cookie)) return -1;
    }
    
    if (resp->location[0]) {
        if (!append_to_buffer(out, out_max, &pos, "Location: %s\r\n", resp->location)) return -1;
    }

    // 3. Fine degli header
    if (!append_to_buffer(out, out_max, &pos, "\r\n")) return -1;

    // 4. Copia del corpo
    if (pos + body_len > out_max) return -1; // Nota: rimosso '=' perché memcpy non mette il \0
    if (body_len) {
        memcpy(out + pos, resp->body, body_len);
        pos += body_len;
    }

    return (int)pos;
}

/* ── Body / HTML helpers ─────────────────────────────────────────────── */

const char *post_body(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

void get_field(const char *src, const char *param_name, char *dest, size_t max) {
    dest[0] = '\0';
    if (unlikely(!src || !param_name)) return;
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

void html_escape(const char * restrict src, char * restrict dest, size_t max) {
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
    if (unlikely(!msg || !msg[0])) { dest[0] = '\0'; return; }
    snprintf(dest, max, "<div class='alert alert-err'>%s</div>", msg);
}