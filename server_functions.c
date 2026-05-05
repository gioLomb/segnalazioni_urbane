/**
 * server_functions.c — libuv event loop, connection lifecycle, and main()
 *
 * Architecture overview
 * ─────────────────────
 *  main()
 *    │  initialise pool, DB, session table, rate-limit table, templates
 *    └─► server_loop(rateLimitTable)
 *          │  bind TCP socket on PORT
 *          │  register SIGINT handler (on_signal → uv_stop)
 *          │  register snapshot timer  (snapshot_cb every 5 min)
 *          └─► uv_run()  ← libuv event loop
 *                │
 *                ├─ on_connection()  ← new TCP connection arrives
 *                │    └─ setup_client()
 *                │         allocate ClientCtx from pool
 *                │         uv_tcp_init + uv_timer_init
 *                │         uv_accept → start keepalive timer + read
 *                │
 *                ├─ alloc_cb()  ← libuv asks where to store incoming bytes
 *                │    point into ctx->buffer at ctx->totalRead offset
 *                │
 *                ├─ read_cb()  ← bytes received
 *                │    accumulate → detect complete HTTP request
 *                │    rate-limit check → send 429 if exceeded
 *                │    handle_request() → send_response()
 *                │
 *                ├─ write_cb()  ← uv_write completed
 *                │    keep-alive: reset buffer, restart timer + read
 *                │    close:      close_client()
 *                │
 *                ├─ timer_cb()  ← keepalive timeout fired
 *                │    close_client()
 *                │
 *                └─ on_close()  ← one libuv handle fully closed
 *                     decrement pending_closes; when 0: unlink + pool release
 *
 * Connection close protocol
 * ─────────────────────────
 * Every ClientCtx embeds two libuv handles (tcp + timer).  Both must be
 * closed with uv_close() before the slot can be returned to the pool,
 * because libuv continues to reference the handle memory until the close
 * callback fires.
 *
 *   close_client(ctx)          — sets ctx->closing = true,
 *                                 ctx->pending_closes = 2,
 *                                 calls uv_close() on both handles.
 *
 *   on_close(handle)           — decrements ctx->pending_closes;
 *                                 when it reaches 0 both handles are done →
 *                                 unlink from active list, return slot to pool.
 *
 * The ctx->closing guard prevents double-close if close_client() is called
 * more than once for the same connection (e.g. write error + timer firing).
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
#include <arpa/inet.h>   /* AF_INET for uv_inet_ntop */

/* ── Exported globals (declared extern in server_functions.h) ────────── */

volatile sig_atomic_t keepRunning    = 1;
ServerStats           stats          = {0};
Hash_Table           *g_sessions     = NULL;
Hash_Table           *g_geo_table    = NULL;

unsigned long g_stat_requests    = 0;
unsigned long g_stat_connections = 0;
time_t        g_stat_start       = 0;

/* ── Loop-level state (private to this translation unit) ─────────────── */

static uv_loop_t  *g_loop           = NULL;
static ClientCtx  *g_clients_head   = NULL;   /* doubly-linked list of live conns */
static int         g_active_clients = 0;
static Hash_Table *g_rate_table     = NULL;

/* ── Write request type ──────────────────────────────────────────────── */

/*
 * WriteReq bundles the libuv bookkeeping (uv_write_t) with the response
 * bytes in a single allocation.  The flexible array member holds the full
 * HTTP response (header immediately followed by body), so no second malloc
 * is needed and the data remains alive until write_cb fires and frees it.
 *
 * IMPORTANT: uv_write_t must be the first member so that a WriteReq* can
 * be safely cast to uv_write_t* and vice versa, as required by libuv.
 */
typedef struct {
    uv_write_t  req;        /* must be first — see note above */
    ClientCtx  *ctx;
    int         keep_alive;
    char        data[];     /* HTTP header + body */
} WriteReq;

/* ── Forward declarations ────────────────────────────────────────────── */

