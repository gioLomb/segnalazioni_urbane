/**
 * @file connection_handler.h
 * @brief Networking layer: connection setup, I/O callbacks, response dispatch.
 *
 * Exposes the four libuv callbacks and the close helper needed by server.c:
 *
 *   on_connection    — accept callback passed to uv_listen(); allocates
 *                      ClientCtx, inits handles, arms the keepalive timer.
 *   on_buffer_alloc  — buffer allocation callback; lazily allocates the
 *                      receive buffer and returns the unused tail.
 *   on_read          — read callback; accumulates bytes, parses HTTP,
 *                      dispatches via handle_request(), sends the response.
 *   close_client     — initiates graceful teardown of TCP + timer handles;
 *                      called from server.c during shutdown.
 */

#ifndef CONNECTION_HANDLER_H
#define CONNECTION_HANDLER_H

#include "config.h"
#include "client_manager.h"
#include "http_utils.h"

/**
 * @brief libuv connection callback — accept, init and register a new client.
 *
 * Passed directly to uv_listen() in server.c. Allocates a ClientCtx from
 * the slab pool, initialises the TCP and timer handles, accepts the socket,
 * enables TCP_NODELAY, arms the keepalive timer, and starts reading via
 * on_buffer_alloc / on_read.
 *
 * @param server The listening TCP server handle.
 * @param status 0 on success; negative libuv error code on failure.
 */
void on_connection(uv_stream_t *server, int status);

/**
 * @brief Initiates a graceful close of both the TCP handle and keepalive timer.
 *
 * Guarded by ctx->closing to prevent double-close. Memory is released inside
 * the on_close callback once both uv_close() calls have fired.
 *
 * @param ctx Client context to close.
 */
void close_client(ClientCtx *ctx);

/**
 * @brief libuv buffer allocation callback.
 *
 * Lazily allocates ctx->buffer on the first call; subsequent calls return
 * the unused tail of the existing buffer. Sets buf->base = NULL when the
 * buffer is full, which triggers UV_ENOBUFS in on_read and closes the connection.
 *
 * @param handle    TCP handle whose data field points to the owning ClientCtx.
 * @param suggested Suggested buffer size (ignored — fixed BUFFER_SIZE is used).
 * @param buf       Output descriptor to fill.
 */
void on_buffer_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);

/**
 * @brief libuv read callback — accumulate, parse, dispatch, respond.
 *
 * Accumulates raw bytes in ctx->buffer until http_is_request_complete()
 * returns true, then parses the HTTP request, optionally checks the rate
 * limit, dispatches through handle_request(), and sends the response.
 * Closes the connection on network error, parse failure or rate-limit rejection.
 *
 * @param stream TCP stream whose data field points to the owning ClientCtx.
 * @param nread  Bytes read; negative signals EOF or network error.
 * @param buf    Buffer descriptor filled by on_buffer_alloc (informational only).
 */
void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

#endif /* CONNECTION_HANDLER_H */