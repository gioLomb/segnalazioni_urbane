#include "geo.h"
#include "hash_table.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Internal state ──────────────────────────────────────────────────── */

static Hash_Table *s_geo = NULL;

/* ── Private helpers ─────────────────────────────────────────────────── */

static bool parse_geometry(cJSON *geometry, CityGeo *geo) {
    geo->latMin = geo->lonMin =  DBL_MAX;
    geo->latMax = geo->lonMax = -DBL_MAX;
    double latSum = 0, lonSum = 0;
    int    count  = 0;

    const char *type   = cJSON_GetStringValue(cJSON_GetObjectItem(geometry, "type"));
    cJSON      *coords = cJSON_GetObjectItem(geometry, "coordinates");
    if (!type || !coords) return false;

    cJSON *outerRing = cJSON_GetArrayItem(coords, 0);
    if (strcmp(type, "MultiPolygon") == 0)
        outerRing = cJSON_GetArrayItem(outerRing, 0);
    if (!outerRing) return false;

    cJSON *pair;
    cJSON_ArrayForEach(pair, outerRing) {
        double lon = cJSON_GetArrayItem(pair, 0)->valuedouble;
        double lat = cJSON_GetArrayItem(pair, 1)->valuedouble;

        if (lat < geo->latMin) geo->latMin = lat;
        if (lat > geo->latMax) geo->latMax = lat;
        if (lon < geo->lonMin) geo->lonMin = lon;
        if (lon > geo->lonMax) geo->lonMax = lon;

        latSum += lat;
        lonSum += lon;
        count++;
    }

    if (count == 0) return false;
    geo->centroidLat = latSum / count;
    geo->centroidLon = lonSum / count;
    return true;
}

static int load_geojson(const char *path, Hash_Table *ht, const char *citiesOut) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;

    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { perror("mmap"); return -1; }

    cJSON *root = cJSON_ParseWithLength(data, size);
    munmap((void *)data, size);

    if (!root) {
        fprintf(stderr, "geo: parse error near: %.20s\n", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *features  = cJSON_GetObjectItem(root, "features");
    cJSON *citiesArr = citiesOut ? cJSON_CreateArray() : NULL;
    int    loaded = 0, skipped = 0;

    cJSON *feature;
    cJSON_ArrayForEach(feature, features) {
        cJSON      *props    = cJSON_GetObjectItem(feature, "properties");
        cJSON      *geometry = cJSON_GetObjectItem(feature, "geometry");
        const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(props, "comune"));

        if (!name || !geometry) { skipped++; continue; }

        CityGeo geo = {0};
        if (!parse_geometry(geometry, &geo)) { skipped++; continue; }

        ht_set(ht, (void *)name, strlen(name) + 1, &geo, sizeof(geo));
        if (citiesArr)
            cJSON_AddItemToArray(citiesArr, cJSON_CreateString(name));
        loaded++;
    }

    cJSON_Delete(root);

    if (citiesArr) {
        char *json = cJSON_PrintUnformatted(citiesArr);
        cJSON_Delete(citiesArr);
        if (json) {
            FILE *cf = fopen(citiesOut, "w");
            if (cf) { fputs(json, cf); fclose(cf); }
            else    perror(citiesOut);
            free(json);
        }
    }

    printf("geo: loaded %d comuni, skipped %d\n", loaded, skipped);
    return loaded;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int geo_init(const char *geojsonPath, const char *citiesOut) {
    extern unsigned long hash_key(const void *, size_t, unsigned long);
    s_geo = ht_create(8192, hash_key);
    if (!s_geo) {
        fprintf(stderr, "Fatal: geo table allocation failed\n");
        return -1;
    }
    int n = load_geojson(geojsonPath, s_geo, citiesOut);
    if (n < 0) {
        ht_destroy(s_geo, NULL);
        s_geo = NULL;
    }
    return n;
}

void geo_cleanup(void) {
    if (s_geo) {
        ht_destroy(s_geo, NULL);
        s_geo = NULL;
    }
}

bool geo_lookup(const char *comune, CityGeo *out) {
    if (!s_geo || !comune) return false;
    return ht_get(s_geo, (void *)comune, strlen(comune) + 1, out, sizeof(CityGeo));
}

bool geo_contains(const CityGeo *geo, double lat, double lon) {
    return lat >= geo->latMin && lat <= geo->latMax &&
           lon >= geo->lonMin && lon <= geo->lonMax;
}