/**
 * @file geo.h
 * @brief City geometry module for indexing and validating geographical data.
 *
 * The module manages its own internal hash table; callers never handle
 * a Hash_Table pointer directly.  Lifecycle: geo_init() at startup,
 * geo_cleanup() at shutdown, geo_lookup() / geo_contains() at any time.
 */

#ifndef GEO_H
#define GEO_H

#include <stdbool.h>

/**
 * @brief Geometry data for a specific municipality (comune).
 */
typedef struct {
    double latMin, latMax;           /**< Latitude boundaries    */
    double lonMin, lonMax;           /**< Longitude boundaries   */
    double centroidLat, centroidLon; /**< Calculated center point */
} CityGeo;

/**
 * @brief Allocates the internal hash table and loads city geometry from disk.
 *
 * @param geojsonPath  Path to the ISTAT GeoJSON file.
 * @param citiesOut    Optional path to write a JSON array of loaded city names.
 * @return Number of cities loaded on success, -1 on fatal error.
 */
int geo_init(const char *geojsonPath, const char *citiesOut);

/**
 * @brief Frees all resources allocated by geo_init().
 * Safe to call even if geo_init() was never called or failed.
 */
void geo_cleanup(void);

/**
 * @brief Retrieves geometry information for a city by its name.
 *
 * @param comune  Name of the city to search for.
 * @param out     Populated with the city's geometry if found.
 * @return true if the city was found, false otherwise.
 */
bool geo_lookup(const char *comune, CityGeo *out);

/**
 * @brief Fast bounding-box check for a coordinate pair.
 *
 * @param geo  Pointer to the city geometry.
 * @param lat  Latitude to check.
 * @param lon  Longitude to check.
 * @return true if the coordinates fall within the city's bounding box.
 */
bool geo_contains(const CityGeo *geo, double lat, double lon);

#endif /* GEO_H */