static void close_client(ClientCtx *ctx);
static void on_close    (uv_handle_t *handle);
static void alloc_cb    (uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
static void read_cb     (uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void timer_cb    (uv_timer_t  *handle);
static void write_cb    (uv_write_t  *req, int status);
static void on_connection(uv_stream_t *server, int status);
static void snapshot_cb (uv_timer_t  *handle);
static void on_signal   (uv_signal_t *handle, int signum);
static int  rate_limit_check(const char *ip);
static void send_response(ClientCtx *ctx,
                          int statusCode, const char *body, size_t bodyLen,
                          const char *setCookie, const char *location,
                          int keepAlive,const char *content_type);

/* ══════════════════════════════════════════════════════════════════════
   SECTION 1 — Utility
   ══════════════════════════════════════════════════════════════════════ */

/**
 * MurmurHash-inspired hash function used for every Hash_Table in the app.
 * The per-table random seed mitigates hash-flooding attacks.
 */
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

/**
 * Installs SIG_IGN for SIGPIPE so that writing to a closed socket returns
 * EPIPE instead of killing the process.
 * SIGINT is handled inside the event loop via uv_signal_t (see on_signal).
 */
void config_signal_context(void) {
    signal(SIGPIPE, SIG_IGN);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 2 — HTTP response builder
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Builds a complete HTTP/1.1 response and submits it to libuv for sending.
 *
 * Content-Type is inferred from the first byte of body:
 *   '<'       → text/html; charset=utf-8
 *   '{' | '[' → application/json
 *   else      → text/plain; charset=utf-8
 *
 * The WriteReq (header + body in one allocation) is freed in write_cb
 * once libuv confirms delivery.
 */
static void send_response(ClientCtx *ctx,
                          int statusCode, const char *body, size_t bodyLen,
                          const char *setCookie, const char *location,
                          int keepAlive,const char *content_type) {
    /* Map status code to reason phrase. */
    const char *statusMsg;
    switch (statusCode) {
        case 200: statusMsg = "OK";                    break;
        case 302: statusMsg = "Found";                 break;
        case 400: statusMsg = "Bad Request";           break;
        case 401: statusMsg = "Unauthorized";          break;
        case 403: statusMsg = "Forbidden";             break;
        case 404: statusMsg = "Not Found";             break;
        case 405: statusMsg = "Method Not Allowed";    break;
        case 429: statusMsg = "Too Many Requests";     break;
        case 500: statusMsg = "Internal Server Error"; break;
        default:  statusMsg = "OK";                    break;
    }

    /* Infer Content-Type from the first character of the body. */
    const char *ct;

    if (content_type && content_type[0]) {
        ct = content_type;   
    } else if (body && body[0] == '<') {
        ct = "text/html; charset=utf-8";
    } else if (body && (body[0] == '{' || body[0] == '[')) {
        ct = "application/json";
    } else {
        ct = "text/plain; charset=utf-8";
    }

    /* Build the response header. */
    char hdr[1024];
    int  hlen = 0;
    hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n",
        statusCode, statusMsg, ct, bodyLen,
        keepAlive ? "keep-alive" : "close");
    if (setCookie && setCookie[0])
        hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
                         "Set-Cookie: %s\r\n", setCookie);
    if (location && location[0])
        hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
                         "Location: %s\r\n", location);
    hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen, "\r\n");

    /* Allocate one block: WriteReq metadata + header + body. */
    size_t    total = (size_t)hlen + bodyLen;
    WriteReq *wr    = malloc(sizeof(WriteReq) + total);
    if (!wr) { close_client(ctx); return; }

    wr->ctx        = ctx;
    wr->keep_alive = keepAlive;
    memcpy(wr->data,             hdr,  (size_t)hlen);
    if (bodyLen) memcpy(wr->data + hlen, body, bodyLen);

    uv_buf_t buf = uv_buf_init(wr->data, total);
    uv_write(&wr->req, (uv_stream_t *)&ctx->tcp, &buf, 1, write_cb);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 3 — libuv connection lifecycle callbacks
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Called when uv_write() completes (or fails).
 *
 * On success + keep-alive: reset the request buffer and restart reading
 *   so the connection can serve the next pipelined request.
 * On error or Connection: close: close_client().
 */
