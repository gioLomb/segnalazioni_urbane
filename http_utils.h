#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

/** Maximum number of HTTP headers parsed per request (used by parse_cookie). */
#define HTTP_MAX_HEADERS 32

void        parse_request_line(const char *req, char *method, size_t method_max,
                                char *path, size_t path_max);
const char *post_body         (const char *req);
void        get_field         (const char *src, const char *param_name,
                                char *dest, size_t max);
void        parse_cookie      (const char *req, const char *name,
                                char *dest, size_t max);
void        html_escape       (const char *src, char *dest, size_t max);
void        make_error_block  (const char *msg, char *dest, size_t max);

#endif /* HTTP_UTILS_H */