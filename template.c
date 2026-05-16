#include "template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Tipo interno ───────────────────────────────────────────────────── */

typedef enum {
    FRAG_LITERAL,   /* testo statico: copiato verbatim */
    FRAG_VARIABLE   /* variabile da sostituire, es. "USERNAME" */
} FragType;

typedef struct {
    FragType    type;
    const char *ptr; /* LITERAL → puntatore in src; VARIABLE → nome chiave */
    size_t      len;
} Fragment;

struct Template {
    char      *path;       /* percorso del file (chiave di lookup) */
    char      *src;        /* contenuto del file, heap-allocated, NUL-terminated */
    size_t     src_len;    /* strlen(src) */
    Fragment  *fragments;  /* array pre-calcolato al caricamento */
    int        frag_count;
};

#define MAX_TEMPLATES 64

static Template g_pool[MAX_TEMPLATES];
static int      g_count = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *lookup(const char *key, size_t klen,
                          const TplVar *vars, int nvars) {
    for (int i = 0; i < nvars; i++)
        if (strlen(vars[i].key) == klen &&
            memcmp(vars[i].key, key, klen) == 0)
            return vars[i].value;
    return "";
}

static const char *find_open(const char *cur, const char *end) {
    while (cur < end - 1) {
        cur = memchr(cur, '{', end - cur);
        if (!cur) return NULL;
        if (cur[1] == '{') return cur;
        cur++;
    }
    return NULL;
}

static const char *find_close(const char *cur, const char *end) {
    while (cur < end - 1) {
        cur = memchr(cur, '}', end - cur);
        if (!cur) return end;
        if (cur[1] == '}') return cur;
        cur++;
    }
    return end;
}

/* ── Fragment pre-parser ────────────────────────────────────────────── */

/*
 * Scansiona src una sola volta e costruisce l'array di Fragment.
 * Due passate: la prima conta, la seconda riempie — nessun realloc.
 * I puntatori ptr dei FRAG_LITERAL puntano direttamente dentro src
 * (zero copie); quelli dei FRAG_VARIABLE puntano al nome della chiave
 * nel segmento {{KEY}} dentro src (anch'esso zero copie).
 */
static int parse_fragments(Template *tpl) {
    const char *src = tpl->src;
    const char *end = src + tpl->src_len;
    int count = 0;

    /* Prima passata: conta i fragment */
    const char *cur = src;
    while (cur < end) {
        const char *open = find_open(cur, end);
        if (!open) { if (cur < end) count++; break; }  /* literal finale */
        if (open > cur) count++;                         /* literal prima di {{ */
        count++;                                         /* variable */
        const char *ke = find_close(open + 2, end);
        cur = (ke < end) ? ke + 2 : end;
    }

    tpl->fragments = malloc((count ? count : 1) * sizeof(Fragment));
    if (!tpl->fragments) return -1;
    tpl->frag_count = 0;

    /* Seconda passata: riempie i fragment */
    cur = src;
    while (cur < end) {
        const char *open = find_open(cur, end);
        if (!open) {
            if (cur < end)
                tpl->fragments[tpl->frag_count++] =
                    (Fragment){ FRAG_LITERAL, cur, (size_t)(end - cur) };
            break;
        }
        if (open > cur)
            tpl->fragments[tpl->frag_count++] =
                (Fragment){ FRAG_LITERAL, cur, (size_t)(open - cur) };

        const char *ks = open + 2;
        const char *ke = find_close(ks, end);
        tpl->fragments[tpl->frag_count++] =
            (Fragment){ FRAG_VARIABLE, ks, (size_t)(ke - ks) };
        cur = (ke < end) ? ke + 2 : end;
    }

    return 0;
}

/* ── Load ───────────────────────────────────────────────────────────── */

static int load_one(const char *path) {
    if (g_count >= MAX_TEMPLATES) {
        fprintf(stderr, "template: max %d templates reached\n", MAX_TEMPLATES);
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd == -1) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); return -1; }
    size_t fsize = (size_t)st.st_size;

    char *map = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { perror("mmap"); return -1; }

    char *buf = malloc(fsize + 1);
    if (!buf) { munmap(map, fsize); return -1; }
    memcpy(buf, map, fsize);
    buf[fsize] = '\0';
    munmap(map, fsize);

    char *key = strdup(path);
    if (!key) { free(buf); return -1; }

    g_pool[g_count].path      = key;
    g_pool[g_count].src       = buf;
    g_pool[g_count].src_len   = fsize;
    g_pool[g_count].fragments  = NULL;
    g_pool[g_count].frag_count = 0;

    if (parse_fragments(&g_pool[g_count]) != 0) {
        free(buf); free(key); return -1;
    }

    printf("template: loaded '%s' (%zu bytes, %d fragments)\n",
           path, fsize, g_pool[g_count].frag_count);
    g_count++;
    return 0;
}

/* ── API pubblica ───────────────────────────────────────────────────── */

int tpl_load_files(const char *first_path, ...) {
    va_list ap;
    va_start(ap, first_path);
    int ret = 0;
    for (const char *p = first_path; p; p = va_arg(ap, const char *)) {
        if (load_one(p) != 0)
            ret = -1;
    }
    va_end(ap);
    return ret;
}

void tpl_unload_all(void) {
    for (int i = 0; i < g_count; i++) {
        free(g_pool[i].path);
        free(g_pool[i].src);
        free(g_pool[i].fragments);
    }
    g_count = 0;
}

const Template *tpl_get(const char *path) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_pool[i].path, path) == 0)
            return &g_pool[i];
    return NULL;
}

/* ── Rendering ──────────────────────────────────────────────────────── */

int tpl_render(const Template *tpl, char *dest, size_t dest_size,
               const TplVar *vars, int nvars) {
    if (!tpl || !dest || !dest_size) return -1;

    /*
     * I fragment sono stati pre-calcolati al caricamento del template:
     * nessuna scansione di src a runtime, solo un loop lineare sull'array.
     * FRAG_LITERAL → memcpy diretta; FRAG_VARIABLE → lookup + memcpy.
     */
    size_t written = 0;

    for (int i = 0; i < tpl->frag_count; i++) {
        const Fragment *f = &tpl->fragments[i];
        const char     *data;
        size_t          len;

        if (f->type == FRAG_LITERAL) {
            data = f->ptr;
            len  = f->len;
        } else {
            data = lookup(f->ptr, f->len, vars, nvars);
            len  = strlen(data);
        }

        if (written + len + 1 > dest_size) {
            dest[written] = '\0';
            return -1;
        }
        memcpy(dest + written, data, len);
        written += len;
    }

    dest[written] = '\0';
    return (int)written;
}