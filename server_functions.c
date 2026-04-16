#include "server_functions.h"

#include "route_handler.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

volatile sig_atomic_t keepRunning = 1;
ServerStats stats = {0};

/* STATIC FUNCTION PROTOTYPES */

/**
 * SIGINT handler: writes a short message to stdout and clears keepRunning
 * so server_loop() exits cleanly on the next epoll_wait() iteration.
 * Only async-signal-safe functions are used.
 */
static void handle_sigint(int sig);

/**
 * Creates and configures the listening TCP socket (SO_REUSEADDR, non-blocking,
 * bind, listen), then creates an epoll instance and registers the server fd
 * with EPOLLIN | EPOLLET and data.ptr = NULL (sentinel for the event loop).
 * Calls exit(EXIT_FAILURE) on any fatal error. Returns a fully initialised ServerCtx.
 */
static ServerCtx start_server(int port);

/**
 * Adds O_NONBLOCK to fd via fcntl(), preserving all existing flags.
 * Returns 0 on success, -1 if either fcntl() call fails.
 */
static int set_nonblocking(int fd);

/**
 * Creates a one-shot CLOCK_MONOTONIC timerfd armed to fire after
 * KEEPALIVE_TIMEOUT seconds. Returns the fd on success, -1 on failure.
 */
static int make_timerfd(void);

/**
 * Re-arms ctx->timerEv.fd to KEEPALIVE_TIMEOUT seconds from now.
 * Does nothing if timerEv.fd is -1.It returns 0 on success, -1 otherwise.
 */
static int reset_timer(ClientCtx *ctx);

/**
 * Unlinks ctx from the live-client list, deregisters and closes both fds,
 * frees the ClientCtx, and decrements *activeClients.
 */
static void close_client(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients);

/**
 * Allocates and fully initialises a ClientCtx for an already-accepted clientFd.
 * Returns 1 on success, 0 on any failure (resources are released internally;
 * the caller is responsible only for closing clientFd on failure).
 */
static int setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *activeClients);

/**
 * Drains all pending connections from the listening socket, calling
 * setup_client() for each one. Drops connections when MAX_CLIENTS is
 * reached or when setup_client() fails, logging the reason to stderr.
 */
static void accept_connections(ServerCtx sctx, ClientCtx **head, int *activeClients);

/**
 * Handles a fired timerfd event: consumes the expiration counter and closes
 * the owning client connection.
 */
static void handle_timerEvent(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients);

/**
 * Reads the pending HTTP request from ctx->sockEv.fd, dispatches it to
 * handle_request(), and sends the response. Resets the timer and returns 1
 * on keep-alive, calls close_client() and returns 0 otherwise.
 */
static int handle_socket_event(int epollFd, ClientCtx *ctx, Hash_Table *db,
                                Hash_Table *rateLimitTable, ClientCtx **head, int *activeClients);

/**
 * check_snapshot - Periodically triggers a database dump based on a timer.
 * Checks the snapshot timerfd and, if expired, calls ht_snapshot to persist 
 * the current hash table state to the specified path.
 */
static void check_snapshot(Hash_Table *db, const char *path,ServerCtx sctx);

/**
 * Implements a sliding window rate limiting algorithm.
 * Tracks requests per IP in the rateLimitTable; returns 1 if the request is 
 * within the allowed RATE_LIMIT_RPS, 0 if the limit has been exceeded.
 */
static int rate_limit_check(Hash_Table **rateLimitTable, const char *ip);

/**
 * Checks if a file path is safe to use.
 * This function ensures that the provided path is relative and does not 
 * attempt to "climb" out of the server's intended directory using ".." 
 * sequences. It also restricts characters to a safe 
 * whitelist to prevent unexpected behavior with special shell characters.
 * Returns 1 if the path is clear, or 0 if it looks suspicious.
 */
static int is_safe_path(const char *path);


/* DEFINITIONS */


unsigned long hash_key(const void *key, size_t keySize, unsigned long seed) {
    const unsigned char *data = (const unsigned char *)key;
    unsigned long h = seed;

    // Byte-by-byte processing with MurmurHash magic constant
    for (size_t i = 0; i < keySize; i++) {
        h ^= data[i];
        h *= 0x5bd1e995; // 32-bit MurmurHash2 constant for bit dispersion
        h ^= h >> 15;
    }

    // Finalization Mix: forces all bits of the hash to avalanche
    // to ensure that even similar keys result in very different indices.
    h ^= h >> 13;
    h *= 0x85ebca6b;
    h ^= h >> 16;

    return h;
}

