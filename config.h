#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <uv.h>

#define KEEPALIVE_TIMEOUT 10       /* idle connection timeout, seconds    */
#ifndef PORT
#define PORT 8080
#endif

#ifndef APP_DB_PATH
#define APP_DB_PATH "segnalacity.db"
#endif
#define MAX_EVENTS        4096     /* max events per uv_run() iteration   */
#define MAX_CLIENTS       16384    /* hard cap on simultaneous connections */
#define LISTEN_BACKLOG    65535    /* TCP listen backlog                  */

/* BUFFER_SIZE          - per-connection request accumulation buffer.
 * RESPONSE_BUFFER_SIZE - heap-allocated per-request response buffer;
 *                        must be large enough for the biggest HTML page. */
#define BUFFER_SIZE          (1 << 13)  /* 8 KB   */
#define RESPONSE_BUFFER_SIZE (1 << 18)  /* 256 KB */
#define URL_BUFFER_SIZE      (1 << 10)
#define PARAM_KEY_SIZE       (1 << 6)
#define PARAM_VALUE_SIZE     (1 << 10)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define GEO_JSON_PATH    "data/comuni.geojson"
#define CITIES_JSON_PATH "data/cities.json"

#endif /* CONFIG_H */