static void write_cb(uv_write_t *req, int status) {
    WriteReq  *wr         = (WriteReq *)req;
    ClientCtx *ctx        = wr->ctx;
    int        keep_alive = wr->keep_alive;
    free(wr);   /* safe: libuv is done with the buffer */

    if (status < 0 || !keep_alive) {
        close_client(ctx);
        return;
    }

    /* Reset for the next request on this connection. */
    ctx->totalRead = 0;
    memset(ctx->buffer, 0, BUFFER_SIZE);
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);
    uv_read_start((uv_stream_t *)&ctx->tcp, alloc_cb, read_cb);
}

/**
 * Called by libuv before each read to ask where to store incoming bytes.
 * We point directly into the unused tail of ctx->buffer so that each
 * chunk is appended without any extra copy.
 */
static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)suggested_size;
    ClientCtx *ctx       = (ClientCtx *)handle->data;
    size_t     remaining = BUFFER_SIZE - 1 - ctx->totalRead;

    if (remaining == 0) {
        buf->base = NULL;
        buf->len  = 0;
        return;
    }
    buf->base = ctx->buffer + ctx->totalRead;
    buf->len  = remaining;
}

/**
 * Called whenever libuv delivers bytes for a connection.
 *
 * Data is accumulated in ctx->buffer until a complete HTTP request is
 * detected.  "Complete" means:
 *   - All headers received (the "\r\n\r\n" terminator is present), AND
 *   - For POST requests: Content-Length body bytes have also arrived.
 *
 * Once complete, reading is stopped, the request is dispatched, and
 * a response is sent.
 */
static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    (void)buf;
    ClientCtx *ctx = (ClientCtx *)stream->data;

    if (nread < 0) {
        /* UV_EOF is normal (peer closed the connection). */
        close_client(ctx);
        return;
    }
    if (nread == 0) return;   /* EAGAIN equivalent — no data yet */

    ctx->totalRead            += (size_t)nread;
    ctx->buffer[ctx->totalRead] = '\0';   /* keep buffer NUL-terminated */

    /* ── Completeness check ──────────────────────────────────────────── */

    const char *hdr_end = memmem(ctx->buffer, ctx->totalRead, "\r\n\r\n", 4);
    if (!hdr_end) return;   /* headers not fully received yet */

    if (strncmp(ctx->buffer, "POST", 4) == 0) {
        const char *cl_hdr = strstr(ctx->buffer, "Content-Length:");
        if (cl_hdr) {
            long   content_length = atol(cl_hdr + 15);
            size_t hdr_bytes      = (size_t)(hdr_end + 4 - ctx->buffer);
            size_t body_received  = ctx->totalRead - hdr_bytes;
            if (content_length > 0 && body_received < (size_t)content_length)
                return;   /* body still arriving */
        }
    }

    /* Complete request — stop reading while we process it. */
    uv_read_stop(stream);
    uv_timer_stop(&ctx->timer);

    /* ── Rate limiting ───────────────────────────────────────────────── */

    char ip[INET6_ADDRSTRLEN] = {0};
    struct sockaddr_in peer;
    int plen = sizeof(peer);
    if (uv_tcp_getpeername(&ctx->tcp, (struct sockaddr *)&peer, &plen) == 0)
        uv_inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    if (DEBUG_RATE_LIMIT && !rate_limit_check(ip)) {
        send_response(ctx, 429, "Too Many Requests\n", 18, NULL, NULL, 0,NULL);
        return;
    }

    /* ── Dispatch ────────────────────────────────────────────────────── */

    char *resp = calloc(1, RESPONSE_BUFFER_SIZE);
    if (!resp) { close_client(ctx); return; }

    RouteExtra extra     = {0};
    int        keepAlive = 0;
    int statusCode = handle_request(ctx->buffer, resp, RESPONSE_BUFFER_SIZE,
                                    &extra, &keepAlive);
    g_stat_requests = ++stats.totalRequests;

    size_t bodyLen = (statusCode == 302) ? 0 : strlen(resp);
    send_response(ctx, statusCode, resp, bodyLen,
              extra.set_cookie[0] ? extra.set_cookie : NULL,
              extra.location[0]   ? extra.location   : NULL,
              keepAlive,
              extra.content_type[0] ? extra.content_type : NULL);
    free(resp);
}

