/**
 * This module parses incoming HTTP requests and dispatches them to the
 * appropriate route handler. Routes are stored in a static array of Route
 * structs; each entry pairs a URL path prefix with a handler function that
 * reads query parameters, interacts with the hash table, and writes a
 * plain-text or JSON response into the caller-supplied buffer.
 * All handler functions return an HTTP status code and write a human-readable
 * message (or a JSON object for /get) into responseBuffer.
 */

#ifndef ROUTE_HANDLER_H
#define ROUTE_HANDLER_H

#include "config.h"
#include "hash_table.h"

/**
 * Associates a URL path prefix with its handler function.
 * handle_request() iterates over the static routes array and calls the first
 * handler whose path is a prefix of the extracted URL.
 * The handler receives the full URL string, the shared hash table, and a
 * buffer to fill with the response body; it returns an HTTP status code.
 */
typedef struct Route{
    char *path;
    int (*handler)(Hash_Table* table, const char* url, char* responseBuffer);
} Route;

/**
 * Parses an HTTP request from requestBuffer, extracts the URL from the
 * first request line using strtok_r (re-entrant and therefore thread-safe),
 * and dispatches it to the first matching route handler.
 * The handler fills responseBuffer with the response body.
 * Returns 200 on success, 400 if the first line is missing or malformed,
 * 404 if no registered route matches the URL.
 */
int handle_request(Hash_Table* db, char *requestBuffer, char *responseBuffer,int *keepAlive);

#endif /* ROUTE_HANDLER_H */