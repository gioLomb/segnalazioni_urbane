#ifndef CONFIG_H
#define CONFIG_H

/* STANDARD LIBRARY */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* MACROS */
#define KEEPALIVE_TIMEOUT    5
#define PORT                 8080
#define MAX_EVENTS           4096
#define MAX_CLIENTS          16384
#define BUFFER_SIZE          (1<<10)
#define URL_BUFFER_SIZE      (1<<10)
#define PARAM_KEY_SIZE       (1<<6)
#define PARAM_VALUE_SIZE     (1<<10)
#define RESPONSE_BUFFER_SIZE (PARAM_VALUE_SIZE * 2 + 256)
#define LISTEN_BACKLOG       65535
#define RATE_LIMIT_RPS 100
#define DEBUG_RATE_LIMIT 1

#endif