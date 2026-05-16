/**
 * template.h — Lightweight HTML template engine
 *
 * Caricamento:
 *   tpl_load_files("templates/login.html", "templates/common.css", NULL);
 *
 * I template vengono cercati per path (lo stesso usato per caricarli).
 */

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} TplVar;

typedef struct Template Template;

/* Carica più template (lista di percorsi terminata da NULL).
   Restituisce 0 se tutti i file sono stati caricati, -1 altrimenti. */
__attribute__((sentinel))
int tpl_load_files(const char *first_path, ...);

/* Libera tutti i template. */
void tpl_unload_all(void);

/* Cerca un template per percorso (es. "templates/login.html"). */
const Template *tpl_get(const char *path);

/* Renderizza il template in dest. Restituisce byte scritti, -1 in errore. */
int tpl_render(const Template *tpl, char *dest, size_t destSize,
               const TplVar *vars, int nvars);

#endif
