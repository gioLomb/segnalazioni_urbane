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

#include "http_utils.h"
#include "connection_manager.h"
#include <stdio.h>
#include <arpa/inet.h>

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Exported globals
   ══════════════════════════════════════════════════════════════════════ */
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

/* Section 4 (HTTP response helpers) lives in http_utils.c:
 * http_response_render() handles status line, headers, body serialization. */

/**
 * Serializes an HttpResponse into a WriteReq and submits it via uv_write().
 * http_response_render() handles header building and content-type inference.
 * The WriteReq (rendered response in one block) is freed in write_cb.
 */
static void send_response(ClientCtx *ctx, const HttpResponse *resp, bool keep_alive) {
    /* Stima dimensione header (mai oltre 512 byte) */
    size_t total_max = 512 + resp->body_len;
    WriteReq *wr = malloc(sizeof(WriteReq) + total_max);
    if (!wr) { close_client(ctx); return; }

    int total = http_response_render(resp, keep_alive, wr->data, total_max);
    if (total < 0) { free(wr); close_client(ctx); return; }

    wr->ctx        = ctx;
    wr->keep_alive = keep_alive;
    uv_buf_t buf = uv_buf_init(wr->data, (size_t)total);
    uv_write(&wr->req, (uv_stream_t *)&ctx->handle, &buf, 1, write_cb);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — Request helpers
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Returns true when buf contains a complete HTTP request.
 *
 * Uses phr_parse_request for header detection:
 *   -2 → headers not yet complete → wait for more data
 *   -1 → malformed request       → pass to route handler for a 400
 *   ≥0 → headers complete        → for POST: verify body has arrived
 *
 * Content-Length is read directly from the parsed header array —
 * no manual strstr or atol on raw bytes.
 */
static bool is_request_complete(const char *buf, size_t len) {
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t            num_headers = HTTP_MAX_HEADERS;
    const char       *method, *path;
    size_t            method_len, path_len;
    int               minor_version;

    int r = phr_parse_request(buf, len,
                               &method, &method_len,
                               &path,   &path_len,
                               &minor_version,
                               headers, &num_headers,
                               0 /* last_len: always fresh parse */);

    if (r == -2) return false;   /* headers incomplete — wait */
    if (r == -1) return true;    /* malformed — let handler return 400 */

    /* For POST requests verify the body has fully arrived. */
    if (method_len != 4 || memcmp(method, "POST", 4) != 0) return true;

    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len != 14) continue;
        if (strncasecmp(headers[i].name, "content-length", 14) != 0) continue;

        char   val[24] = {0};
        size_t vlen    = headers[i].value_len < 23 ? headers[i].value_len : 23;
        memcpy(val, headers[i].value, vlen);
        long cl = strtol(val, NULL, 10);
        if (cl <= 0) break;

        size_t body_received = len - (size_t)r;
        return body_received >= (size_t)cl;
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
    get_peer_ip(&ctx->handle, ip, sizeof(ip));
    if (DEBUG_RATE_LIMIT && !rate_limit_check(ip)) {
        HttpResponse r429 = { .status_code = 429,
                               .body        = "Too Many Requests\n",
                               .body_len    = 18 };
        send_response(ctx, &r429, false);
        return;
    }

    /* Parse request — already complete, so phr will not return -2 */
    HttpRequest req = {0};
    if (!http_request_parse(ctx->buffer, ctx->totalRead, &req)) {
        HttpResponse bad = { .status_code = 400,
                             .body        = "<h1>400 Bad Request</h1>",
                             .body_len    = 24 };
        send_response(ctx, &bad, false);
        return;
    }

    bool keep_alive = http_request_keep_alive(&req);

    /* Allocate body buffer and dispatch */
    char *body_buf = calloc(1, RESPONSE_BUFFER_SIZE);
    if (!body_buf) { close_client(ctx); return; }

    HttpResponse resp = { .status_code = 200, .body = body_buf };
    handle_request(&req, &resp);

    send_response(ctx, &resp, keep_alive);
    free(body_buf);
}

static void write_cb(uv_write_t *req, int status) {
    WriteReq  *wr         = (WriteReq *)req;
    ClientCtx *ctx        = wr->ctx;
    bool       keep_alive = (bool)wr->keep_alive;
    free(wr);

    if (status < 0 || !keep_alive) { close_client(ctx); return; }

    ctx->totalRead = 0;
    memset(ctx->buffer, 0, BUFFER_SIZE);
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);
    uv_read_start((uv_stream_t *)&ctx->handle, alloc_cb, read_cb);
}

static void timer_cb(uv_timer_t *handle) {
    close_client((ClientCtx *)handle->data);
}

static void on_close(uv_handle_t *handle) {
    ClientCtx *ctx = (ClientCtx *)handle->data;
    if (--ctx->pending_closes > 0) return;

    conn_manager_unlink(ctx);
    conn_manager_release(ctx);
}

static void close_client(ClientCtx *ctx) {
    if (ctx->closing) return;
    ctx->closing        = true;
    ctx->pending_closes = 2;
    uv_close((uv_handle_t *)&ctx->handle,   on_close);
    uv_close((uv_handle_t *)&ctx->timer, on_close);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7 — Connection setup
   ══════════════════════════════════════════════════════════════════════ */

static int setup_client(uv_stream_t *server) {
    if (conn_manager_active_count() >= MAX_CLIENTS) return 0;

    ClientCtx *ctx = conn_manager_alloc();
    if (!ctx) return 0;

    uv_loop_t *loop = uv_default_loop();
    uv_tcp_init(loop, &ctx->handle);
    uv_timer_init(loop, &ctx->timer);
    ctx->handle.data   = ctx;
    ctx->timer.data = ctx;

    if (uv_accept(server, (uv_stream_t *)&ctx->handle) != 0) {
        close_client(ctx);   /* handles are initialised: must close via pool */
        return 0;
    }

    uv_tcp_nodelay(&ctx->handle, 1);
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);

    conn_manager_link(ctx);

    uv_read_start((uv_stream_t *)&ctx->handle, alloc_cb, read_cb);
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
    conn_manager_close_all(close_client);
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
                       "templates/admin_map.html",
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
    config_signal_context();

    if (conn_manager_init() != 0) {
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
    conn_manager_destroy();
    db_close();
    printf("Shutdown complete.\n");
    return EXIT_SUCCESS;
}