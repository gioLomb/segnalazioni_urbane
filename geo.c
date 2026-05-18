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

// Singleton hash table: city name (string) → CityGeo (value-copied struct).
static Hash_Table *cityTable = NULL;

/* ── Private helpers ─────────────────────────────────────────────────── */

// Parses a GeoJSON geometry object (Polygon or MultiPolygon) and fills geo
// with the axis-aligned bounding box and arithmetic centroid of the outer ring.
// Returns false if the geometry is missing, malformed or contains no vertices.
static bool parse_geometry(cJSON *geometry, CityGeo *geo) {
    geo->latMin = geo->lonMin = DBL_MAX;
    geo->latMax = geo->lonMax = -DBL_MAX;
    double latSum = 0, lonSum = 0;
    int count = 0;

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(geometry, "type"));
    cJSON *coords = cJSON_GetObjectItem(geometry, "coordinates");
    if (!type || !coords) return false;

    // For both Polygon [ring] and MultiPolygon [[ring]], drill down to the outer ring.
    cJSON *outerRing = cJSON_GetArrayItem(coords, 0);
    if (strcmp(type, "MultiPolygon") == 0) outerRing = cJSON_GetArrayItem(outerRing, 0);
    if (!outerRing) return false;

    // Each element is a [lon, lat] pair per GeoJSON spec (note: lon before lat).
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

    // Centroid approximation: arithmetic mean of the outer ring vertices.
    geo->centroidLat = latSum / count;
    geo->centroidLon = lonSum / count;
    return true;
}

// Opens the GeoJSON file via mmap, parses every feature, inserts each
// municipality into ht, and optionally writes the city-name list to citiesOut.
// Returns the number of successfully loaded cities, or -1 on fatal error.
static int load_geojson(const char *path, Hash_Table *ht, const char *citiesOut) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;

    // Map the file read-only; the fd can be closed immediately after.
    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { perror("mmap"); return -1; }

    cJSON *root = cJSON_ParseWithLength(data, size);
    munmap((void *)data, size);

    if (!root) {
        fprintf(stderr, "geo: parse error near: %.20s\n", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *features = cJSON_GetObjectItem(root, "features");
    cJSON *citiesArr = citiesOut ? cJSON_CreateArray() : NULL;
    int loaded = 0, skipped = 0;

    cJSON *feature;
    cJSON_ArrayForEach(feature, features) {
        cJSON *props = cJSON_GetObjectItem(feature, "properties");
        cJSON *geometry = cJSON_GetObjectItem(feature, "geometry");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(props, "comune"));

        // Skip features that are missing a name or a geometry block.
        if (!name || !geometry) { skipped++; continue; }

        CityGeo geo = {0};
        if (!parse_geometry(geometry, &geo)) { skipped++; continue; }

        // Key includes the NUL terminator so lookups via strlen+1 match exactly.
        ht_set(ht, (void *)name, strlen(name) + 1, &geo, sizeof(geo));
        if (citiesArr) cJSON_AddItemToArray(citiesArr, cJSON_CreateString(name));
        loaded++;
    }

    cJSON_Delete(root);

    // Write the optional city-name JSON array to disk.
    if (citiesArr) {
        char *json = cJSON_PrintUnformatted(citiesArr);
        cJSON_Delete(citiesArr);
        if (json) {
            FILE *cf = fopen(citiesOut, "w");
            if (cf) {
                fputs(json, cf);
                fclose(cf);
            } else {
                perror(citiesOut);
            }
            free(json);
        }
    }

    printf("geo: loaded %d comuni, skipped %d\n", loaded, skipped);
    return loaded;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int geo_init(const char *geojsonPath, const char *citiesOut) {
    extern unsigned long hash_key(const void *, size_t, unsigned long);
    cityTable = ht_create(8192, hash_key);
    if (!cityTable) {
        fprintf(stderr, "Fatal: geo table allocation failed\n");
        return -1;
    }

    int n = load_geojson(geojsonPath, cityTable, citiesOut);
    if (n < 0) {
        ht_destroy(cityTable, NULL);
        cityTable = NULL;
    }
    return n;
}

void geo_cleanup(void) {
    if (cityTable) {
        ht_destroy(cityTable, NULL);
        cityTable = NULL;
    }
}

bool geo_lookup(const char *city, CityGeo *out) {
    if (!cityTable || !city) return false;
    return ht_get(cityTable, (void *)city, strlen(city) + 1, out, sizeof(CityGeo));
}

bool geo_contains(const CityGeo *geo, double lat, double lon) {
    return lat >= geo->latMin && lat <= geo->latMax &&
           lon >= geo->lonMin && lon <= geo->lonMax;
}

void build_map_vars(const char *cityName, MapVars *mv) {
    CityGeo geo;
    if (geo_lookup(cityName, &geo)) {
        // Use the polygon centroid for the initial map centre.
        snprintf(mv->lat, sizeof(mv->lat), "%.6f", geo.centroidLat);
        snprintf(mv->lon, sizeof(mv->lon), "%.6f", geo.centroidLon);
        // Bounds formatted as a Leaflet LatLngBounds literal: [[sw],[ne]].
        snprintf(mv->bounds, sizeof(mv->bounds),
                 "[[%.6f,%.6f],[%.6f,%.6f]]",
                 geo.latMin, geo.lonMin, geo.latMax, geo.lonMax);
    } else {
        // City not found in the geo table: fall back to Rome city centre
        // so the map renders somewhere sensible rather than the null island.
        snprintf(mv->lat, sizeof(mv->lat), "41.9");
        snprintf(mv->lon, sizeof(mv->lon), "12.5");
        snprintf(mv->bounds, sizeof(mv->bounds), "null");
    }
}