static void handle_sigint(int sig) {
    (void)sig;
    keepRunning = 0;
}

void config_signal_context(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    //avoid zombie son process
    signal(SIGCHLD, SIG_IGN);
    //avoid crash due to write on a closed socket
    signal(SIGPIPE, SIG_IGN);
}

static int is_safe_path(const char *path) {
    if (!path || path[0] == '\0') return 0;
    if (path[0] == '/')           return 0; 

    const char *p = path;
    int segLen = 0;
    for (; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '/' && c != '_' && c != '-' && c != '.') return 0;
        if (c == '/') {
            if (segLen == 2 && p[-2] == '.' && p[-1] == '.') return 0;
            segLen = 0;
        } else {
            segLen++;
        }
    }
    if (segLen == 2 && p[-2] == '.' && p[-1] == '.') return 0;
    return 1;
}

void analyze_args(int argc, char **argv, int *idxLoad, int *idxSave) {
    *idxLoad = *idxSave = -1;
    
    if (argc > 5) {
        fprintf(stderr, "Usage:\n  %s <file>\n  %s -ls <file>\n  %s -l <file> -s <file>\n",
                argv[0], argv[0], argv[0]);
        exit(EXIT_FAILURE);
    }
    if (argc == 2 && argv[1][0] != '-') { *idxLoad = *idxSave = 1; return; }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ls") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1 || *idxSave != -1) {
                fprintf(stderr, "Error: duplicate load/save flags\n"); exit(EXIT_FAILURE);
            }
            if(!is_safe_path(argv[i+1])){
                fprintf(stderr, "Error: input file is prohibited\n");
                exit(EXIT_FAILURE);
            }
            *idxLoad = *idxSave = i + 1; return;
        }
        if (strcmp(argv[i], "-l") == 0 && (i + 1) < argc) {
            if (*idxLoad != -1) { fprintf(stderr, "Error: duplicate -l flag\n"); exit(EXIT_FAILURE); }

            //check load path safety
            if(!is_safe_path(argv[i+1])){
                fprintf(stderr, "Error: load file is prohibited\n");
                exit(EXIT_FAILURE);
            }
            *idxLoad = i + 1;
        }
        if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
            if (*idxSave != -1) { fprintf(stderr, "Error: duplicate -s flag\n"); exit(EXIT_FAILURE); }

            //check save path safety
            if(!is_safe_path(argv[i+1])){
                fprintf(stderr, "Error: save file is prohibited\n");
                exit(EXIT_FAILURE);
            }
            *idxSave = i + 1;
        }
    }
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_timerfd(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd == -1) return -1;

    //timer follows the keepalive timeout
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(tfd, 0, &ts, NULL) == -1) { close(tfd); return -1; }
    return tfd;
}

static int reset_timer(ClientCtx *ctx) {
    if (ctx->timerEv.fd == -1) return 0; 
    struct itimerspec ts = { .it_interval = {0,0}, .it_value = {KEEPALIVE_TIMEOUT,0} };
    if (timerfd_settime(ctx->timerEv.fd, 0, &ts, NULL) == -1) {
        perror("timerfd_settime failed");
        return -1;
    }
    return 0;
}

static void close_client(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients) {    
    //delete from list
    if (ctx->prev) ctx->prev->next = ctx->next;
    else *head = ctx->next;
    if (ctx->next) ctx->next->prev = ctx->prev;

    //delete epoll instance
    epoll_ctl(epollFd, EPOLL_CTL_DEL, ctx->sockEv.fd, NULL);
    close(ctx->sockEv.fd);

    if (ctx->timerEv.fd != -1) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, ctx->timerEv.fd, NULL);
        close(ctx->timerEv.fd);
    }

    client_pool_release(ctx);
    (*activeClients)--;
}

