/**
 * @file geo.h
 * @brief City geometry module for indexing and validating geographical data.
 * This module allows loading city boundaries (bounding boxes) and centroids
 * from GeoJSON files into a hash table for fast O(1) lookups.
 */

#ifndef GEO_H
#define GEO_H

#include "hash_table.h"
#include <stdbool.h>

/**
 * @brief Geometry data for a specific municipality (comune).
 * This structure stores the bounding box and the calculated centroid
 * used for spatial validation and map centering.
 */
typedef struct {
    double lat_min, lat_max;    /**< Latitude boundaries */
    double lon_min, lon_max;    /**< Longitude boundaries */
    double centroid_lat, centroid_lon; /**< Calculated center point */
} CityGeo;

/**
 * @brief Parses a GeoJSON file and populates the hash table with city data.
 * @pre path points to a valid GeoJSON file, ht is an initialized Hash_Table.
 * @post Every valid feature in the file is inserted into ht, keyed by city name.
 * @param path Path to the ISTAT GeoJSON file.
 * @param ht Pointer to the destination hash table.
 * @param cities_out Optional path to save a JSON list of loaded city names.
 * @return Number of cities loaded on success, -1 on fatal error.
 */
int geo_load(const char *path, Hash_Table *ht, const char *cities_out);

/**
 * @brief Retrieves geometry information for a city by its name.
 * @pre ht is initialized, comune is a null-terminated string.
 * @post if found, the out structure is populated with the city's geometry.
 * @param ht Pointer to the hash table.
 * @param comune Name of the city to search for.
 * @param out Pointer to a CityGeo structure where results will be stored.
 * @return true if the city was found, false otherwise.
 */
bool geo_lookup(Hash_Table * restrict ht, const char * restrict comune, CityGeo * restrict out);

/**
 * @brief Performs a fast bounding box check for a coordinate.
 * @param geo Pointer to the city geometry.
 * @param lat Latitude to check.
 * @param lon Longitude to check.
 * @return true if the coordinates fall within the city's bounding box.
 */
bool geo_contains(const CityGeo *geo, double lat, double lon);

#endif /* GEO_H */