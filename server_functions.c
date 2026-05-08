/**
 * server_functions.c — libuv event loop, connection lifecycle, and main()
 *
 * File layout
 * ───────────
 *   Section 1  — Exported globals
 *   Section 2  — Private state + WriteReq type
 *   Section 3  — Utility          (hash_key, config_signal_context)
 *   Section 4  — HTTP response    (http_status_msg, infer_content_type,
 *                                   build_http_header, send_response)
 *   Section 5  — Request helpers  (is_request_complete, get_peer_ip)
 *   Section 6  — Connection lifecycle (alloc_cb, read_cb, write_cb,
 *                                       timer_cb, on_close, close_client)
 *   Section 7  — Connection setup (setup_client, on_connection)
 *   Section 8  — Rate limiter     (rate_limit_check)
 *   Section 9  — Server loop      (on_signal, server_bind, server_shutdown,
 *                                   server_loop)
 *   Section 10 — Startup          (init_db, init_templates, init_geo_table,
 *                                   init_sessions, main)
 */

#include "server_functions.h"
#include "route_handler.h"
#include "template.h"
#include "user.h"
#include "report.h"
#include "geo.h"
#include "session.h"
#include "db.h"

#include <stdio.h>
#include <arpa/inet.h>

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Exported globals
   ══════════════════════════════════════════════════════════════════════ */

ServerStats  stats      = {0};
Hash_Table  *g_sessions  = NULL;
Hash_Table  *g_geo_table = NULL;

/* ══════════════════════════════════════════════════════════════════════
   SECTION 2 — Private state + WriteReq type
   ══════════════════════════════════════════════════════════════════════ */

static Hash_Table *g_rate_table = NULL;

/*
 * WriteReq bundles the libuv write request with the response bytes in one
 * allocation.  uv_write_t MUST be the first member (libuv cast requirement).
 * The flexible array holds the full HTTP response (header + body).
 */
typedef struct {
    uv_write_t  req;        /* must be first */
    ClientCtx  *ctx;
    int         keep_alive;
    char        data[];
} WriteReq;