static int setup_client(ServerCtx sctx, int clientFd, ClientCtx **head, int *activeClients) {
    ClientCtx *ctx = NULL;
    int tfd = -1;

    //async option
    if (set_nonblocking(clientFd) == -1) {
        perror("set_nonblocking failed");
        goto fail;
    }

    //avoid Nagle's algorithm and latency
    int yes = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)); 

    ctx = client_pool_alloc(); 
    if (!ctx) {
        fprintf(stderr, "Pool exhausted or allocation failed\n");
        goto fail;
    }

    tfd = make_timerfd();
    if (tfd == -1)
        fprintf(stderr, "warn: make_timerfd failed (%s) - no keepalive timer\n",strerror(errno));

    ctx->sockEv = (ConnectionEvent){ .fd = clientFd, .type = TYPE_SOCKET, .parent = ctx };
    ctx->timerEv = (ConnectionEvent){ .fd = tfd, .type = TYPE_TIMER, .parent = ctx };

    //set epoll instances
    struct epoll_event cev = { .events = EPOLLIN, .data.ptr = &ctx->sockEv };
    if (epoll_ctl(sctx.epollFd, EPOLL_CTL_ADD, clientFd, &cev) == -1) {
        perror("epoll_ctl clientFd failed");
        goto fail;
    }

    if (tfd != -1) {
        struct epoll_event tev = { .events = EPOLLIN, .data.ptr = &ctx->timerEv };
        if (epoll_ctl(sctx.epollFd, EPOLL_CTL_ADD, tfd, &tev) == -1) {
            perror("epoll_ctl timerfd failed");
            epoll_ctl(sctx.epollFd, EPOLL_CTL_DEL, clientFd, NULL);
            goto fail;
        }
    }

    //add to head
    ctx->next = *head;
    ctx->prev = NULL;
    if (*head) (*head)->prev = ctx;
    *head = ctx;
    (*activeClients)++;
    stats.totalConnections++;
    return 1;

fail:
    if (clientFd != -1) close(clientFd);
    if (tfd != -1) close(tfd);
    if (ctx) client_pool_release(ctx);
    return 0;
}

static void accept_connections(ServerCtx sctx, ClientCtx **head, int *activeClients) {
    while (1) {
        int clientFd = accept(sctx.serverFd, NULL, NULL);
        if (clientFd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept failed");
            break;
        }

        if (*activeClients >= MAX_CLIENTS) {
            fprintf(stderr, "max clients (%d) reached, dropping\n", MAX_CLIENTS);
            close(clientFd);
            continue;
        }

        if (!setup_client(sctx, clientFd, head, activeClients)) {
            /* clientFd already closed inside setup_client's fail path */
        }
    }
}

static void handle_timerEvent(int epollFd, ClientCtx *ctx, ClientCtx **head, int *activeClients) {
    // consume the expiration counter so the fd does not re-fire immediately
    uint64_t expirations;
    if (read(ctx->timerEv.fd, &expirations, sizeof(expirations)) == -1 && errno != EAGAIN)
        perror("timerfd read failed");

    close_client(epollFd, ctx, head, activeClients);
}

