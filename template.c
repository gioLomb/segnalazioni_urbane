/**
 * template.c — Lightweight HTML template engine
 *
 * Overview
 * ────────
 * Manages a fixed array of in-memory file buffers (templates) loaded once
 * at startup.  Two kinds of assets share the same store:
 *
 *   HTML templates  — contain {{KEY}} placeholders replaced per-request.
 *                     Loaded by tpl_load_all(), looked up by base name
 *                     without extension (e.g. tpl_get("login")).
 *
 *   Static assets   — served verbatim, no substitution needed.
 *                     Loaded by tpl_load_file(), looked up by full filename
 *                     (e.g. tpl_get("common.css")).
 *                     Call tpl_render() with nvars=0 to copy as-is.
 *
 * Loading  (startup, once)
 * ─────────────────────────
 *   Each file is memory-mapped read-only, copied into a heap buffer
 *   (NUL-terminated), then unmapped.  After this the file descriptor and
 *   mapping are released; the server holds only the heap copy.
 *
 * Rendering  (per request, zero allocation)
 * ──────────────────────────────────────────
 *   tpl_render() walks src linearly.  When it sees "{{" it extracts the key,
 *   looks it up in the caller-supplied TplVar array, and writes the value.
 *   Everything else is copied verbatim.  Unknown placeholders become "".
 *
 * Updating assets
 * ────────────────
 *   Edit the file under templates/ and restart the server.
 *   No build step required.
 */

#include "template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Internal type ───────────────────────────────────────────────────── */

struct Template {
    char  *name;    /* lookup key — base name for HTML, full filename for assets */
    char  *src;     /* file content, heap-allocated, NUL-terminated              */
    size_t src_len; /* strlen(src), cached to avoid repeated calls               */
};

#define MAX_TEMPLATES 17  /* 5 HTML pages + common.css + room to grow */

static Template g_templates[MAX_TEMPLATES];
static int      g_tpl_count = 0;

/* ── Loading ─────────────────────────────────────────────────────────── */

/*
 * Reads the file at path into a heap buffer and registers it under name.
 * Uses mmap for the initial read (avoids a double-buffered read through
 * stdio), then immediately releases the mapping.
 * Returns 0 on success, -1 on any error.
 */
int tpl_load_file(const char *path, const char *name) {
    if (g_tpl_count >= MAX_TEMPLATES) {
        fprintf(stderr, "template: too many templates (max %d)\n", MAX_TEMPLATES);
        return -1;
    }

    /* Open and stat to get the file size before mapping. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t file_size = (size_t)st.st_size;

    /* Map, copy to heap, unmap. */
    char *mapped = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) { perror("mmap"); return -1; }

    char *src = malloc(file_size + 1);
    if (!src) { munmap(mapped, file_size); return -1; }
    memcpy(src, mapped, file_size);
    src[file_size] = '\0';
    munmap(mapped, file_size);

    /* Register in the global array. */
    Template *t = &g_templates[g_tpl_count++];
    t->src     = src;
    t->src_len = file_size;
    t->name    = strdup(name);
    if (!t->name) { free(t->src); g_tpl_count--; return -1; }

    printf("template: loaded '%s' (%zu bytes)\n", name, file_size);
    return 0;
}

/*
 * Loads every *.html file in dir_path.
 * The lookup name for each file is the base name without the ".html" suffix
 * (e.g. "login.html" → tpl_get("login")).
 * Returns 0 if all files loaded successfully, -1 if any failed.
 */
int tpl_load_all(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) { perror(dir_path); return -1; }

    int ok = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *fname = entry->d_name;
        size_t      flen  = strlen(fname);

        /* Skip anything that is not a .html file. */
        if (flen < 5 || strcmp(fname + flen - 5, ".html") != 0)
            continue;

        /* Build the full path and the name-without-extension. */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, fname);

        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)(flen - 5), fname);

        if (tpl_load_file(full_path, name) != 0) {
            fprintf(stderr, "template: failed to load '%s'\n", full_path);
            ok = -1;
        }
    }
    closedir(dir);
    return ok;
}

/* Frees every loaded template. Safe to call even if tpl_load_all() failed. */
void tpl_unload_all(void) {
    for (int i = 0; i < g_tpl_count; i++) {
        free(g_templates[i].name);
        free(g_templates[i].src);
    }
    g_tpl_count = 0;
}

/* Returns the template registered under name, or NULL if not found. */
const Template *tpl_get(const char *name) {
    for (int i = 0; i < g_tpl_count; i++)
        if (strcmp(g_templates[i].name, name) == 0)
            return &g_templates[i];
    return NULL;
}

/* ── Rendering ───────────────────────────────────────────────────────── */

/*
 * Searches vars for a placeholder key matching key_buf (key_len bytes).
 * Returns the associated value string, or "" if no match is found.
 */
static const char *lookup_var(const char *key_buf, size_t key_len,
                               const TplVar *vars, int nvars) {
    for (int i = 0; i < nvars; i++) {
        if (strlen(vars[i].key) == key_len &&
            memcmp(vars[i].key, key_buf, key_len) == 0)
            return vars[i].value;
    }
    return "";
}

/*
 * Scans forward from src[pos] looking for the two-character sequence "}}".
 * Returns the index of the first '}' of the closing pair, or src_len if
 * the sequence is not found (malformed placeholder — treated as literal).
 */
static size_t find_closing(const char *src, size_t pos, size_t src_len) {
    while (pos < src_len) {
        if (src[pos] == '}' && pos + 1 < src_len && src[pos + 1] == '}')
            return pos;
        pos++;
    }
    return src_len; /* not found */
}

/*
 * Copies len bytes from str into dest starting at *wi, then advances *wi.
 * Returns 0 on success, -1 if dest_size would be exceeded.
 * On failure dest is NUL-terminated at the current write position.
 */
static inline int write_str(char *dest, size_t dest_size, size_t *wi,
                     const char *str, size_t len) {
    if (*wi + len + 1 > dest_size) {
        dest[*wi] = '\0';
        return -1;
    }
    memcpy(dest + *wi, str, len);
    *wi += len;
    return 0;
}

int tpl_render(const Template *tpl,
               char *dest, size_t dest_size,
               const TplVar *vars, int nvars) {
    if (!tpl || !dest || dest_size == 0) return -1;

    const char *src  = tpl->src;
    size_t      slen = tpl->src_len;
    size_t      wi   = 0; /* write index into dest */
    size_t      i    = 0; /* read index into src   */

    while (i < slen) {

        /* Detect opening "{{". */
        if (src[i] == '{' && i + 1 < slen && src[i + 1] == '{') {
            size_t key_start = i + 2;
            size_t key_end   = find_closing(src, key_start, slen);

            const char *value = lookup_var(src + key_start,
                                           key_end - key_start,
                                           vars, nvars);
            if (write_str(dest, dest_size, &wi, value, strlen(value)) != 0)
                return -1;

            /* Advance past "{{", the key, and "}}". */
            i = key_end + 2;

        } else {
            /* Literal character — copy and advance. */
            if (write_str(dest, dest_size, &wi, src + i, 1) != 0)
                return -1;
            i++;
        }
    }

    dest[wi] = '\0';
    return (int)wi;
}