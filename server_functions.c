#include "server_functions.h"
#include "route_handler.h"
#include "user.h"
#include "report.h"
#include "session.h"
#include "db.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

/* ── Globals ─────────────────────────────────────────────────────────── */

volatile sig_atomic_t keepRunning = 1;
ServerStats           stats       = {0};
Hash_Table           *g_sessions  = NULL;   /* session token → userId */

/* Simple counters exported for route_handler.c stats endpoint */
unsigned long g_stat_requests    = 0;
unsigned long g_stat_connections = 0;
time_t        g_stat_start       = 0;   /* owned here, used by route_handler */

/* ── Static prototypes ───────────────────────────────────────────────── */

static void    handle_sigint(int sig);
static ServerCtx start_server(int port);
static int     set_nonblocking(int fd);
static int     make_timerfd(void);
static int     reset_timer(ClientCtx *ctx);
static void    close_client(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients);
static int     setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *activeClients);
static void    accept_connections(ServerCtx sctx, ClientCtx **head, int *activeClients);
static void    handle_timerEvent(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients);
static int     handle_socket_event(int epollFd, ClientCtx *ctx,
                                   Hash_Table **rateLimitTable,
                                   ClientCtx **head, int *activeClients);
static void    check_snapshot(ServerCtx sctx);
static int     rate_limit_check(Hash_Table **rateLimitTablePtr, const char *ip);
static inline  void set_snapshot_timer(int epollFd, int *snap_tfd, int *sentinel);

/* ── send_response ───────────────────────────────────────────────────── */

void send_response(int socketFd, int statusCode,
                   const char *body, size_t bodyLen,
                   const char *setCookie, const char *location,
                   int keepAlive) {

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

    /* Infer Content-Type from first body byte */
    const char *ct;
    if (body && (body[0] == '<'))
        ct = "text/html; charset=utf-8";
    else if (body && (body[0] == '{' || body[0] == '['))
        ct = "application/json";
    else
        ct = "text/plain; charset=utf-8";

    /* Build header into a stack buffer (headers are always small) */
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

    if (write(socketFd, hdr, (size_t)hlen) < 0) { /* best-effort */ }

    size_t total = 0;
    while (total < bodyLen) {
        ssize_t n = write(socketFd, body + total, bodyLen - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
}

/* ── Signal + misc helpers ───────────────────────────────────────────── */

static void handle_sigint(int sig) { (void)sig; keepRunning = 0; }

void config_signal_context(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}


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

/* ── Timer / client helpers ──────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_timerfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd == -1) return -1;
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) { close(tfd); return -1; }
    return tfd;
}

static int reset_timer(ClientCtx *ctx) {
    if (ctx->timerEv.fd == -1) return 0;
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    return timerfd_settime(ctx->timerEv.fd, 0, &ts, NULL);
}

static void close_client(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients) {
    if (ctx->prev) ctx->prev->next = ctx->next;
    else           *head           = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;

    epoll_ctl(epollFd, EPOLL_CTL_DEL, ctx->sockEv.fd, NULL);
    close(ctx->sockEv.fd);
    if (ctx->timerEv.fd != -1) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, ctx->timerEv.fd, NULL);
        close(ctx->timerEv.fd);
    }
    client_pool_release(ctx);
    (*activeClients)--;
    stats.activeClients = *activeClients;
}

static int setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *activeClients) {
    if (set_nonblocking(clientFd) == -1) goto fail;
    int yes = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    ClientCtx *ctx = client_pool_alloc();
    if (!ctx) goto fail;

    int tfd = make_timerfd();
    ctx->sockEv  = (ConnectionEvent){ .fd = clientFd, .type = TYPE_SOCKET, .parent = ctx };
    ctx->timerEv = (ConnectionEvent){ .fd = tfd,      .type = TYPE_TIMER,  .parent = ctx };

    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = &ctx->sockEv };
    if (epoll_ctl(sctx.epollFd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
        if (tfd != -1) close(tfd);
        client_pool_release(ctx); goto fail;
    }
    if (tfd != -1) {
        struct epoll_event tev = { .events = EPOLLIN, .data.ptr = &ctx->timerEv };
        epoll_ctl(sctx.epollFd, EPOLL_CTL_ADD, tfd, &tev);
    }
    ctx->next = *head; ctx->prev = NULL;
    if (*head) (*head)->prev = ctx;
    *head = ctx;
    (*activeClients)++;
    stats.activeClients = *activeClients;
    stats.totalConnections++;
    g_stat_connections = stats.totalConnections;
    return 1;
fail:
    close(clientFd);
    return 0;
}

static void accept_connections(ServerCtx sctx, ClientCtx **head, int *activeClients) {
    while (1) {
        int fd = accept(sctx.serverFd, NULL, NULL);
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept"); break;
        }
        if (*activeClients >= MAX_CLIENTS) { close(fd); continue; }
        setup_client(sctx, fd, head, activeClients);
    }
}

static void handle_timerEvent(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients) {
    uint64_t exp;
    if (read(ctx->timerEv.fd, &exp, sizeof(exp)) < 0) { /* drain */ }
    close_client(epollFd, ctx, head, activeClients);
}

