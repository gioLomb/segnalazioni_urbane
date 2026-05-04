/**
 * geo.h — City geometry module
 *
 * Loads an ISTAT GeoJSON file at startup and indexes every comune by name
 * into the application's shared hash table.  After loading, all lookups
 * are O(1) with zero I/O.
 *
 * Each entry stores:
 *   - bounding box  (lat/lon min-max) for server-side coordinate validation
 *   - centroid      (average of polygon vertices) for map centering
 *
 * Usage:
 *   1. geo_load("data/comuni.geojson", ht)  — once at startup
 *   2. geo_lookup("Roma", &info)             — per request
 *   3. geo_contains(&info, lat, lon)         — coordinate validation
 */

#ifndef GEO_H
#define GEO_H

#include "hash_table.h"
#include <stdbool.h>

/*
 * Geometry data for one comune.
 * Stored as the value in the hash table, keyed by comune name.
 */
typedef struct {
    double lat_min, lat_max;
    double lon_min, lon_max;
    double centroid_lat, centroid_lon;
} CityGeo;

/*
 * Parses the GeoJSON file at path and inserts one CityGeo entry per comune
 * into ht.  Skips malformed features but continues parsing.
 *
 * Returns the number of comuni loaded, or -1 on fatal error (file not found,
 * allocation failure).
 */
int geo_load(const char *path, Hash_Table *ht);

/*
 * Looks up a comune by name in ht and fills *out.
 * Returns true if found, false otherwise.
 */
bool geo_lookup(Hash_Table *ht, const char *comune, CityGeo *out);

/*
 * Returns true if (lat, lon) falls inside the bounding box of geo.
 * Used for server-side validation of report coordinates.
 */
bool geo_contains(const CityGeo *geo, double lat, double lon);

#endif /* GEO_H */
