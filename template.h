/**
 * @file template.h
 * @brief Lightweight HTML template engine.
 *
 * Templates are loaded from disk once and stored in an internal pool.
 * Placeholders use {{KEY}} syntax; substitution is driven by a TplVar array.
 *
 * Usage:
 *   tpl_load_files(TPL_LOGIN, TPL_CSS, NULL);
 *   const Template *tpl = tpl_get(TPL_LOGIN);
 *   tpl_render(tpl, buf, sizeof(buf), vars, nVars);
 */

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include <stddef.h>

/**
 * @brief Key-value pair used to substitute a {{KEY}} placeholder at render time.
 */
typedef struct {
    const char *key;   /**< Placeholder name, e.g. "USERNAME"  */
    const char *value; /**< Replacement string (not HTML-escaped) */
} TplVar;

/**
 * @brief Opaque handle to a loaded template. Obtained via tpl_get().
 */
typedef struct Template Template;

/**
 * @brief Loads one or more template files into the internal pool.
 *
 * Each file is memory-mapped, copied into a heap buffer, and pre-parsed
 * into a Fragment array so that tpl_render() needs no runtime scanning.
 * The argument list must be terminated with a NULL sentinel.
 *
 * @pre Each path must be a readable file; the pool has capacity for up to
 *      MAX_TEMPLATES files in total.
 * @param first_path Path to the first template file.
 * @param ...        Additional paths, terminated by NULL.
 * @return 0 if every file loaded successfully, -1 if any file failed.
 */
__attribute__((sentinel))
int tpl_load_files(const char *first_path, ...);

/**
 * @brief Unloads all templates and frees all associated memory.
 * @post The internal pool is empty; all Template pointers are dangling.
 */
void tpl_unload_all(void);

/**
 * @brief Looks up a loaded template by the path used to load it.
 *
 * @param path Path string used in the tpl_load_files() call.
 * @return Pointer to the Template, or NULL if not found.
 */
const Template *tpl_get(const char *path);

/**
 * @brief Renders a template into dest, substituting {{KEY}} placeholders.
 *
 * Iterates the pre-computed Fragment array: literals are copied directly,
 * variables are resolved via a linear scan of vars. No runtime parsing.
 *
 * @pre tpl != NULL, dest != NULL, destSize > 0.
 * @param tpl      Loaded template to render.
 * @param dest     Output buffer.
 * @param destSize Capacity of dest in bytes.
 * @param vars     Array of placeholder substitutions (may be NULL if nVars == 0).
 * @param nVars    Number of elements in vars.
 * @return Number of bytes written on success, -1 if dest is too small.
 */
int tpl_render(const Template *tpl, char *dest, size_t destSize,
               const TplVar *vars, int nVars);

#endif /* TEMPLATE_H */