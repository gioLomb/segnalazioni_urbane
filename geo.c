/**
 * geo.c — City geometry loader
 *
 * Parser design
 * ─────────────
 * Hand-written linear scanner on a memory-mapped view of the GeoJSON file.
 * For each feature:
 *   1. Find "comune": "NAME" — the key with the opening quote of the value
 *      is part of the search needle, so "comune_a": null is never matched.
 *   2. Find the following "coordinates": and skip to the outer ring "[[".
 *   3. Scan [lon, lat] pairs to compute bbox and centroid.
 *   4. Insert into the hash table.
 *
 * No heap allocation per feature — one CityGeo on the stack per iteration.
 */

#include "geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Scanner primitives ──────────────────────────────────────────────── */

/*
 * Returns a pointer to the first occurrence of needle in [cur, end),
 * or NULL if not found.
 */
static const char *scan_find(const char *cur, const char *end,
                              const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return cur;
    while (cur + nlen < end) {
        if (memcmp(cur, needle, nlen) == 0)
            return cur;
        cur++;
    }
    return NULL;
}

/*
 * Reads a JSON quoted string starting just after the opening '"'.
 * Copies at most dest_size-1 bytes into dest and NUL-terminates.
 * Returns pointer past the closing '"', or NULL if closing '"' not found.
 */
static const char *scan_string(const char *cur, const char *end,
                                char *dest, size_t dest_size) {
    size_t i = 0;
    while (cur < end && *cur != '"') {
        if (i + 1 < dest_size)
            dest[i++] = *cur;
        cur++;
    }
    dest[i] = '\0';
    if (cur >= end || *cur != '"') return NULL;
    return cur + 1; /* skip closing '"' */
}

/*
 * Skips whitespace then parses a double.
 * Returns pointer past the number, or NULL on failure.
 */
static const char *scan_double(const char *cur, const char *end, double *out) {
    while (cur < end && (*cur == ' ' || *cur == '\n' ||
                          *cur == '\r' || *cur == '\t'))
        cur++;
    if (cur >= end) return NULL;
    char *endptr;
    *out = strtod(cur, &endptr);
    return (endptr > cur) ? endptr : NULL;
}

/* ── Coordinate parser ───────────────────────────────────────────────── */

/*
 * Reads all [lon, lat] pairs from the outer ring starting at cur,
 * accumulating bbox and centroid into *geo.
 * Returns a pointer past the closing ']' of the ring.
 */
static const char *parse_coordinates(const char *cur, const char *end,
                                      CityGeo *geo) {
    geo->lat_min = geo->lon_min =  1e9;
    geo->lat_max = geo->lon_max = -1e9;
    double lat_sum = 0.0, lon_sum = 0.0;
    int    count   = 0;

    while (cur < end) {
        /* Advance to next '[' (vertex) or ']' (end of ring). */
        while (cur < end && *cur != '[' && *cur != ']')
            cur++;
        if (cur >= end || *cur == ']') break;
        cur++; /* skip '[' */

        double lon, lat;
        const char *after_lon = scan_double(cur, end, &lon);
        if (!after_lon) break;
        cur = after_lon;

        while (cur < end && *cur != ',' && *cur != ']') cur++;
        if (cur >= end || *cur != ',') break;
        cur++; /* skip ',' */

        const char *after_lat = scan_double(cur, end, &lat);
        if (!after_lat) break;
        cur = after_lat;

        if (lat < geo->lat_min) geo->lat_min = lat;
        if (lat > geo->lat_max) geo->lat_max = lat;
        if (lon < geo->lon_min) geo->lon_min = lon;
        if (lon > geo->lon_max) geo->lon_max = lon;
        lat_sum += lat;
        lon_sum += lon;
        count++;

        /* Skip to the closing ']' of this vertex pair. */
        while (cur < end && *cur != ']') cur++;
        if (cur < end) cur++; /* skip ']' */
    }

    if (count > 0) {
        geo->centroid_lat = lat_sum / count;
        geo->centroid_lon = lon_sum / count;
    }

    return cur;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int geo_load(const char *path, Hash_Table *ht, const char *cities_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;

    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { perror("mmap"); return -1; }

    const char *cur     = data;
    const char *end     = data + size;
    int         loaded  = 0;
    int         skipped = 0;

    /*
     * Open the cities JSON output file if requested.
     * Failure is non-fatal: the server starts without the autocomplete asset
     * and the error is reported via perror().
     */
    FILE *cf = NULL;
    if (cities_out) {
        cf = fopen(cities_out, "w");
        if (!cf) perror(cities_out);
        else     fputc('[', cf);
    }

    /*
     * Key insight: search for "comune": " (with space and opening quote).
     * This matches ONLY the string-valued "comune" field and never
     * "comune_a": null, which has no opening quote after the colon.
     */
    static const char COMUNE_KEY[]  = "\"comune\": \"";
    static const char COORDS_KEY[]  = "\"coordinates\":";

    while (cur < end) {

        /* ── Step 1: find the comune name ───────────────────────────── */
        cur = scan_find(cur, end, COMUNE_KEY);
        if (!cur) break;
        cur += strlen(COMUNE_KEY); /* now pointing at first char of name */

        char name[128];
        const char *after_name = scan_string(cur, end, name, sizeof(name));
        if (!after_name || name[0] == '\0') {
            /* scan_string failed or empty name — skip one byte and retry. */
            cur++;
            continue;
        }
        cur = after_name;

        /* ── Step 2: find coordinates, guarding against next feature ── */
        const char *next_feature = scan_find(cur, end, COMUNE_KEY);
        const char *coords       = scan_find(cur, end, COORDS_KEY);

        if (!coords) break; /* end of file */

        if (next_feature && next_feature < coords) {
            /* This feature has no geometry — skip it. */
            skipped++;
            cur = next_feature;
            continue;
        }

        cur = coords + strlen(COORDS_KEY);

        /* Skip whitespace then the opening '[[' of the outer ring. */
        int depth = 0;
        while (cur < end && depth < 2) {
            if (*cur == '[') depth++;
            cur++;
        }
        if (depth < 2) break; /* malformed */

        /* ── Step 3: parse vertices ──────────────────────────────────── */
        CityGeo geo = {0};
        cur = parse_coordinates(cur, end, &geo);

        if (geo.lat_min > geo.lat_max || geo.lon_min > geo.lon_max) {
            skipped++;
            continue;
        }

        /* ── Step 4: insert into hash table ─────────────────────────── */
        ht_set(ht, (void *)name, strlen(name) + 1, &geo, sizeof(geo));

        /* ── Step 5: append name to cities JSON ─────────────────────── */
        if (cf) {
            if (loaded > 0) fputc(',', cf);
            fputc('"', cf);
            for (const char *p = name; *p; p++) {
                if (*p == '"' || *p == '\\') fputc('\\', cf);
                fputc(*p, cf);
            }
            fputc('"', cf);
        }

        loaded++;
    }

    munmap((void *)data, size);

    if (cf) { fputc(']', cf); fclose(cf); }

    printf("geo: loaded %d comuni, skipped %d\n", loaded, skipped);
    return loaded;
}

bool geo_lookup(Hash_Table *ht, const char *comune, CityGeo *out) {
    return ht_get(ht, (void *)comune, strlen(comune) + 1,
                  out, sizeof(CityGeo));
}

bool geo_contains(const CityGeo *geo, double lat, double lon) {
    return lat >= geo->lat_min && lat <= geo->lat_max &&
           lon >= geo->lon_min && lon <= geo->lon_max;
}