/**
 * Keepalive timer fired — the connection has been idle for KEEPALIVE_TIMEOUT
 * seconds with no new data.  Close it.
 */
static void timer_cb(uv_timer_t *handle) {
    close_client((ClientCtx *)handle->data);
}

/**
 * Fires once for each libuv handle that finishes closing (tcp or timer).
 *
 * ClientCtx.pending_closes starts at 2 (one per handle).  When it reaches
 * 0 both handles are fully closed and the slot can safely be unlinked from
 * the active list and returned to the pool.
 */
static void on_close(uv_handle_t *handle) {
    ClientCtx *ctx = (ClientCtx *)handle->data;
    if (--ctx->pending_closes > 0) return;   /* other handle still closing */

    /* Unlink from the doubly-linked active-clients list. */
    if (ctx->prev) ctx->prev->next = ctx->next;
    else           g_clients_head  = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;

    client_pool_release(ctx);
    g_active_clients--;
    stats.activeClients = g_active_clients;
}

/**
 * Initiates the close sequence for a connection.
 *
 * Sets ctx->closing = true to guard against double-close (e.g. a write
 * error followed by the keepalive timer firing for the same ctx).
 * Sets ctx->pending_closes = 2 so on_close() knows to wait for both
 * the tcp and timer handles before releasing the pool slot.
 */
static void close_client(ClientCtx *ctx) {
    if (ctx->closing) return;
    ctx->closing        = true;
    ctx->pending_closes = 2;
    uv_close((uv_handle_t *)&ctx->tcp,   on_close);
    uv_close((uv_handle_t *)&ctx->timer, on_close);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 4 — Connection setup
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Accepts a new TCP connection and sets up its ClientCtx.
 *
 * BUG FIX: if uv_accept() fails, both handles have already been
 * initialised with uv_tcp_init / uv_timer_init.  We MUST close them
 * through close_client() (which sets closing=true and pending_closes=2)
 * rather than calling uv_close() directly, so that on_close() can
 * safely decrement the counter and free the slot.
 *
 * Returns 1 on success, 0 on failure (client limit reached or alloc error).
 */
static int setup_client(uv_stream_t *server) {
    if (g_active_clients >= MAX_CLIENTS) return 0;

    ClientCtx *ctx = client_pool_alloc();
    if (!ctx) return 0;

    /* Initialise libuv handles and bind the back-pointer. */
    uv_tcp_init(g_loop, &ctx->tcp);
    uv_timer_init(g_loop, &ctx->timer);
    ctx->tcp.data   = ctx;
    ctx->timer.data = ctx;

    if (uv_accept(server, (uv_stream_t *)&ctx->tcp) != 0) {
        /*
         * Accept failed after handles were initialised.
         * Use close_client() so that both handles are closed correctly
         * and on_close() can return the slot to the pool when done.
         */
        close_client(ctx);
        return 0;
    }

    uv_tcp_nodelay(&ctx->tcp, 1);   /* disable Nagle for lower latency */
    uv_timer_start(&ctx->timer, timer_cb, KEEPALIVE_TIMEOUT * 1000, 0);

    /* Prepend to the active-clients doubly-linked list. */
    ctx->next = g_clients_head;
    ctx->prev = NULL;
    if (g_clients_head) g_clients_head->prev = ctx;
    g_clients_head = ctx;

    g_active_clients++;
    g_stat_connections = ++stats.totalConnections;
    stats.activeClients = g_active_clients;

    uv_read_start((uv_stream_t *)&ctx->tcp, alloc_cb, read_cb);
    return 1;
}

/** Called by libuv when the server socket has a pending incoming connection. */
static void on_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "on_connection error: %s\n", uv_strerror(status));
        return;
    }
    setup_client(server);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 5 — Periodic snapshot & signal handler
   ══════════════════════════════════════════════════════════════════════ */

