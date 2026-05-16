#include "geo.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ── Geometry ────────────────────────────────────────────────────────── */

/**
 * Internal helper to extract bounding box and centroid from GeoJSON coordinates.
 */
static bool parse_geometry(cJSON *geometry, CityGeo *geo) {
    // Initialize boundaries to extreme values for min/max comparison
    geo->latMin = geo->lonMin =  DBL_MAX;
    geo->latMax = geo->lonMax = -DBL_MAX;
    double latSum = 0, lonSum = 0;
    int    count   = 0;

    const char *type   = cJSON_GetStringValue(cJSON_GetObjectItem(geometry, "type"));
    cJSON      *coords = cJSON_GetObjectItem(geometry, "coordinates");
    if (!type || !coords) return false;

    //GeoJSON structures differ between Polygon and MultiPolygon.
    //We aim for the first outer ring (array of coordinates).
    cJSON *outerRing = cJSON_GetArrayItem(coords, 0);
    if (strcmp(type, "MultiPolygon") == 0)
        outerRing = cJSON_GetArrayItem(outerRing, 0);
    if (!outerRing) return false;

    cJSON *pair;
    cJSON_ArrayForEach(pair, outerRing) {
        // GeoJSON standard defines [longitude, latitude] order
        double lon = cJSON_GetArrayItem(pair, 0)->valuedouble;
        double lat = cJSON_GetArrayItem(pair, 1)->valuedouble;
        
        // Update Bounding Box boundaries
        if (lat < geo->latMin) geo->latMin = lat;
        if (lat > geo->latMax) geo->latMax = lat;
        if (lon < geo->lonMin) geo->lonMin = lon;
        if (lon > geo->lonMax) geo->lonMax = lon;
        
        // Accumulate values for centroid calculation (arithmetic mean)
        latSum += lat;
        lonSum += lon;
        count++;
    }

    if (count == 0) return false;
    geo->centroidLat = latSum / count;
    geo->centroidLon = lonSum / count;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int geo_load(const char *path, Hash_Table *ht, const char *citiesOut) {
    // Open file using low-level descriptor for mmap
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    // Retrieve file size to allocate mapping
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t size = (size_t)st.st_size;

    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // fd no longer needed after mapping
    if (data == MAP_FAILED) { perror("mmap"); return -1; }

    // Parse the entire buffer into a JSON tree
    cJSON *root = cJSON_ParseWithLength(data, size);
    munmap((void *)data, size); // Unmap as soon as parsing is done to free virtual memory

    if (!root) {
        fprintf(stderr, "geo: parse error near: %.20s\n", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *features   = cJSON_GetObjectItem(root, "features");
    cJSON *citiesArr = citiesOut ? cJSON_CreateArray() : NULL;
    int    loaded = 0, skipped = 0;

    cJSON *feature;
    // Iterate through every city feature in the GeoJSON collection
    cJSON_ArrayForEach(feature, features) {
        cJSON      *props    = cJSON_GetObjectItem(feature, "properties");
        cJSON      *geometry = cJSON_GetObjectItem(feature, "geometry");
        const char *name     = cJSON_GetStringValue(cJSON_GetObjectItem(props, "comune"));

        if (!name || !geometry) { skipped++; continue; }

        CityGeo geo = {0};
        // Process geometry coordinates
        if (!parse_geometry(geometry, &geo)) { skipped++; continue; }

        // Index the city by its name in the thread-safe hash table
        ht_set(ht, (void *)name, strlen(name) + 1, &geo, sizeof(geo));
        
        // Track the name for the optional output list
        if (citiesArr)
            cJSON_AddItemToArray(citiesArr, cJSON_CreateString(name));
        loaded++;
    }

    cJSON_Delete(root);

    // Write city names list to disk if requested. 

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

bool geo_lookup(Hash_Table * restrict ht, const char * restrict comune, CityGeo * restrict out) {
    // Perform lookup including the null terminator in the key
    return ht_get(ht, (void *)comune, strlen(comune) + 1, out, sizeof(CityGeo));
}

bool geo_contains(const CityGeo *geo, double lat, double lon) {
    // Basic rectangular inclusion check (Bounding Box)
    return lat >= geo->latMin && lat <= geo->latMax &&
           lon >= geo->lonMin && lon <= geo->lonMax;
}