static int handle_socket_event(int epollFd, ClientCtx *ctx, Hash_Table *db,
                                Hash_Table *rateLimitTable, ClientCtx **head, int *activeClients) {

    char   responseBuffer[RESPONSE_BUFFER_SIZE] = {0};
    size_t totalRead = 0;
    int    keepAlive = 0;

    // zero the buffer so stale bytes from a previous keep-alive request
    // cannot bleed into the new parse if this request is shorter
    memset(ctx->buffer, 0, BUFFER_SIZE);

    // get client IP
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    char ip[INET_ADDRSTRLEN] = {0};
    if (getpeername(ctx->sockEv.fd, (struct sockaddr *)&peer, &peerlen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    // check rate limit
    if (DEBUG_RATE_LIMIT && !rate_limit_check(&rateLimitTable, ip)) {
        send_response(ctx->sockEv.fd, 429, "Too Many Requests\n", 0);
        close_client(epollFd, ctx, head, activeClients);
        return 0;
    }

    //get bytes from client (until EAGAIN)
    while (totalRead < BUFFER_SIZE - 1) {
        ssize_t nBytes = read(ctx->sockEv.fd, ctx->buffer + totalRead, BUFFER_SIZE - 1 - totalRead);

        if (nBytes > 0) {
            totalRead += (size_t)nBytes;
            if (memmem(ctx->buffer, totalRead, "\r\n\r\n", 4)) break;
        } else if (nBytes == 0) {
            // peer closed the connection
            goto close_connection;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (totalRead == 0) { 
                    if(reset_timer(ctx) == -1) goto close_connection; 
                    return 1; 
                }
                break;
            }
            perror("read failed");
            goto close_connection;
        }
    }

    //handle request and response
    ctx->buffer[totalRead] = '\0';
    int statusCode = handle_request(db, ctx->buffer, responseBuffer, &keepAlive);
    stats.totalRequests++;
    send_response(ctx->sockEv.fd, statusCode, responseBuffer, keepAlive);

    if (keepAlive) {
        if(reset_timer(ctx) == -1) goto close_connection; 
        return 1;
    }

    close_connection:
        close_client(epollFd, ctx, head, activeClients);
        return 0;
}

static ServerCtx start_server(int port) {
    ServerCtx ctx;
    struct sockaddr_in address;

    ctx.serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx.serverFd == -1) { perror("socket failed"); exit(EXIT_FAILURE); }

    int opt = 1;
    if (setsockopt(ctx.serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed"); exit(EXIT_FAILURE);
    }

    if (set_nonblocking(ctx.serverFd) == -1) { 
        perror("set_nonblocking failed");
        exit(EXIT_FAILURE); 
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(ctx.serverFd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed"); exit(EXIT_FAILURE);
    }
    if (listen(ctx.serverFd, LISTEN_BACKLOG) < 0) { perror("listen failed"); exit(EXIT_FAILURE); }

    ctx.epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epollFd == -1) { perror("epoll_create1 failed"); exit(EXIT_FAILURE); }

    // server fd uses data.ptr = NULL as sentinel ,distinguishable from any
    // valid ConnectionEvent pointer in the event loop
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = NULL };
    if (epoll_ctl(ctx.epollFd, EPOLL_CTL_ADD, ctx.serverFd, &ev) == -1) {
        perror("epoll_ctl serverFd failed"); exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d (epoll, max %d clients)...\n", port, MAX_CLIENTS);
    return ctx;
}

void send_response(int socketFd, int statusCode, char *responseMsg, int keepAlive) {
    char fullResponse[256 + RESPONSE_BUFFER_SIZE];
    const char* extraHeader = "";
    char *statusMsg = "OK";
    size_t bodyLen;

    // gzip special status code
    if (statusCode >= 10000) {
        bodyLen = statusCode - 10000; //unpack file size
        statusCode = 200;
        extraHeader = "Content-Encoding: gzip\r\n";
    } else {
        //http standard status code
        bodyLen = strlen(responseMsg);
    }

    switch (statusCode) {
        case 200: statusMsg = "OK"; break;
        case 400: statusMsg = "Bad Request"; break;
        case 404: statusMsg = "Not Found"; break;
        default:  statusMsg = "Internal Server Error"; break;
    }

    // make header
    int headerLen = snprintf(fullResponse, sizeof(fullResponse),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n\r\n",
        statusCode, statusMsg,
        (statusCode == 200 && extraHeader[0] == '\0' && responseMsg[0] == '{') ? "application/json" : "text/html",
        extraHeader, bodyLen,
        keepAlive ? "keep-alive" : "close");

    // send header and then message
    write(socketFd, fullResponse, headerLen);
    
    size_t total = 0;
    while (total < bodyLen) {
        ssize_t n = write(socketFd, responseMsg + total, bodyLen - total);
        if (n <= 0) break; 
        total += n;
    }
}

static void check_snapshot(Hash_Table *db, const char *path, ServerCtx sctx) {
    if (!path) return;
    time_t now = time(NULL);
    if ((now - stats.lastSnapshotTime) >= 300 && stats.keysModifiedSinceSnapshot >= 100) {

        pid_t pid = fork();
        //son process manage the snapshot 
        if (pid == 0) {
            //close the fds, otherwise they allow listening by son process
            close(sctx.serverFd);
            close(sctx.epollFd);
            
            ht_snapshot(db, path); //Note: ht_snapshot hold an unecessary rdlock
            exit(0);
        } else if (pid > 0) {

            unsigned long modified = stats.keysModifiedSinceSnapshot;
            stats.keysModifiedSinceSnapshot = 0;
            stats.lastSnapshotTime = now;
            printf("Snapshot started in child process %d (%lu keys modified)\n", pid, modified);
        } else {
            perror("fork snapshot failed");
            stats.lastSnapshotTime = now;
        }
    }
}


static int rate_limit_check(Hash_Table **rateLimitTablePtr, const char *ip) {
    if (!ip || ip[0] == '\0') return 1;
    Hash_Table *table = *rateLimitTablePtr;

    //check about table max size
    if (table->size > 10000) {
        // clean and recreate
        ht_destroy(table, NULL);
        table = ht_create(1024, hash_key);
        if (!table) return 1;
        *rateLimitTablePtr = table;
        
        if (DEBUG_RATE_LIMIT) printf("Rate limit reached\n");

    }

    RateEntry entry = {0};
    time_t now = time(NULL);

    ht_get(table, (void*)ip, strlen(ip) + 1, &entry, sizeof(entry));

    // slide counters
    if (now - entry.windowStartTime >= 1) {
        entry.countPrev = entry.countCurr;
        entry.countCurr = 0;
        entry.windowStartTime = now;
    }

    // compute weighted average 
    double elapsed = difftime(now, entry.windowStartTime);
    double estimated = entry.countPrev * (1.0 - elapsed) + entry.countCurr;

    // block
    if (estimated >= RATE_LIMIT_RPS) {
        ht_set(table, (void*)ip, strlen(ip) + 1, &entry, sizeof(entry));
        return 0;
    }

    // save
    entry.countCurr++;
    ht_set(table, (void*)ip, strlen(ip) + 1, &entry, sizeof(entry));
    return 1;
}

static inline void set_snapshot_timer(int epollFd,int *snap_tfd,int *snapshot_tfd_sentinel){
    //set snapshot timer event
    *snap_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec snap_ts = { .it_interval = {60, 0}, .it_value = {60, 0} }; //check every minute
    timerfd_settime(*snap_tfd, 0, &snap_ts, NULL);

    struct epoll_event snap_ev = { .events = EPOLLIN, .data.ptr = snapshot_tfd_sentinel };
    epoll_ctl(epollFd, EPOLL_CTL_ADD, *snap_tfd, &snap_ev);
}

void server_loop(ServerCtx sctx, Hash_Table *db, Hash_Table *rateLimitTable, const char *snapFilePath) {
    struct epoll_event events[MAX_EVENTS];
    ClientCtx *head = NULL;
    int activeClients = 0;
    stats.activeClientsPtr = &activeClients;

    static int snapshot_tfd_sentinel = 0; 
    int snap_tfd = -1;
    if(snapFilePath) set_snapshot_timer(sctx.epollFd,&snap_tfd,&snapshot_tfd_sentinel);

    while (keepRunning) {
        int numEventReady = epoll_wait(sctx.epollFd, events, MAX_EVENTS, -1);
        if (numEventReady == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < numEventReady; i++) {
            ConnectionEvent *ev = (ConnectionEvent *)events[i].data.ptr;

            // NULL sentinel for server event
            if (ev == NULL) {
                accept_connections(sctx, &head, &activeClients);
                continue;
            }

            //snapshot timer check
            if (ev == (ConnectionEvent *)&snapshot_tfd_sentinel) {
                uint64_t exp;
                read(snap_tfd, &exp, sizeof(exp)); 
                check_snapshot(db, snapFilePath,sctx);
                continue;
            }

            ClientCtx *ctx = ev->parent;

            /* closing=1 is set before client_pool_release() to guard against
            * epoll_wait() returning stale events for the same ClientCtx in one
            * batch: if a TYPE_TIMER releases ctx and a TYPE_SOCKET follows in the
            * same loop iteration, the socket event is discarded before touching
            * any field of the already-freed ctx. */
            if (ctx->closing) continue; 

            if (ev->type == TYPE_TIMER) {
                ctx->closing = 1;
                handle_timerEvent(sctx.epollFd, ctx, &head, &activeClients);
                continue;
            }

            handle_socket_event(sctx.epollFd, ctx, db, rateLimitTable, &head, &activeClients);
        }
    }

    // shutdown: walk the live-client list and close every connection
    while (head)
        close_client(sctx.epollFd, head, &head, &activeClients);

    if (snap_tfd != -1) close(snap_tfd);
    close(sctx.epollFd);
    close(sctx.serverFd);
}


int main(int argc, char **argv) {
    stats.startTime         = time(NULL);
    stats.lastSnapshotTime = time(NULL);
    int idxLoad, idxSave;
    analyze_args(argc, argv, &idxLoad, &idxSave);
    config_signal_context();

    if (client_pool_init() == -1) {
        fprintf(stderr, "Critical: Could not initialize client pool\n");
        return EXIT_FAILURE;
    }

    Hash_Table *db       = ht_create(16384, hash_key);
    Hash_Table *rateLimitTable = ht_create(1024, hash_key); 

    if (!db || !rateLimitTable) {
        fprintf(stderr, "Critical: Could not initialize hash tables\n");
        return EXIT_FAILURE;
    }

    if (idxLoad != -1 && ht_load(db, argv[idxLoad]))
        printf("Table loaded from %s\n", argv[idxLoad]);
    else
        printf("Starting with empty table\n");

    server_loop(start_server(PORT), db, rateLimitTable, (idxSave != -1) ? argv[idxSave] : NULL);

    //clean
    client_pool_destroy();
    ht_destroy(db, (idxSave != -1) ? argv[idxSave] : NULL);
    ht_destroy(rateLimitTable, NULL); 
    return 0;
}