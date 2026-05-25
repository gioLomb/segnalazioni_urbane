/**
 * @file connection_handler.c
 * @brief Networking layer: connection setup, I/O callbacks, response dispatch.
 *
 * Functions defined here
 * ──────────────────────
 *   setup_client      — allocates ClientCtx, inits handles, accepts socket
 *   on_connection     — libuv accept callback passed to uv_listen()
 *   on_buffer_alloc   — lazily allocates the receive buffer
 *   on_read           — accumulates bytes, parses HTTP, dispatches, responds
 *   on_write_complete — frees WriteReq; re-arms keepalive or closes
 *   on_timeout        — inactivity timeout → close_client()
 *   on_close          — final cleanup once both handles are closed
 *   close_client      — initiates graceful close of TCP + timer handles
 *   send_response     — serialises HttpResponse into WriteReq, calls uv_write()
 *   get_peer_ip       — resolves the IPv4 string of the peer
 */

#include "connection_handler.h"
#include "route_handler.h"
#include "rate_limiter.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>

/* ── Forward declarations ────────────────────────────────────────────── */

static void send_response(ClientCtx *ctx, const HttpResponse *resp, bool keepAlive);
static void on_write_complete(uv_write_t *req, int status);
static void on_timeout(uv_timer_t *handle);
static void on_close(uv_handle_t *handle);
static void get_peer_ip(uv_tcp_t *tcp, char *ip, size_t len);
static int  setup_client(uv_stream_t *server);

/* ── Private types ───────────────────────────────────────────────────── */

// WriteReq bundles the libuv write request with the response bytes in a
// single heap allocation so on_write_complete can free both at once.
// uv_write_t MUST be the first member — libuv casts WriteReq* to it.
// The flexible array holds the complete HTTP response (status + headers + body).
typedef struct {
    uv_write_t req;   // must be first — libuv casts WriteReq* to uv_write_t*
    ClientCtx *ctx;
    int keepAlive;
    char data[];
} WriteReq;

// Shared scratch buffer for handle_request() output. Safe because the event
// loop is single-threaded: only one request is dispatched at a time.
static char g_response_buffer[RESPONSE_BUFFER_SIZE];



// Serialises resp into a heap-allocated WriteReq (header + body in one
// contiguous block) and submits it via uv_write().
// The WriteReq is freed inside on_write_complete once the kernel has
// consumed the data. Closes the connection on allocation or render failure.
static void send_response(ClientCtx *ctx, const HttpResponse *resp, bool keepAlive) {
    size_t totalMax = MAX_HEADER_STR_LEN + resp->bodyLen;
    WriteReq *wr = malloc(sizeof(WriteReq) + totalMax);
    if (!wr) {
        close_client(ctx);
        return;
    }

    int total = http_response_render(resp, keepAlive, wr->data, totalMax);
    if (total < 0) {
        free(wr);
        close_client(ctx);
        return;
    }

    wr->ctx = ctx;
    wr->keepAlive = keepAlive;
    uv_buf_t buf = uv_buf_init(wr->data, (size_t)total);
    uv_write(&wr->req, (uv_stream_t *)&ctx->handle, &buf, 1, on_write_complete);
}


// Resolves the IPv4 address of the TCP peer into ip[].
// Sets ip[0] = '\0' on failure so callers can treat it as an empty string.
static void get_peer_ip(uv_tcp_t *tcp, char *ip, size_t len) {
    struct sockaddr_in peer;
    int plen = sizeof(peer);
    if (uv_tcp_getpeername(tcp, (struct sockaddr *)&peer, &plen) == 0){
        uv_inet_ntop(AF_INET, &peer.sin_addr, ip, len);
    }else{
        ip[0] = '\0';
    }
}


// Accepts one incoming connection from the listening socket, allocates a
// ClientCtx and registers it with the event loop.
// Returns 1 on success, 0 if the client cap is reached or allocation fails.
static int setup_client(uv_stream_t *server) {
    if (client_manager_active_count() >= MAX_CLIENTS) return 0;

    ClientCtx *ctx = client_manager_alloc();
    if (!ctx) return 0;

    uv_loop_t *loop = uv_default_loop();
    uv_tcp_init(loop, &ctx->handle);
    uv_timer_init(loop, &ctx->timer);
    // Store the ClientCtx pointer so callbacks can recover it with a cast.
    ctx->handle.data = ctx;
    ctx->timer.data = ctx;

    if (uv_accept(server, (uv_stream_t *)&ctx->handle) != 0) {
        close_client(ctx);
        return 0;
    }

    // TCP_NODELAY disables Nagle's algorithm for lower latency on small responses.
    uv_tcp_nodelay(&ctx->handle, 1);
    uv_timer_start(&ctx->timer, on_timeout, KEEPALIVE_TIMEOUT * 1000, 0);
    client_manager_link(ctx);
    uv_read_start((uv_stream_t *)&ctx->handle, on_buffer_alloc, on_read);
    return 1;
}


