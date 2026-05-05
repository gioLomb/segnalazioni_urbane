#ifndef CONFIG_H
#define CONFIG_H

/* STANDARD LIBRARY */
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

/* libuv — replaces epoll, timerfd, fcntl, raw socket calls */
#include <uv.h>

/* SERVER TUNING */
#define KEEPALIVE_TIMEOUT    10
#define PORT                 8080
#define MAX_EVENTS           4096
#define MAX_CLIENTS          16384
#define LISTEN_BACKLOG       65535
#define RATE_LIMIT_RPS       100
#define DEBUG_RATE_LIMIT     1

/* BUFFER SIZES
 * BUFFER_SIZE           - per-connection request accumulation buffer.
 * RESPONSE_BUFFER_SIZE  - heap-allocated per-request response buffer;
 *                         must fit the largest HTML page (operator map). */
#define BUFFER_SIZE          (1 << 13)   /* 8 KB   */
#define RESPONSE_BUFFER_SIZE (1 << 18)   /* 256 KB */
#define URL_BUFFER_SIZE      (1 << 10)
#define PARAM_KEY_SIZE       (1 << 6)
#define PARAM_VALUE_SIZE     (1 << 10)

/* APPLICATION */
#define APP_DB_PATH         "segnalacity.db"
#define GEO_JSON_PATH       "data/comuni.geojson"
#define CITIES_JSON_PATH    "data/cities.json"
#define SESSION_COOKIE_NAME "sid"
#define SESSION_MAX_AGE     86400        /* 24 h in seconds */

#endif /* CONFIG_H */