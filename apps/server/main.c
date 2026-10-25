/* Development-only CHttp/CNet WebSocket smoke server.
 * Binds loopback and accepts no game commands or authentication.
 * Production gameplay must use an authenticated, owner-affine command pipeline.
 */
#include <http_server/http.h>

#include <stdio.h>
#include <string.h>

static int health(void *user, const chttp_server_request_view *request,
                  chttp_server_response *response) {
    static const char body[] = "{\"status\":\"ok\",\"protocol\":1,\"mode\":\"development\"}";
    (void)user;
    (void)request;
    return chttp_server_reply(response, 200u, "application/json", body, sizeof(body) - 1u);
}

static int ws_open(void *user, chttp_websocket *peer,
                   const chttp_server_request_view *request,
                   chttp_server_response *response) {
    static const char welcome[] =
        "{\"v\":1,\"type\":\"server.hello\",\"capabilities\":[\"ping\"]}";
    (void)user;
    (void)request;
    (void)response;
    return chttp_websocket_send_text(peer, welcome, sizeof(welcome) - 1u);
}

static void ws_event(void *user, chttp_websocket *peer,
                     const chttp_websocket_event *event) {
    static const char ping[] = "{\"v\":1,\"type\":\"client.ping\"}";
    static const char pong[] = "{\"v\":1,\"type\":\"server.pong\"}";
    static const char unsupported[] =
        "{\"v\":1,\"type\":\"server.error\",\"code\":\"unsupported_command\"}";
    (void)user;
    if (!event || event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
        event->message_type != CHTTP_WEBSOCKET_MESSAGE_TEXT)
        return;
    if (event->size == sizeof(ping) - 1u && event->data &&
        memcmp(event->data, ping, sizeof(ping) - 1u) == 0) {
        (void)chttp_websocket_send_text(peer, pong, sizeof(pong) - 1u);
    } else {
        (void)chttp_websocket_send_text(peer, unsupported, sizeof(unsupported) - 1u);
    }
}

static native_io_backend_kind platform_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config development_config(void) {
    chttp_server_config config = {0};
    config.host = "127.0.0.1"; /* never expose unauthenticated dev endpoint */
    config.port = 1780u;
    config.backlog = 16u;
    config.network.backend = platform_backend();
    config.network.connection_capacity = 32u;
    config.network.command_capacity = 128u;
    config.network.request_capacity = 32u;
    config.network.completion_batch_capacity = 16u;
    config.network.event_capacity = 128u;
    config.network.max_send_bytes = 64u * 1024u;
    config.network.receive_buffer_bytes = 4096u;
    config.network.connect_timeout_ms = 5000u;
    config.network.read_timeout_ms = 5000u;
    config.network.write_timeout_ms = 5000u;
    config.route_capacity = 4u;
    config.middleware_capacity = 4u;
    config.max_route_middleware_count = 4u;
    config.max_route_param_count = 2u;
    config.max_route_param_bytes = 128u;
    config.max_target_bytes = 256u;
    config.max_header_count = 16u;
    config.max_header_bytes = 4096u;
    config.max_request_body_bytes = 1024u;
    config.max_response_header_count = 16u;
    config.max_response_header_bytes = 1024u;
    config.max_response_body_bytes = 1024u;
    config.max_buffered_response_body_bytes = 1024u;
    config.poll_slice_ms = 2u;
    return config;
}

int main(void) {
    chttp_server server = {0};
    chttp_server_config config = development_config();
    int status = chttp_server_init(&server, &config);
    if (status != SALTS_OK) {
        fprintf(stderr, "CHttp init failed: %d\n", status);
        return 1;
    }
    status = chttp_server_get(&server, "/health", health, NULL);
    if (status == SALTS_OK)
        status = chttp_server_websocket(&server, "/ws", ws_open, ws_event, NULL);
    if (status == SALTS_OK) status = chttp_server_start(&server);
    if (status != SALTS_OK) {
        fprintf(stderr, "CHttp setup/start failed: %d\n", status);
        (void)chttp_server_destroy(&server);
        return 1;
    }
    puts("MyGames dev-only gateway listening at http://127.0.0.1:1780");
    puts("Press Enter to stop.");
    (void)getchar();
    status = chttp_server_stop(&server, 5000u);
    if (status != SALTS_OK) {
        fprintf(stderr, "CHttp stop failed (owner not destroyed): %d\n", status);
        return 1;
    }
    status = chttp_server_destroy(&server);
    return status == SALTS_OK ? 0 : 1;
}
