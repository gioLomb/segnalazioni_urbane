/**
 * server_functions.c — libuv event loop, connection lifecycle, and main()
 *
 * File layout
 * ───────────
 *   Section 1  — Exported globals
 *   Section 2  — Private state + WriteReq type
 *   Section 3  — Utility          (hash_key)
 *   Section 4  — HTTP response    (send_response)
 *   Section 5  — Request helpers  (get_peer_ip)
 *   Section 6  — Connection lifecycle (alloc_cb, read_cb, write_cb,
 *                                       timer_cb, on_close, close_client)
 *   Section 7  — Connection setup (setup_client, on_connection)
 *   Section 8  — Rate limiter     (rate_limit_check)
 *   Section 9  — Server loop      (on_signal, server_bind, server_shutdown)
 *   Section 10 — Startup + main() (init_db, init_templates, init_geo_table,
 *                                   init_sessions, main)
 *
 * main() algorithm at a glance
 * ────────────────────────────
 *   signal(SIGPIPE, SIG_IGN)
 *   assert: conn_manager_init, init_db, init_templates, init_geo_table,
 *           init_sessions, rate_table alloc        ← fail-fast, each init
 *                                                     prints its own diagnostic
 *   server_bind()    — TCP bind + listen + SIGINT arm
 *   uv_run()         — blocks until SIGINT
 *   server_shutdown()— drain callbacks + destroy all subsystems
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
#include <assert.h>
#include <arpa/inet.h>

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Exported globals
   ══════════════════════════════════════════════════════════════════════ */

Hash_Table *g_sessions  = NULL;
Hash_Table *g_geo_table = NULL;

/* ══════════════════════════════════════════════════════════════════════
   SECTION 2 — Private state + WriteReq type
   ══════════════════════════════════════════════════════════════════════ */

static Hash_Table *g_rate_table     = NULL;
static char        g_response_buffer[RESPONSE_BUFFER_SIZE];

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

/* ══════════════════════════════════════════════════════════════════════
   SECTION 4 — HTTP response builder
   ══════════════════════════════════════════════════════════════════════ */

/* http_response_render() (status line, headers, body) lives in http_utils.c */

/**
 * Serializes an HttpResponse into a WriteReq and submits it via uv_write().
 * The WriteReq (header + body in one block) is freed in write_cb.
 */
static void send_response(ClientCtx *ctx, const HttpResponse *resp, bool keep_alive) {
    size_t    total_max = MAX_HEADER_STR_LEN + resp->body_len;
    WriteReq *wr        = malloc(sizeof(WriteReq) + total_max);
    if (!wr) { close_client(ctx); return; }

    int total = http_response_render(resp, keep_alive, wr->data, total_max);
    if (total < 0) { free(wr); close_client(ctx); return; }

    wr->ctx        = ctx;
    wr->keep_alive = keep_alive;
    uv_buf_t buf   = uv_buf_init(wr->data, (size_t)total);
    uv_write(&wr->req, (uv_stream_t *)&ctx->handle, &buf, 1, write_cb);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — Request helpers
   ══════════════════════════════════════════════════════════════════════ */

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
    ClientCtx *ctx = (ClientCtx *)handle->data;

    if (!ctx->buffer) {
        ctx->buffer = malloc(BUFFER_SIZE);
        if (!ctx->buffer) { buf->base = NULL; buf->len = 0; return; }
    }

    size_t remaining = BUFFER_SIZE - 1 - ctx->totalRead;
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

    if (!http_is_request_complete(ctx->buffer, ctx->totalRead)) return;

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

    HttpRequest req = {0};
    if (!http_request_parse(ctx->buffer, ctx->totalRead, &req)) {
        HttpResponse bad = { .status_code = 400,
                             .body        = "<h1>400 Bad Request</h1>",
                             .body_len    = 24 };
        send_response(ctx, &bad, false);
        return;
    }

    bool keep_alive = http_request_contains_keepalive(&req);

    HttpResponse resp = { .status_code = 200, .body = g_response_buffer };
    handle_request(&req, &resp);
    send_response(ctx, &resp, keep_alive);
}

