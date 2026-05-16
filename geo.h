/**
 * @file geo.h
 * @brief City geometry module for indexing and validating geographic data.
 *
 * Manages an internal hash table keyed by city name; callers never touch
 * a Hash_Table pointer directly.
 * Lifecycle: geo_init() at startup, geo_cleanup() at shutdown,
 * geo_lookup() / geo_contains() at any point in between.
 */

#ifndef GEO_H
#define GEO_H

#include <stdbool.h>

/**
 * @brief Axis-aligned bounding box and centroid for a municipality.
 */
typedef struct {
    double latMin, latMax;           /**< Latitude boundaries (south, north)  */
    double lonMin, lonMax;           /**< Longitude boundaries (west, east)   */
    double centroidLat, centroidLon; /**< Arithmetic centroid of the polygon  */
} CityGeo;

/**
 * @brief Loads city geometry from a GeoJSON file into the internal table.
 *
 * Allocates the hash table, memory-maps the file, parses every feature
 * and computes the bounding box and centroid for each municipality.
 * Optionally writes a JSON array of loaded city names to citiesOut.
 *
 * @pre geojsonPath is a readable ISTAT GeoJSON file.
 * @post On success the internal table is populated and ready for lookups.
 * @param geojsonPath Path to the ISTAT GeoJSON file.
 * @param citiesOut   Optional path to write the city-name JSON array (may be NULL).
 * @return Number of cities loaded on success, -1 on fatal error.
 */
int geo_init(const char *geojsonPath, const char *citiesOut);

/**
 * @brief Releases all resources allocated by geo_init().
 *
 * Safe to call even if geo_init() was never called or returned an error.
 * @post The internal table is destroyed and set to NULL.
 */
void geo_cleanup(void);

/**
 * @brief Looks up geometry data for a city by name.
 *
 * @pre geo_init() completed successfully.
 * @param city Name of the municipality to search for.
 * @param out  Output struct populated with geometry on success.
 * @return true if the city was found, false otherwise.
 */
bool geo_lookup(const char *city, CityGeo *out);

/**
 * @brief Axis-aligned bounding-box test for a coordinate pair.
 *
 * This is an approximation: the bounding box is larger than the actual
 * municipal polygon, so a point inside the box may still lie outside
 * the real boundary.
 *
 * @param geo Pointer to the city's geometry.
 * @param lat Latitude of the point to test.
 * @param lon Longitude of the point to test.
 * @return true if (lat, lon) falls within the bounding box.
 */
bool geo_contains(const CityGeo *geo, double lat, double lon);

#endif /* GEO_H */