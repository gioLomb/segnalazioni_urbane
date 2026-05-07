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

struct Template {
    char  *path;      /* percorso del file (chiave di lookup) */
    char  *src;       /* contenuto del file, heap-allocated, NUL-terminated */
    size_t src_len;   /* strlen(src) */
};

#define MAX_TEMPLATES 16

static Template g_pool[MAX_TEMPLATES];
static int      g_count = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Carica un singolo file e lo registra usando 'path' come chiave */
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

    g_pool[g_count].path    = key;
    g_pool[g_count].src     = buf;
    g_pool[g_count].src_len = fsize;
    g_count++;

    printf("template: loaded '%s' (%zu bytes)\n", path, fsize);
    return 0;
}

/* ── API pubblica ───────────────────────────────────────────────────── */

int tpl_load_files(const char *first_path, ...) {
    va_list ap;
    va_start(ap, first_path);
    int ret = 0;
    for (const char *p = first_path; p; p = va_arg(ap, const char *)) {
        if (load_one(p) != 0)
            ret = -1;   /* continua a caricare gli altri */
    }
    va_end(ap);
    return ret;
}

void tpl_unload_all(void) {
    for (int i = 0; i < g_count; i++) {
        free(g_pool[i].path);
        free(g_pool[i].src);
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

static const char *lookup(const char *key, size_t klen,
                          const TplVar *vars, int nvars) {
    for (int i = 0; i < nvars; i++)
        if (strlen(vars[i].key) == klen &&
            memcmp(vars[i].key, key, klen) == 0)
            return vars[i].value;
    return "";
}

/* Trova il prossimo '{{' — i '{' singoli vengono ignorati naturalmente */
static const char *find_open(const char *cur, const char *end) {
    while (cur < end - 1) {
        cur = memchr(cur, '{', end - cur);
        if (!cur) return NULL;
        if (cur[1] == '{') return cur;
        cur++;
    }
    return NULL;
}

/* Trova il prossimo '}}' restituendo un puntatore al primo '}' */
static const char *find_close(const char *cur, const char *end) {
    while (cur < end - 1) {
        cur = memchr(cur, '}', end - cur);
        if (!cur) return end;
        if (cur[1] == '}') return cur;
        cur++;
    }
    return end;
}

int tpl_render(const Template *tpl, char *dest, size_t dest_size,const TplVar *vars, int nvars) {
    if (!tpl || !dest || !dest_size) return -1;

    #define EMIT(ptr, len)  do {                                        \
        if (written + (len) + 1 > dest_size) goto overflow;        \
        memcpy(dest + written, (ptr), (len));                       \
        written += (len);                                           \
    } while (0)
    
    const char *cursor  = tpl->src;
    const char *src_end = tpl->src + tpl->src_len;
    size_t      written = 0;

    while (cursor < src_end) {
        const char *open = find_open(cursor, src_end);

        /* Nessun placeholder: copia il resto e termina */
        if (!open) { EMIT(cursor, src_end - cursor); break; }

        /* Testo letterale prima di '{{' */
        EMIT(cursor, open - cursor);

        /* Chiave tra '{{' e '}}' */
        const char *key_start = open + 2;
        const char *key_end   = find_close(key_start, src_end);

        const char *value     = lookup(key_start, key_end - key_start, vars, nvars);
        EMIT(value, strlen(value));

        cursor = key_end + 2;   /* salta '}}' */
    }

#undef EMIT
    dest[written] = '\0';
    return (int)written;

overflow:
    dest[written] = '\0';
    return -1;
}