void on_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "on_connection error: %s\n", uv_strerror(status));
    } else {
        setup_client(server);
    }
}


void on_buffer_alloc(uv_handle_t *handle, size_t suggestedSize, uv_buf_t *buf) {
    (void)suggestedSize;
    ClientCtx *ctx = (ClientCtx *)handle->data;

    // Lazy allocation: the buffer is created only when data first arrives,
    // so idle keep-alive connections don't consume BUFFER_SIZE each.
    if (!ctx->buffer) {
        ctx->buffer = malloc(CLIENT_BUFFER_SIZE);
        if (!ctx->buffer) {
            buf->base = NULL;
            buf->len = 0;
            return;
        }
    }

    // Give libuv only the unused portion of the buffer.
    // Setting base = NULL signals "no space" and triggers a zero-length read.
    size_t remaining = CLIENT_BUFFER_SIZE - 1 - ctx->totalRead;
    buf->base = remaining ? ctx->buffer + ctx->totalRead : NULL;
    buf->len = remaining;
}


void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    (void)buf;
    ClientCtx *ctx = (ClientCtx *)stream->data;

    // nread < 0 covers EOF (UV_EOF) and all I/O errors.
    if (nread < 0) {
        close_client(ctx);
        return;
    }
    if (nread == 0) return;  // EAGAIN / no data yet

    // Accumulate bytes and NUL-terminate for the HTTP parser.
    ctx->totalRead += (size_t)nread;
    ctx->buffer[ctx->totalRead] = '\0';

    // Wait for more data if the full request has not yet arrived.
    if (!http_is_request_complete(ctx->buffer, ctx->totalRead)) return;

    // Stop reading and the keepalive timer: we own the connection
    // until send_response() hands it back to the event loop.
    uv_read_stop(stream);
    uv_timer_stop(&ctx->timer);

    // Rate-limit check
    char ip[INET6_ADDRSTRLEN] = {0};
    get_peer_ip(&ctx->handle, ip, sizeof(ip));
    if (DEBUG_RATE_LIMIT && !rate_limit_check(ip)) {
        HttpResponse r429 = {
            .statusCode = 429,
            .body = "Too Many Requests\n",
            .bodyLen = 18
        };
        send_response(ctx, &r429, false);
        return;
    }

    // Reject malformed requests immediately with a 400.
    HttpRequest req = {0};
    if (!http_request_parse(ctx->buffer, ctx->totalRead, &req)) {
        HttpResponse bad = {
            .statusCode = 400,
            .body = "<h1>400 Bad Request</h1>",
            .bodyLen = 24
        };
        send_response(ctx, &bad, false);
        return;
    }

    bool keepAlive = http_request_contains_keepalive(&req);

    // Dispatch: handle_request() writes the response into g_response_buffer.
    HttpResponse resp = { .statusCode = 200, .body = g_response_buffer };
    handle_request(&req, &resp);
    send_response(ctx, &resp, keepAlive);
}

/* ── Private callbacks ───────────────────────────────────────────────── */

static void on_write_complete(uv_write_t *req, int status) {
    // Recover the WriteReq from the embedded uv_write_t (safe: req is first member).
    WriteReq *wr = (WriteReq *)req;
    ClientCtx *ctx = wr->ctx;
    bool keepAlive = (bool)wr->keepAlive;
    free(wr);

    // Close on write error or when the client did not request keep-alive.
    if (status < 0 || !keepAlive) {
        close_client(ctx);
        return;
    }

    // Keep-alive: reset the accumulator, restart the idle timer and resume reading.
    ctx->totalRead = 0;
    uv_timer_start(&ctx->timer, on_timeout, KEEPALIVE_TIMEOUT * 1000, 0);
    uv_read_start((uv_stream_t *)&ctx->handle, on_buffer_alloc, on_read);
}

static void on_timeout(uv_timer_t *handle) {
    // Idle timeout expired: close the connection to free the slot.
    close_client((ClientCtx *)handle->data);
}

static void on_close(uv_handle_t *handle) {
    ClientCtx *ctx = (ClientCtx *)handle->data;
    // Both the TCP handle and the timer fire on_close; wait until both have
    // fired (pendingCloses starts at 2) before releasing the context, otherwise
    // it would be freed while the second callback still holds a pointer to it.
    if (--ctx->pendingCloses > 0) return;

    client_manager_unlink(ctx);
    client_manager_release(ctx);
}


void close_client(ClientCtx *ctx) {
    if (ctx->closing) return;   // guard against double-close
    ctx->closing = true;
    ctx->pendingCloses = 2;     // one for TCP handle, one for timer
    uv_close((uv_handle_t *)&ctx->handle, on_close);
    uv_close((uv_handle_t *)&ctx->timer,  on_close);
}