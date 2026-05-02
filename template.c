/**
 * template.c — Mmap-based HTML template engine
 *
 * Load phase (once at startup, called by tpl_load_all):
 *   1. Read common.css into a heap buffer (the "css blob").
 *   2. For every *.html file in the templates directory:
 *      a. mmap the file read-only.
 *      b. Expand {{COMMON_CSS}} → css blob; copy all other bytes verbatim.
 *         This pre-expansion means every subsequent render only has to
 *         handle the small, page-specific placeholders (USERNAME, CITY, …).
 *      c. Store the expanded text in a heap-allocated Template struct.
 *      d. munmap the original file.
 *
 * Render phase (per request, called by tpl_render):
 *   Walk the pre-expanded template byte by byte.  On "{{" scan for "}}" and
 *   look up the enclosed key in the caller-supplied TplVar array.  Copy the
 *   value (or "" for unknown keys) into dest.  All other bytes are copied
 *   verbatim.  No heap allocation happens during rendering.
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

/* ── Internal types ──────────────────────────────────────────────────── */

/*
 * A loaded, pre-expanded template.
 * name   — base filename without extension (e.g. "login").
 * src    — heap-allocated NUL-terminated string ready for rendering.
 * src_len — strlen(src), cached to avoid repeated calls.
 */
struct Template {
    char  *name;
    char  *src;
    size_t src_len;
};

/* Fixed-size registry of all loaded templates. */
#define MAX_TEMPLATES 16

static Template g_templates[MAX_TEMPLATES];
static int      g_tpl_count = 0;

/* ── Static helpers ──────────────────────────────────────────────────── */

/*
 * Reads an entire file into a heap buffer and NUL-terminates it.
 * Sets *out_len to the file size (excluding the added NUL).
 * Returns the buffer on success, NULL on any error.
 * Caller must free() the result.
 */
static char *read_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }

    size_t size = (size_t)st.st_size;
    char  *buf  = malloc(size + 1);
    if (!buf) { close(fd); return NULL; }

    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buf + total, size - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);

    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

/*
 * Expands a single placeholder {{TOKEN}} → replacement in src, writing the
 * result into a newly allocated buffer returned to the caller.
 * Any occurrence of {{TOKEN}} (including multiple) is replaced.
 * Returns the expanded string (caller must free), or NULL on alloc failure.
 *
 * Used only during the load phase to inline {{COMMON_CSS}}.
 */
static char *expand_placeholder(const char *src,
                                 const char *token,
                                 const char *replacement) {
    /* Build the full placeholder string: "{{TOKEN}}" */
    size_t tlen = strlen(token);
    char   marker[tlen + 5];        /* "{{" + token + "}}" + NUL */
    snprintf(marker, sizeof(marker), "{{%s}}", token);
    size_t mlen  = strlen(marker);
    size_t rlen  = strlen(replacement);

    /* First pass: count occurrences to size the output buffer. */
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, marker)) != NULL) { count++; p += mlen; }

    size_t src_len  = strlen(src);
    size_t out_size = src_len + count * (rlen - mlen) + 1;
    char  *out      = malloc(out_size);
    if (!out) return NULL;

    /* Second pass: copy with substitutions. */
    char       *w  = out;
    const char *r  = src;
    const char *hit;
    while ((hit = strstr(r, marker)) != NULL) {
        size_t before = (size_t)(hit - r);
        memcpy(w, r, before);
        w += before;
        memcpy(w, replacement, rlen);
        w += rlen;
        r  = hit + mlen;
    }
    /* Copy the remainder after the last match. */
    strcpy(w, r);
    return out;
}

/*
 * Loads one HTML file into a Template slot.
 * path   — full path to the file.
 * name   — base name without extension (already allocated by caller).
 * css    — pre-loaded CSS text to inline at {{COMMON_CSS}}.
 */
