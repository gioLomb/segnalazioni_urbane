/**
 * @file server.c
 * @brief Server orchestrator.
 *
 * File layout
 * ───────────
 *   hash_key()          — shared hash function for all Hash_Table instances
 *   server_bind()       — TCP bind + listen + SIGINT arm
 *   server_shutdown()   — drain callbacks and destroy all subsystems
 *   init_*()            — per-subsystem startup helpers
 *   main()              — entry point
 *
 */

#include "server.h"
#include "rate_limiter.h"
#include "connection_handler.h"
#include "template.h"
#include "user.h"
#include "report.h"
#include "geo.h"
#include "session.h"
#include "db.h"
#include "client_manager.h"

#include <stdio.h>
#include <assert.h>


// MurmurHash-inspired function used by every Hash_Table instance in the
// application. Defined here and declared
// in server.h so all modules share a single implementation.
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



static void on_signal(uv_signal_t *handle, int signum) {
    (void)signum;
    // Stop the signal watcher and request the event loop to exit cleanly.
    uv_signal_stop(handle);
    uv_stop(uv_default_loop());
}

// Initialises the TCP server handle, binds to PORT, starts listening,
// and arms the SIGINT signal handler.
// Returns 0 on success, -1 on listen error.
static int server_bind(uv_loop_t *loop, uv_tcp_t *server, uv_signal_t *sig) {
    struct sockaddr_in addr;
    uv_tcp_init(loop, server);
    uv_ip4_addr("0.0.0.0", PORT, &addr);
    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);

    int r = uv_listen((uv_stream_t *)server, LISTEN_BACKLOG, on_connection);
    if (r) {
        fprintf(stderr, "uv_listen error: %s\n", uv_strerror(r));
        return -1;
    }

    uv_signal_init(loop, sig);
    uv_signal_start(sig, on_signal, SIGINT);
    return 0;
}


// Shutdown sequence (reverse of init order):
//   close all clients , close server and signal handles ,
//   drain close callbacks , destroy subsystems
static void server_shutdown(uv_loop_t *loop, uv_tcp_t *server, uv_signal_t *sig) {
    client_manager_close_all(close_client);
    uv_close((uv_handle_t *)sig,    NULL);
    uv_close((uv_handle_t *)server, NULL);

    // Final uv_run drains the close callbacks queued by close_all above.
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);

    // Destroy subsystems in reverse initialisation order.
    tpl_unload_all();
    session_destroy_all();
    geo_cleanup();
    rate_limiter_destroy();
    client_manager_destroy();
    db_close();

    printf("Shutdown complete.\n");
}

/* ── Subsystem init helpers ──────────────────────────────────────────── */

static int init_db(void) {
    if (db_init(APP_DB_PATH) != 0) {
        fprintf(stderr, "Fatal: cannot open database '%s': %s\n",
                APP_DB_PATH, db_errmsg());
        return -1;
    }
    if (user_setup_table() != 0) {
        fprintf(stderr, "Fatal: user_setup_table\n");
        return -1;
    }
    if (report_setup_table() != 0) {
        fprintf(stderr, "Fatal: report_setup_table\n");
        return -1;
    }
    printf("Database: %s\n", APP_DB_PATH);
    return 0;
}

static int init_templates(void) {
    if (tpl_load_files(TPL_LOGIN,
                       TPL_REGISTER,
                       TPL_CITIZEN_HOME,
                       TPL_OPERATOR_MAP,
                       TPL_SUBMIT,
                       TPL_CSS,
                       TPL_ADMIN_MAP,
                       NULL) != 0) {
        fprintf(stderr, "Fatal: failed to load core templates\n");
        return -1;
    }
    return 0;
}

static int init_geo_table(void) {
    if (geo_init(GEO_JSON_PATH, CITIES_JSON_PATH) < 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", GEO_JSON_PATH);
        return -1;
    }
    // Load the city-name JSON produced by geo_init into the template cache
    // so it can be served directly via route_api_cities.
    if (tpl_load_files(CITIES_JSON_PATH, NULL) != 0) {
        fprintf(stderr, "Fatal: failed to load '%s'\n", CITIES_JSON_PATH);
        return -1;
    }
    return 0;
}

static int init_sessions(void) {
    return session_init();
}

/* ── main ────────────────────────────────────────────────────────────── */

/**
 * Server startup sequence:
 *
 *  Ignore SIGPIPE so that a broken client pipe cannot kill the process.
 *  Initialise every subsystem — a single assert() provides fail-fast
 *  semantics; each helper prints its own diagnostic on failure.
 *  Bind the TCP socket and arm the SIGINT handler.
 *  Enter the event loop (blocks until SIGINT).
 *  Graceful shutdown: drain callbacks and destroy all state.
 */
int main(void) {
    uv_loop_t  *loop;
    uv_tcp_t    server;
    uv_signal_t sig;

    // Suppress SIGPIPE process-wide: libuv may trigger it on concurrent closes.
    signal(SIGPIPE, SIG_IGN);

    // Fail-fast subsystem initialisation; the assert string is printed by
    // the C runtime if any operand evaluates to zero or NULL.
    assert( client_manager_init() == 0 &&
            init_db()             == 0 &&
            init_templates()      == 0 &&
            init_geo_table()      == 0 &&
            init_sessions()       == 0 &&
            rate_limiter_init() == 0
            && "Fatal: startup failed — see stderr for details" );

    loop = uv_default_loop();
    assert(server_bind(loop, &server, &sig) == 0 && "Fatal: server_bind");

    printf("SegnalaCity listening on port %d\n", PORT);
    uv_run(loop, UV_RUN_DEFAULT);

    server_shutdown(loop, &server, &sig);
    return EXIT_SUCCESS;
}