/** Persists the in-memory session table to disk every 5 minutes. */
static void snapshot_cb(uv_timer_t *handle) {
    (void)handle;
    ht_snapshot(g_sessions, "sessions.bin");
}

/**
 * SIGINT handler: sets keepRunning = 0 and stops the event loop cleanly.
 * The loop will fall through to the graceful-shutdown code in server_loop().
 */
static void on_signal(uv_signal_t *handle, int signum) {
    (void)signum;
    keepRunning = 0;
    uv_signal_stop(handle);
    uv_stop(g_loop);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 6 — Rate limiter
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Sliding-window rate limiter (one entry per unique IP address).
 *
 * Uses two counters (current window and previous window) to produce a
 * weighted estimate of the request rate, smoothing over window boundaries.
 *
 * Returns 1 if the request is within the limit, 0 if it should be rejected.
 *
 * The table is destroyed and recreated when it exceeds 10 000 entries to
 * bound memory usage (a simple DOS mitigation).
 */
static int rate_limit_check(const char *ip) {
    if (!ip || !ip[0] || !g_rate_table) return 1;

    /* Prevent unbounded memory growth from IP churn. */
    if (g_rate_table->size > 10000) {
        ht_destroy(g_rate_table, NULL);
        g_rate_table = ht_create(1024, hash_key);
        if (!g_rate_table) return 1;   /* allow on alloc failure */
    }

    RateEntry e   = {0};
    time_t    now = time(NULL);
    ht_get(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));

    /* Rotate windows when a full second has elapsed. */
    if (now - e.windowStartTime >= 1) {
        e.countPrev       = e.countCurr;
        e.countCurr       = 0;
        e.windowStartTime = now;
    }

    /* Weighted estimate: use the fraction of the previous window still "active". */
    double elapsed   = difftime(now, e.windowStartTime);
    double estimated = e.countPrev * (1.0 - elapsed) + e.countCurr;

    if (estimated >= RATE_LIMIT_RPS) {
        ht_set(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
        return 0;
    }
    e.countCurr++;
    ht_set(g_rate_table, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 7 — server_loop
   ══════════════════════════════════════════════════════════════════════ */

/**
 * Starts the libuv event loop and blocks until SIGINT is received.
 *
 * rateLimitTable is owned by main(); the loop reads and writes it in-place
 * but never frees it.
 *
 * Shutdown sequence:
 *   1. SIGINT → on_signal() → uv_stop() returns from uv_run().
 *   2. Close all active client connections.
 *   3. Run the loop once more to drain pending close callbacks.
 *   4. Close server-level handles (signal watcher, snapshot timer, server socket).
 *   5. Run once more for their close callbacks.
 *   6. uv_loop_close().
 */
void server_loop(Hash_Table *rateLimitTable) {
    g_rate_table = rateLimitTable;
    g_loop       = uv_default_loop();

    /* ── Bind and listen ────────────────────────────────────────────── */
    uv_tcp_t server;
    uv_tcp_init(g_loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", PORT, &addr);
    uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);

    int r = uv_listen((uv_stream_t *)&server, LISTEN_BACKLOG, on_connection);
    if (r) {
        fprintf(stderr, "uv_listen error: %s\n", uv_strerror(r));
        return;
    }
    printf("SegnalaCity listening on port %d\n", PORT);

    /* ── SIGINT watcher ─────────────────────────────────────────────── */
    uv_signal_t sig;
    uv_signal_init(g_loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    /* ── Periodic session snapshot (every 5 minutes) ────────────────── */
    uv_timer_t snap_timer;
    uv_timer_init(g_loop, &snap_timer);
    uv_timer_start(&snap_timer, snapshot_cb, 300000, 300000);

    /* ── Run ────────────────────────────────────────────────────────── */
    uv_run(g_loop, UV_RUN_DEFAULT);

    /* ── Graceful shutdown ──────────────────────────────────────────── */
    /* Step 1: initiate close on all active client connections. */
    ClientCtx *cur = g_clients_head;
    while (cur) {
        ClientCtx *next = cur->next;
        close_client(cur);
        cur = next;
    }
    /* Step 2: drain client close callbacks. */
    uv_run(g_loop, UV_RUN_DEFAULT);

    /* Step 3: close server-level handles, then drain their callbacks. */
    uv_close((uv_handle_t *)&sig,        NULL);
    uv_close((uv_handle_t *)&snap_timer, NULL);
    uv_close((uv_handle_t *)&server,     NULL);
    uv_run(g_loop, UV_RUN_DEFAULT);

    uv_loop_close(g_loop);
}

/* ══════════════════════════════════════════════════════════════════════
   SECTION 8 — main()
   ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    stats.startTime = time(NULL);
    g_stat_start    = stats.startTime;
    config_signal_context();

    /* ── Connection memory pool ─────────────────────────────────────── */
    if (client_pool_init() != 0) {
        fprintf(stderr, "Fatal: client pool init failed\n");
        return EXIT_FAILURE;
    }

    /* ── SQLite database ────────────────────────────────────────────── */
    if (db_init(APP_DB_PATH) != 0) {
        fprintf(stderr, "Fatal: cannot open database '%s': %s\n",
                APP_DB_PATH, db_errmsg());
        return EXIT_FAILURE;
    }
    if (user_setup_table()   != 0) {
        fprintf(stderr, "Fatal: user_setup_table failed\n");
        return EXIT_FAILURE;
    }
    if (report_setup_table() != 0) {
        fprintf(stderr, "Fatal: report_setup_table failed\n");
        return EXIT_FAILURE;
    }
    printf("Database: %s\n", APP_DB_PATH);

    /* ── HTML templates ─────────────────────────────────────────────── */
    if (tpl_load_all("templates") != 0) {
        fprintf(stderr, "Fatal: failed to load HTML templates\n");
        return EXIT_FAILURE;
    }
    if (tpl_load_file("templates/common.css", "common.css") != 0) {
        fprintf(stderr, "Fatal: failed to load templates/common.css\n");
        return EXIT_FAILURE;
    }

    /* ── City geometry table ────────────────────────────────────────── */
    g_geo_table = ht_create(8192, hash_key);
    if (!g_geo_table) {
        fprintf(stderr, "Fatal: geo table allocation failed\n");
        return EXIT_FAILURE;
    }
    if (geo_load(GEO_JSON_PATH, g_geo_table, CITIES_JSON_PATH) < 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", GEO_JSON_PATH);
        return EXIT_FAILURE;
    }
    if (tpl_load_file(CITIES_JSON_PATH, "cities.json") != 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", CITIES_JSON_PATH);
        return EXIT_FAILURE;
    }

    /* ── In-memory session table ────────────────────────────────────── */
    g_sessions = ht_create(0, hash_key);
    if (!g_sessions) {
        fprintf(stderr, "Fatal: session table allocation failed\n");
        return EXIT_FAILURE;
    }
    if (ht_load(g_sessions, "sessions.bin"))
        printf("Sessions: %zu session(s) restored from sessions.bin\n",
               g_sessions->size);

    /* ── Rate-limit table ───────────────────────────────────────────── */
    Hash_Table *rate_limit = ht_create(1024, hash_key);
    if (!rate_limit) {
        fprintf(stderr, "Fatal: rate-limit table allocation failed\n");
        return EXIT_FAILURE;
    }

    /* ── Run ────────────────────────────────────────────────────────── */
    server_loop(rate_limit);

    /* ── Cleanup ────────────────────────────────────────────────────── */
    tpl_unload_all();
    ht_destroy(g_sessions,  "sessions.bin");
    ht_destroy(g_geo_table, NULL);
    ht_destroy(rate_limit,  NULL);
    client_pool_destroy();
    db_close();
    printf("Shutdown complete.\n");
    return EXIT_SUCCESS;
}