static int load_one_template(const char *path,
                              const char *name,
                              const char *css) {
    if (g_tpl_count >= MAX_TEMPLATES) {
        fprintf(stderr, "template: too many templates (max %d)\n", MAX_TEMPLATES);
        return -1;
    }

    /* mmap the HTML file for efficient reading. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    size_t file_size = (size_t)st.st_size;
    char  *mapped    = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) { perror("mmap"); return -1; }

    /* Copy to a heap buffer so we can NUL-terminate and then expand. */
    char *raw = malloc(file_size + 1);
    if (!raw) { munmap(mapped, file_size); return -1; }
    memcpy(raw, mapped, file_size);
    raw[file_size] = '\0';
    munmap(mapped, file_size);

    /* Pre-expand {{COMMON_CSS}} → actual CSS.  Page-specific placeholders
     * (USERNAME, CITY, ERROR_BLOCK) are left intact for tpl_render(). */
    char *expanded = expand_placeholder(raw, "COMMON_CSS", css);
    free(raw);
    if (!expanded) return -1;

    Template *t = &g_templates[g_tpl_count++];
    t->name    = strdup(name);
    t->src     = expanded;
    t->src_len = strlen(expanded);

    if (!t->name) {
        free(t->src);
        g_tpl_count--;
        return -1;
    }

    printf("template: loaded '%s' (%zu bytes after CSS expansion)\n",
           name, t->src_len);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int tpl_load_all(const char *dir_path, const char *css_path) {
    /* 1. Load the shared CSS blob. */
    size_t css_len;
    char  *css = read_file(css_path, &css_len);
    if (!css) {
        fprintf(stderr, "template: cannot read CSS from '%s'\n", css_path);
        return -1;
    }

    /* 2. Iterate over *.html files in dir_path. */
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror(dir_path);
        free(css);
        return -1;
    }

    int ok = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *fname = entry->d_name;
        size_t      flen  = strlen(fname);

        /* Skip non-.html files. */
        if (flen < 5 || strcmp(fname + flen - 5, ".html") != 0)
            continue;

        /* Build the full path. */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, fname);

        /* Derive template name = filename without ".html". */
        char name[256];
        size_t name_len = flen - 5;
        memcpy(name, fname, name_len);
        name[name_len] = '\0';

        if (load_one_template(full_path, name, css) != 0) {
            fprintf(stderr, "template: failed to load '%s'\n", full_path);
            ok = -1;
        }
    }
    closedir(dir);
    free(css);
    return ok;
}

void tpl_unload_all(void) {
    for (int i = 0; i < g_tpl_count; i++) {
        free(g_templates[i].name);
        free(g_templates[i].src);
    }
    g_tpl_count = 0;
}

const Template *tpl_get(const char *name) {
    for (int i = 0; i < g_tpl_count; i++) {
        if (strcmp(g_templates[i].name, name) == 0)
            return &g_templates[i];
    }
    return NULL;
}

int tpl_render(const Template *tpl,
               char *dest, size_t dest_size,
               const TplVar *vars, int nvars) {
    if (!tpl || !dest || dest_size == 0) return -1;

    const char *src = tpl->src;
    size_t      wi  = 0;   /* write index into dest */

#define WRITE_CHAR(c) \
    do { \
        if (wi + 1 >= dest_size) { dest[wi] = '\0'; return -1; } \
        dest[wi++] = (c); \
    } while (0)

#define WRITE_STR(s, n) \
    do { \
        if (wi + (n) + 1 > dest_size) { dest[wi] = '\0'; return -1; } \
        memcpy(dest + wi, (s), (n)); \
        wi += (n); \
    } while (0)

    for (size_t i = 0; i < tpl->src_len; ) {
        /* Detect opening "{{" */
        if (src[i] == '{' && src[i + 1] == '{') {
            i += 2;   /* skip "{{" */

            /* Find the closing "}}" */
            size_t key_start = i;
            while (i < tpl->src_len && !(src[i] == '}' && src[i + 1] == '}'))
                i++;

            size_t key_len = i - key_start;
            i += 2;   /* skip "}}" */

            /* Look up the key in the caller-supplied vars array. */
            const char *value     = "";
            size_t      value_len = 0;
            for (int v = 0; v < nvars; v++) {
                if (strlen(vars[v].key) == key_len &&
                    memcmp(vars[v].key, src + key_start, key_len) == 0) {
                    value     = vars[v].value;
                    value_len = strlen(value);
                    break;
                }
            }
            WRITE_STR(value, value_len);
        } else {
            WRITE_CHAR(src[i++]);
        }
    }

#undef WRITE_CHAR
#undef WRITE_STR

    dest[wi] = '\0';
    return (int)wi;
}