/* ── Core socket event handler ───────────────────────────────────────── */

static int handle_socket_event(int epollFd, ClientCtx *ctx,
                                Hash_Table **rateLimitTable,
                                ClientCtx **head, int *activeClients) {
    /* Heap-allocated response: large enough for the operator Leaflet page */
    char *resp = calloc(1, RESPONSE_BUFFER_SIZE);
    if (!resp) { close_client(epollFd, ctx, head, activeClients); return 0; }

    memset(ctx->buffer, 0, BUFFER_SIZE);
    size_t totalRead = 0;
    int    keepAlive = 0;

    /* Rate limiting */
    struct sockaddr_in peer; socklen_t plen = sizeof(peer);
    char ip[INET_ADDRSTRLEN] = {0};
    if (getpeername(ctx->sockEv.fd, (struct sockaddr *)&peer, &plen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    if (DEBUG_RATE_LIMIT && !rate_limit_check(rateLimitTable, ip)) {
        send_response(ctx->sockEv.fd, 429, "Too Many Requests\n", 18, NULL, NULL, 0);
        free(resp);
        close_client(epollFd, ctx, head, activeClients);
        return 0;
    }

    /* Read request headers (stop at \r\n\r\n) */
    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t n = read(ctx->sockEv.fd, ctx->buffer + totalRead, BUFFER_SIZE - 1 - totalRead);
        if (n > 0) {
            totalRead += (size_t)n;
            if (memmem(ctx->buffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (n == 0) {
            goto close_conn;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalRead == 0) { reset_timer(ctx); free(resp); return 1; }
                break;
            }
            goto close_conn;
        }
    }

    /*
     * For POST requests, read the body after the blank line.
     * We honour Content-Length but cap at remaining buffer space to avoid
     * overflows; a real server would stream oversized bodies.
     */
    if (totalRead >= 4 && strncmp(ctx->buffer, "POST", 4) == 0) {
        const char *cl_hdr = strstr(ctx->buffer, "Content-Length:");
        if (cl_hdr) {
            long cl = atol(cl_hdr + 15);
            const char *hdr_end = strstr(ctx->buffer, "\r\n\r\n");
            if (hdr_end && cl > 0) {
                size_t hdr_len   = (size_t)(hdr_end + 4 - ctx->buffer);
                size_t have_body = totalRead - hdr_len;
                size_t need      = (size_t)cl > have_body ? (size_t)cl - have_body : 0;
                while (need > 0 && totalRead < BUFFER_SIZE - 1) {
                    ssize_t n = read(ctx->sockEv.fd, ctx->buffer + totalRead,
                                     BUFFER_SIZE - 1 - totalRead);
                    if (n <= 0) break;
                    totalRead += (size_t)n;
                    need      -= ((size_t)n < need) ? (size_t)n : need;
                }
            }
        }
    }
    ctx->buffer[totalRead] = '\0';

    /* Dispatch */
    RouteExtra extra = {0};
    int statusCode = handle_request(ctx->buffer, resp, RESPONSE_BUFFER_SIZE,
                                    &extra, &keepAlive);
    stats.totalRequests++;
    g_stat_requests = stats.totalRequests;

    size_t bodyLen = (statusCode == 302) ? 0 : strlen(resp);
    send_response(ctx->sockEv.fd, statusCode, resp, bodyLen,
                  extra.set_cookie[0] ? extra.set_cookie : NULL,
                  extra.location[0]  ? extra.location  : NULL,
                  keepAlive);
    free(resp);
    resp = NULL;

    if (keepAlive) { reset_timer(ctx); return 1; }

close_conn:
    free(resp);
    close_client(epollFd, ctx, head, activeClients);
    return 0;
}

/* ── Snapshot ────────────────────────────────────────────────────────── */

static void check_snapshot(ServerCtx sctx) {
    (void)sctx;
    /* Sessions are the only in-memory table still persisted to disk.
     * Reports live exclusively in SQLite and need no separate snapshot. */
    ht_snapshot(g_sessions, "sessions.bin");
}

/* ── Rate limiter ────────────────────────────────────────────────────── */

static int rate_limit_check(Hash_Table **tblPtr, const char *ip) {
    if (!ip || !ip[0]) return 1;
    Hash_Table *t = *tblPtr;
    if (t->size > 10000) {
        ht_destroy(t, NULL);
        t = ht_create(1024, hash_key);
        if (!t) return 1;
        *tblPtr = t;
    }
    RateEntry e = {0};
    time_t now = time(NULL);
    ht_get(t, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
    if (now - e.windowStartTime >= 1) {
        e.countPrev = e.countCurr;
        e.countCurr = 0;
        e.windowStartTime = now;
    }
    double elapsed   = difftime(now, e.windowStartTime);
    double estimated = e.countPrev * (1.0 - elapsed) + e.countCurr;
    if (estimated >= RATE_LIMIT_RPS) {
        ht_set(t, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
        return 0;
    }
    e.countCurr++;
    ht_set(t, (void *)ip, strlen(ip) + 1, &e, sizeof(e));
    return 1;
}

/* ── Server init + loop ──────────────────────────────────────────────── */

static ServerCtx start_server(int port) {
    ServerCtx ctx;
    ctx.serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.serverFd == -1) { perror("socket"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(ctx.serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(ctx.serverFd);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)port)
    };
    if (bind(ctx.serverFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind");   exit(EXIT_FAILURE); }
    if (listen(ctx.serverFd, LISTEN_BACKLOG) < 0){ perror("listen"); exit(EXIT_FAILURE); }
    ctx.epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epollFd == -1) { perror("epoll_create1"); exit(EXIT_FAILURE); }
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = NULL };
    epoll_ctl(ctx.epollFd, EPOLL_CTL_ADD, ctx.serverFd, &ev);
    printf("SegnalaCity server listening on port %d ...\n", port);
    return ctx;
}

static inline void set_snapshot_timer(int epollFd, int *snap_tfd, int *sentinel) {
    *snap_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec ts = { .it_interval = {300,0}, .it_value = {300,0} };
    timerfd_settime(*snap_tfd, 0, &ts, NULL);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = sentinel };
    epoll_ctl(epollFd, EPOLL_CTL_ADD, *snap_tfd, &ev);
}

void server_loop(ServerCtx sctx, Hash_Table *rateLimitTable) {
    struct epoll_event events[MAX_EVENTS];
    ClientCtx *head        = NULL;
    int        activeClients = 0;
    stats.activeClients     = 0;

    static int snap_sentinel = 0;
    int snap_tfd = -1;
    set_snapshot_timer(sctx.epollFd, &snap_tfd, &snap_sentinel);

    while (keepRunning) {
        int nev = epoll_wait(sctx.epollFd, events, MAX_EVENTS, -1);
        if (nev == -1) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < nev; i++) {
            ConnectionEvent *ev = (ConnectionEvent *)events[i].data.ptr;

            if (ev == NULL) { accept_connections(sctx, &head, &activeClients); continue; }
            if (ev == (ConnectionEvent *)&snap_sentinel) {
                { uint64_t _e=0; if(read(snap_tfd,&_e,sizeof(_e))<0){} }
                check_snapshot(sctx);
                continue;
            }

            ClientCtx *ctx = ev->parent;
            if (ctx->closing) continue;
            if (ev->type == TYPE_TIMER) {
                ctx->closing = 1;
                handle_timerEvent(sctx.epollFd, ctx, &head, &activeClients);
                continue;
            }
            handle_socket_event(sctx.epollFd, ctx, &rateLimitTable, &head, &activeClients);
        }
    }

    while (head) close_client(sctx.epollFd, head, &head, &activeClients);
    if (snap_tfd != -1) close(snap_tfd);
    close(sctx.epollFd);
    close(sctx.serverFd);
}

/* ══════════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    stats.startTime = time(NULL);
    g_stat_start = stats.startTime;
    config_signal_context();

    /* --- Memory pool --- */
    if (client_pool_init() == -1) {
        fprintf(stderr, "Critical: client pool init failed\n");
        return EXIT_FAILURE;
    }

    /* --- SQLite --- */
    if (db_init(APP_DB_PATH) != 0) {
        fprintf(stderr, "Critical: cannot open database '%s': %s\n",
                APP_DB_PATH, db_errmsg());
        return EXIT_FAILURE;
    }
    if (user_setup_table()   != 0) { fprintf(stderr, "user_setup_table failed\n");   return EXIT_FAILURE; }
    if (report_setup_table() != 0) { fprintf(stderr, "report_setup_table failed\n"); return EXIT_FAILURE; }

    /* --- In-memory tables --- */
    g_sessions = ht_create(0, hash_key);
    if (!g_sessions) {
        fprintf(stderr, "Critical: hash table allocation failed\n");
        return EXIT_FAILURE;
    }

    /* --- Persistence: restore sessions from disk if the file exists --- */
    if (ht_load(g_sessions, "sessions.bin"))
        printf("Persistence: %zu sessioni ripristinate da sessions.bin\n",
               g_sessions->size);

    Hash_Table *rateLimit = ht_create(1024, hash_key);
    if (!rateLimit) { fprintf(stderr, "Critical: rate limit table alloc failed\n"); return EXIT_FAILURE; }

    printf("Database:  %s\n", APP_DB_PATH);

    /* --- Run --- */
    server_loop(start_server(PORT), rateLimit);

    /* --- Cleanup --- */
    ht_destroy(g_sessions, "sessions.bin");
    ht_destroy(rateLimit, NULL);
    client_pool_destroy();
    db_close();

    printf("Shutdown complete.\n");
    return 0;
}