/* Forward declarations for callbacks referenced before their definition. */
static void close_client (ClientCtx *ctx);
static void on_close     (uv_handle_t *handle);
static void alloc_cb     (uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
static void read_cb      (uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void timer_cb     (uv_timer_t  *handle);
static void write_cb     (uv_write_t  *req, int status);
static void on_connection(uv_stream_t *server, int status);
static int  rate_limit_check(const char *ip);

/* ══════════════════════════════════════════════════════════════════════
   SECTION 3 — Utility
   ══════════════════════════════════════════════════════════════════════ */

unsigned long hash_key(const void *key, size_t keySize, unsigned long seed) {
    const unsigned char *data = (const unsigned char *)key;
    unsigned long h = seed;
    for (size_t i = 0; i < keySize; i++) {
        h ^= data[i];
        h *= 0x5bd1e995UL;
        h ^= h >> 15;
    }
    h ^= h >> 13;
    h *= 0x85ebca6bUL;
    h ^= h >> 16;
    return h;
}

void config_signal_context(void) {
    signal(SIGPIPE, SIG_IGN);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 4 — HTTP response builder
   ══════════════════════════════════════════════════════════════════════ */

static const char *http_status_msg(int code) {
    switch (code) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

static const char *infer_content_type(const char *body, const char *override) {
    if (override && override[0])                  return override;
    if (body && body[0] == '<')                   return "text/html; charset=utf-8";
    if (body && (body[0] == '{' || body[0] == '[')) return "application/json";
    return "text/plain; charset=utf-8";
}

static int build_http_header(char *hdr, size_t hdr_max,
                              int status_code, const char *content_type,
                              size_t body_len, int keep_alive,
                              const char *set_cookie, const char *location) {
    int n = snprintf(hdr, hdr_max,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n",
        status_code, http_status_msg(status_code),
        content_type, body_len,
        keep_alive ? "keep-alive" : "close");

    if (set_cookie && set_cookie[0])
        n += snprintf(hdr + n, hdr_max - (size_t)n,
                      "Set-Cookie: %s\r\n", set_cookie);
    if (location && location[0])
        n += snprintf(hdr + n, hdr_max - (size_t)n,
                      "Location: %s\r\n", location);

    n += snprintf(hdr + n, hdr_max - (size_t)n, "\r\n");
    return n;
}

/**
 * Allocates a WriteReq (header + body in one block) and submits it via
 * uv_write().  Freed in write_cb once libuv confirms delivery.
 */
static void send_response(ClientCtx *ctx,
                          int status_code, const char *body, size_t body_len,
                          const char *set_cookie, const char *location,
                          int keep_alive, const char *content_type) {
    char hdr[1024];
    int  hdr_len = build_http_header(
            hdr, sizeof(hdr), status_code,
            infer_content_type(body, content_type),
            body_len, keep_alive, set_cookie, location);

    WriteReq *wr = malloc(sizeof(WriteReq) + (size_t)hdr_len + body_len);
    if (!wr) { close_client(ctx); return; }

    wr->ctx        = ctx;
    wr->keep_alive = keep_alive;
    memcpy(wr->data,                 hdr,  (size_t)hdr_len);
    if (body_len) memcpy(wr->data + hdr_len, body, body_len);

    uv_buf_t buf = uv_buf_init(wr->data, (size_t)hdr_len + body_len);
    uv_write(&wr->req, (uv_stream_t *)&ctx->tcp, &buf, 1, write_cb);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — Request helpers
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Returns true when buf contains a complete HTTP request.
 * For POST, waits until Content-Length body bytes have been received.
 */
static bool is_request_complete(const char *buf, size_t len) {
    const char *hdr_end = memmem(buf, len, "\r\n\r\n", 4);
    if (!hdr_end) return false;

    if (strncmp(buf, "POST", 4) == 0) {
        const char *cl = strstr(buf, "Content-Length:");
        if (cl) {
            long   content_len   = atol(cl + 15);
            size_t hdr_bytes     = (size_t)(hdr_end + 4 - buf);
            size_t body_received = len - hdr_bytes;
            if (content_len > 0 && body_received < (size_t)content_len)
                return false;
        }
    }
    return true;
}

/** Resolves the IPv4 address of the TCP peer into ip. */
static void get_peer_ip(uv_tcp_t *tcp, char *ip, size_t len) {
    struct sockaddr_in peer;
    int plen = sizeof(peer);
    if (uv_tcp_getpeername(tcp, (struct sockaddr *)&peer, &plen) == 0)
        uv_inet_ntop(AF_INET, &peer.sin_addr, ip, len);
    else
        ip[0] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 6 — Connection lifecycle callbacks
   ══════════════════════════════════════════════════════════════════════ */

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)suggested_size;
    ClientCtx *ctx       = (ClientCtx *)handle->data;
    size_t     remaining = BUFFER_SIZE - 1 - ctx->totalRead;
    buf->base = remaining ? ctx->buffer + ctx->totalRead : NULL;
    buf->len  = remaining;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    (void)buf;
    ClientCtx *ctx = (ClientCtx *)stream->data;

    if (nread < 0) { close_client(ctx); return; }
    if (nread == 0) return;

    ctx->totalRead             += (size_t)nread;
    ctx->buffer[ctx->totalRead] = '\0';

    if (!is_request_complete(ctx->buffer, ctx->totalRead)) return;

    uv_read_stop(stream);
    uv_timer_stop(&ctx->timer);

    char ip[INET6_ADDRSTRLEN] = {0};
    get_peer_ip(&ctx->tcp, ip, sizeof(ip));
    if (DEBUG_RATE_LIMIT && !rate_limit_check(ip)) {
        send_response(ctx, 429, "Too Many Requests\n", 18, NULL, NULL, 0, NULL);
        return;
    }

    char *resp = calloc(1, RESPONSE_BUFFER_SIZE);
    if (!resp) { close_client(ctx); return; }

    RouteExtra extra      = {0};
    int        keep_alive = 0;
    int        status     = handle_request(ctx->buffer, resp,
                                           RESPONSE_BUFFER_SIZE,
                                           &extra, &keep_alive);
    stats.totalRequests++;

    size_t body_len = (status == 302) ? 0 : strlen(resp);
    send_response(ctx, status, resp, body_len,
                  extra.set_cookie[0]   ? extra.set_cookie   : NULL,
                  extra.location[0]     ? extra.location     : NULL,
                  keep_alive,
                  extra.content_type[0] ? extra.content_type : NULL);
    free(resp);
}

static void write_cb(uv_write_t *req, int status) {
    WriteReq  *wr         = (WriteReq *)req;
    ClientCtx *ctx        = wr->ctx;
    int        keep_alive = wr->keep_alive;
    free(wr);

    if (status < 0 || !keep_alive) { close_client(ctx); return; }

    ctx->totalRead = 0;
    memset(ctx->buffer, 0, BUFFER_SIZE);
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);
    uv_read_start((uv_stream_t *)&ctx->tcp, alloc_cb, read_cb);
}

static void timer_cb(uv_timer_t *handle) {
    close_client((ClientCtx *)handle->data);
}

static void on_close(uv_handle_t *handle) {
    ClientCtx *ctx = (ClientCtx *)handle->data;
    if (--ctx->pending_closes > 0) return;

    client_pool_unlink(ctx);
    client_pool_release(ctx);
    stats.activeClients = client_pool_active_count();
}

static void close_client(ClientCtx *ctx) {
    if (ctx->closing) return;
    ctx->closing        = true;
    ctx->pending_closes = 2;
    uv_close((uv_handle_t *)&ctx->tcp,   on_close);
    uv_close((uv_handle_t *)&ctx->timer, on_close);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7 — Connection setup
   ══════════════════════════════════════════════════════════════════════ */

static int setup_client(uv_stream_t *server) {
    if (client_pool_active_count() >= MAX_CLIENTS) return 0;

    ClientCtx *ctx = client_pool_alloc();
    if (!ctx) return 0;

    uv_loop_t *loop = uv_default_loop();
    uv_tcp_init(loop, &ctx->tcp);
    uv_timer_init(loop, &ctx->timer);
    ctx->tcp.data   = ctx;
    ctx->timer.data = ctx;

    if (uv_accept(server, (uv_stream_t *)&ctx->tcp) != 0) {
        close_client(ctx);   /* handles are initialised: must close via pool */
        return 0;
    }

    uv_tcp_nodelay(&ctx->tcp, 1);
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);

    client_pool_link(ctx);
    stats.totalConnections++;
    stats.activeClients = client_pool_active_count();

    uv_read_start((uv_stream_t *)&ctx->tcp, alloc_cb, read_cb);
    return 1;
}

static void on_connection(uv_stream_t *server, int status) {
    if (status < 0)
        fprintf(stderr, "on_connection error: %s\n", uv_strerror(status));
    else
        setup_client(server);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 8 — Rate limiter
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Sliding-window rate limiter.  Returns 1 if within the limit, 0 to reject.
 * Recycles the hash table when it exceeds 10 000 entries to bound memory.
 */
static int rate_limit_check(const char *ip) {
    if (!ip || !ip[0] || !g_rate_table) return 1;

    if (g_rate_table->size > 10000) {
        ht_destroy(g_rate_table, NULL);
        g_rate_table = ht_create(1024, hash_key);
        if (!g_rate_table) return 1;
    }

    RateEntry e   = {0};
    time_t    now = time(NULL);
    ht_get(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));

    if (now - e.windowStartTime >= 1) {
        e.countPrev       = e.countCurr;
        e.countCurr       = 0;
        e.windowStartTime = now;
    }

    double elapsed   = difftime(now, e.windowStartTime);
    double estimated = e.countPrev * (1.0 - elapsed) + e.countCurr;
    int    allowed   = estimated < RATE_LIMIT_RPS;

    if (allowed) e.countCurr++;
    ht_set(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
    return allowed;
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 9 — Server loop
   ══════════════════════════════════════════════════════════════════════ */

static void on_signal(uv_signal_t *handle, int signum) {
    (void)signum;
    uv_signal_stop(handle);
    uv_stop(uv_default_loop());
}

/** Binds and begins listening on PORT. Returns 0 on success, -1 on error. */
static int server_bind(uv_tcp_t *server) {
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", PORT, &addr);
    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);
    int r = uv_listen((uv_stream_t *)server, LISTEN_BACKLOG, on_connection);
    if (r) { fprintf(stderr, "uv_listen error: %s\n", uv_strerror(r)); return -1; }
    return 0;
}

/** Graceful shutdown: close all clients, drain callbacks, close server handles. */
static void server_shutdown(uv_loop_t *loop,
                             uv_tcp_t *server, uv_signal_t *sig) {
    client_pool_close_all(close_client);
    uv_run(loop, UV_RUN_DEFAULT);       /* drain client close callbacks */

    uv_close((uv_handle_t *)sig,    NULL);
    uv_close((uv_handle_t *)server, NULL);
    uv_run(loop, UV_RUN_DEFAULT);       /* drain server handle callbacks */

    uv_loop_close(loop);
    fprintf(stderr, "Shutdown: event loop closed.\n");
}

void server_loop(Hash_Table *rate_limit_table) {
    g_rate_table = rate_limit_table;

    uv_loop_t  *loop = uv_default_loop();
    uv_tcp_t    server;
    uv_signal_t sig;

    uv_tcp_init(loop, &server);
    if (server_bind(&server) != 0) return;

    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    printf("SegnalaCity listening on port %d\n", PORT);
    uv_run(loop, UV_RUN_DEFAULT);

    server_shutdown(loop, &server, &sig);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 10 — Startup helpers + main()
   ══════════════════════════════════════════════════════════════════════ */

static int init_db(void) {
    if (db_init(APP_DB_PATH) != 0) {
        fprintf(stderr, "Fatal: cannot open database '%s': %s\n",
                APP_DB_PATH, db_errmsg());
        return -1;
    }
    if (user_setup_table()   != 0) { fprintf(stderr, "Fatal: user_setup_table\n");   return -1; }
    if (report_setup_table() != 0) { fprintf(stderr, "Fatal: report_setup_table\n"); return -1; }
    printf("Database: %s\n", APP_DB_PATH);
    return 0;
}

static int init_templates(void) {
    if (tpl_load_files("templates/login.html",
                       "templates/register.html",
                       "templates/citizen_home.html",
                       "templates/operator_map.html",
                       "templates/submit.html",
                       "templates/common.css",
                       NULL) != 0) {
        fprintf(stderr, "Fatal: failed to load core templates\n");
        return -1;
    }
    return 0;
}

static int init_geo_table(void) {
    g_geo_table = ht_create(8192, hash_key);
    if (!g_geo_table) {
        fprintf(stderr, "Fatal: geo table allocation failed\n");
        return -1;
    }
    if (geo_load(GEO_JSON_PATH, g_geo_table, CITIES_JSON_PATH) < 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", GEO_JSON_PATH);
        return -1;
    }
    if (tpl_load_files(CITIES_JSON_PATH, NULL) != 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", CITIES_JSON_PATH);
        return -1;
    }
    return 0;
}

static int init_sessions(void) {
    g_sessions = ht_create(0, hash_key);
    if (!g_sessions) {
        fprintf(stderr, "Fatal: session table allocation failed\n");
        return -1;
    }
    return 0;
}

int main(void) {
    stats.startTime = time(NULL);
    config_signal_context();

    if (client_pool_init() != 0) {
        fprintf(stderr, "Fatal: client pool init failed\n");
        return EXIT_FAILURE;
    }
    if (init_db()        != 0) return EXIT_FAILURE;
    if (init_templates() != 0) return EXIT_FAILURE;
    if (init_geo_table() != 0) return EXIT_FAILURE;
    if (init_sessions()  != 0) return EXIT_FAILURE;

    Hash_Table *rate_limit = ht_create(1024, hash_key);
    if (!rate_limit) {
        fprintf(stderr, "Fatal: rate-limit table allocation failed\n");
        return EXIT_FAILURE;
    }

    server_loop(rate_limit);

    tpl_unload_all();
    ht_destroy(g_sessions,  NULL);
    ht_destroy(g_geo_table, NULL);
    ht_destroy(rate_limit,  NULL);
    client_pool_destroy();
    db_close();
    printf("Shutdown complete.\n");
    return EXIT_SUCCESS;
}