static void write_cb(uv_write_t *req, int status) {
    WriteReq  *wr         = (WriteReq *)req;
    ClientCtx *ctx        = wr->ctx;
    bool       keep_alive = (bool)wr->keep_alive;
    free(wr);

    if (status < 0 || !keep_alive) { close_client(ctx); return; }

    ctx->totalRead = 0;
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
    uv_close((uv_handle_t *)&ctx->handle, on_close);
    uv_close((uv_handle_t *)&ctx->timer,  on_close);
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
    ctx->handle.data = ctx;
    ctx->timer.data  = ctx;

    if (uv_accept(server, (uv_stream_t *)&ctx->handle) != 0) {
        close_client(ctx);
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
    if (unlikely(!ip || !ip[0] || !g_rate_table)) return 1;

    if (unlikely(g_rate_table->size > 10000)) {
        ht_destroy(g_rate_table, NULL);
        g_rate_table = ht_create(1024, hash_key);
        if (!g_rate_table) return 1;
    }

    RateEntry e   = {0};
    time_t    now = time(NULL);
    ht_get(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));

    if (unlikely(now - e.windowStartTime >= 1)) {
        e.countPrev       = e.countCurr;
        e.countCurr       = 0;
        e.windowStartTime = now;
    }

    double elapsed   = difftime(now, e.windowStartTime);
    double estimated = e.countPrev * (1.0 - elapsed) + e.countCurr;
    int    allowed   = likely(estimated < RATE_LIMIT_RPS);

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

/**
 * Initialises the TCP server handle, binds to PORT, starts listening,
 * and arms the SIGINT signal handler.
 * Called by main() before uv_run(). Returns 0 on success, -1 on error.
 */
static int server_bind(uv_loop_t *loop, uv_tcp_t *server, uv_signal_t *sig) {
    struct sockaddr_in addr;
    uv_tcp_init(loop, server);
    uv_ip4_addr("0.0.0.0", PORT, &addr);
    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);
    int r = uv_listen((uv_stream_t *)server, LISTEN_BACKLOG, on_connection);
    if (r) { fprintf(stderr, "uv_listen error: %s\n", uv_strerror(r)); return -1; }

    uv_signal_init(loop, sig);
    uv_signal_start(sig, on_signal, SIGINT);
    return 0;
}

/**
 * Graceful shutdown sequence:
 *   1. Close all active client connections + libuv handles
 *   2. Drain remaining close callbacks with a final uv_run()
 *   3. Destroy all application subsystems in reverse-init order:
 *      templates → sessions → geo table → rate table → conn pool → db
 */
static void server_shutdown(uv_loop_t *loop, uv_tcp_t *server, uv_signal_t *sig) {
    conn_manager_close_all(close_client);
    uv_close((uv_handle_t *)sig,    NULL);
    uv_close((uv_handle_t *)server, NULL);

    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);

    tpl_unload_all();
    session_destroy_all();
    geo_cleanup();;
    ht_destroy(g_rate_table, NULL);
    conn_manager_destroy();
    db_close();

    printf("Shutdown complete.\n");
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
    // g_geo_table = ht_create(8192, hash_key);
    // if (!g_geo_table) { fprintf(stderr, "Fatal: geo table allocation failed\n"); return -1; }
    if (geo_init(GEO_JSON_PATH, CITIES_JSON_PATH) < 0) {
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
    return session_init();
}

/**
 * main() — server algorithm at a glance
 *
 *  1. Ignore SIGPIPE (broken pipes must not kill the process)
 *  2. Init all subsystems — single assert guarantees fail-fast if any returns -1;
 *     each init prints its own diagnostic before returning, so the failing step
 *     is always visible in the log.
 *  3. Bind TCP socket + arm SIGINT handler   (assert on failure)
 *  4. Run the event loop                     (blocks until SIGINT)
 *  5. Graceful shutdown                      (drain callbacks, destroy all state)
 */
int main(void) {
    /* ── Declarations ── */
    uv_loop_t  *loop;
    uv_tcp_t    server;
    uv_signal_t sig;

    /* ── 1. Signal disposition ── */
    signal(SIGPIPE, SIG_IGN);

    /* ── 2. Subsystem initialisation ── */
    assert( conn_manager_init() == 0 &&
            init_db()           == 0 &&
            init_templates()    == 0 &&
            init_geo_table()    == 0 &&
            init_sessions()     == 0 &&
            (g_rate_table = ht_create(1024, hash_key)) != NULL
            && "Fatal: startup failed — see stderr for details" );

    /* ── 3. Bind socket and arm SIGINT ── */
    loop = uv_default_loop();
    assert(server_bind(loop, &server, &sig) == 0 && "Fatal: server_bind");

    /* ── 4. Event loop (blocks here until SIGINT) ── */
    printf("SegnalaCity listening on port %d\n", PORT);
    uv_run(loop, UV_RUN_DEFAULT);

    /* ── 5. Graceful shutdown + full cleanup ── */
    server_shutdown(loop, &server, &sig);
    return EXIT_SUCCESS;
}