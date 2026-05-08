#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <stddef.h>

/* Parsing della prima riga della richiesta */
void parse_request_line(const char *req, char *method, size_t method_max,
                        char *path, size_t path_max);

/* Ritorna l'inizio del corpo POST (dopo "\r\n\r\n"), o NULL */
const char *post_body(const char *req);

/* Decodifica URL e restituisce il valore di un parametro (es. "username=") */
void get_field(const char *src, const char *param_name, char *dest, size_t max);

/* Estrae il valore di un cookie dalla riga Cookie: */
void parse_cookie(const char *req, const char *name, char *dest, size_t max);

/* Escape HTML di base */
void html_escape(const char *src, char *dest, size_t max);

/* Genera un blocco HTML di errore (vuoto se msg è vuoto) */
void make_error_block(const char *msg, char *dest, size_t max);

#endif
