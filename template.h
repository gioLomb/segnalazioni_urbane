/**
 * template.h — Mmap-based HTML template engine
 *
 * Workflow:
 *   1. Run ./inject_css.sh once (and after every CSS change) to embed
 *      common.css into each HTML file.  The server reads self-contained files.
 *
 *   2. Call tpl_load_all() once at startup.
 *      Each HTML file is memory-mapped, copied to heap, and stored as a
 *      plain C string.  No CSS expansion happens here.
 *
 *   3. Call tpl_render() per request to produce a page-specific response.
 *      Placeholders of the form {{KEY}} are replaced with caller-supplied
 *      values; unknown placeholders are replaced with an empty string.
 *
 *   4. Call tpl_unload_all() at shutdown to free all resources.
 *
 * Placeholder syntax:  {{KEY}}   (uppercase, no spaces inside braces)
 * Dynamic keys used across templates:
 *   USERNAME     — HTML-escaped display name of the logged-in user
 *   CITY         — HTML-escaped city name
 *   ERROR_BLOCK  — ready-made <div class="alert ..."> HTML, or empty string
 */

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stddef.h>

/* Maximum number of key=value pairs a single tpl_render() call may pass. */
#define TPL_MAX_VARS 8

/* Opaque handle for a loaded template. */
typedef struct Template Template;

/*
 * A single substitution variable passed to tpl_render().
 * Both key and value must remain valid for the duration of the call.
 */
typedef struct {
    const char *key;    /* placeholder name, e.g. "USERNAME" */
    const char *value;  /* replacement text (pre-escaped by the caller) */
} TplVar;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * Loads every *.html file found in dir_path.
 * Must be called exactly once before any tpl_get() or tpl_render() call.
 * Returns 0 on success, -1 if any file cannot be opened or mapped.
 */
int tpl_load_all(const char *dir_path);

/**
 * Loads a single file from path into the template store under the given name.
 * Can be called after tpl_load_all() to register additional static assets
 * (e.g. tpl_load_file("templates/common.css", "common.css")).
 * For static assets, call tpl_render() with nvars=0 to copy src verbatim.
 * Returns 0 on success, -1 on failure.
 */
int tpl_load_file(const char *path, const char *name);

/** Frees all template memory.  Safe to call even if tpl_load_all failed. */
void tpl_unload_all(void);

/* ── Per-request API ─────────────────────────────────────────────────── */

/**
 * Returns the pre-processed template for the given name (e.g. "login"),
 * or NULL if no such template was loaded.
 * The returned pointer is owned by the engine; do not free it.
 */
const Template *tpl_get(const char *name);

/**
 * Renders the template into dest (NUL-terminated, at most dest_size bytes).
 * vars is an array of nvars TplVar pairs; every {{KEY}} occurrence is
 * replaced by the corresponding value.  Unknown placeholders become "".
 *
 * Returns the number of bytes written (excluding the NUL), or -1 if dest
 * is too small.
 */
int tpl_render(const Template *tpl,
               char *dest, size_t dest_size,
               const TplVar *vars, int nvars);

#endif /* TEMPLATE_H */