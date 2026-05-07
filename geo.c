#include "geo.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Geometry ────────────────────────────────────────────────────────── */

static bool parse_geometry(cJSON *geometry, CityGeo *geo) {
    geo->lat_min = geo->lon_min =  1e9;
    geo->lat_max = geo->lon_max = -1e9;
    double lat_sum = 0, lon_sum = 0;
    int    count   = 0;

    const char *type   = cJSON_GetStringValue(cJSON_GetObjectItem(geometry, "type"));
    cJSON      *coords = cJSON_GetObjectItem(geometry, "coordinates");
    if (!type || !coords) return false;

    /* Outer ring: [[[lon,lat],...]] per Polygon, [[[[lon,lat],...]]] per MultiPolygon */
    cJSON *outer_ring = cJSON_GetArrayItem(coords, 0);
    if (strcmp(type, "MultiPolygon") == 0)
        outer_ring = cJSON_GetArrayItem(outer_ring, 0);
    if (!outer_ring) return false;

    cJSON *pair;
    cJSON_ArrayForEach(pair, outer_ring) {
        double lon = cJSON_GetArrayItem(pair, 0)->valuedouble;
        double lat = cJSON_GetArrayItem(pair, 1)->valuedouble;
        if (lat < geo->lat_min) geo->lat_min = lat;
        if (lat > geo->lat_max) geo->lat_max = lat;
        if (lon < geo->lon_min) geo->lon_min = lon;
        if (lon > geo->lon_max) geo->lon_max = lon;
        lat_sum += lat;
        lon_sum += lon;
        count++;
    }

    if (count == 0) return false;
    geo->centroid_lat = lat_sum / count;
    geo->centroid_lon = lon_sum / count;
    return true;
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

    cJSON *root = cJSON_ParseWithLength(data, size);
    munmap((void *)data, size);

    if (!root) {
        fprintf(stderr, "geo: parse error near: %.20s\n", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *features   = cJSON_GetObjectItem(root, "features");
    cJSON *cities_arr = cities_out ? cJSON_CreateArray() : NULL;
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
        if (cities_arr)
            cJSON_AddItemToArray(cities_arr, cJSON_CreateString(name));
        loaded++;
    }

    cJSON_Delete(root);

    /* Scrive cities.json in un colpo solo da buffer in memoria */
    if (cities_arr) {
        char *json = cJSON_PrintUnformatted(cities_arr);
        cJSON_Delete(cities_arr);
        if (json) {
            FILE *cf = fopen(cities_out, "w");
            if (cf) { fputs(json, cf); fclose(cf); }
            else    perror(cities_out);
            free(json);
        }
    }

    printf("geo: loaded %d comuni, skipped %d\n", loaded, skipped);
    return loaded;
}

bool geo_lookup(Hash_Table *ht, const char *comune, CityGeo *out) {
    return ht_get(ht, (void *)comune, strlen(comune) + 1, out, sizeof(CityGeo));
}

bool geo_contains(const CityGeo *geo, double lat, double lon) {
    return lat >= geo->lat_min && lat <= geo->lat_max &&
           lon >= geo->lon_min && lon <= geo->lon_max;
}