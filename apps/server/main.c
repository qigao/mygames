/* Development-only room gateway. Requires CHttp + CNet installed SDK.
 * Unauthenticated, loopback-only. All WS callbacks use owner_count=1.
 * Never publish this handler to untrusted clients.
 */
#include <http_server/http.h>
#include "mygames/room_directory.h"
#include "mygames/wire.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MYGAMES_PEER_CAPACITY 32u
#define MYGAMES_DEDUP_WINDOW 8u

typedef struct response_cache {
    bool used;
    char request_id[MYGAMES_REQUEST_ID_CAPACITY];
    unsigned char request[MYGAMES_WIRE_MAX_BYTES];
    size_t request_bytes;
    char response[256];
} response_cache;

typedef struct client_peer {
    bool occupied;
    mygames_player_id player;
    chttp_server_websocket_session session;
    response_cache cache[MYGAMES_DEDUP_WINDOW];
    unsigned int next_cache;
} client_peer;

typedef struct dev_server_state {
    mygames_room_directory rooms;
    client_peer peers[MYGAMES_PEER_CAPACITY];
    uint64_t next_player;
} dev_server_state;

static bool append(char *buffer, size_t capacity, size_t *length,
                   const char *format, ...) {
    int count;
    va_list args;
    if (*length >= capacity) return false;
    va_start(args, format);
    count = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *length) return false;
    *length += (size_t)count;
    return true;
}

static bool session_equal(const chttp_server_websocket_session *a,
                          const chttp_server_websocket_session *b) {
    return a->impl == b->impl &&
           a->connection_slot == b->connection_slot &&
           a->connection_generation == b->connection_generation &&
           a->stream_id == b->stream_id;
}

static client_peer *find_peer(dev_server_state *state,
                              const chttp_server_websocket_session *session) {
    unsigned int i;
    for (i = 0; i < MYGAMES_PEER_CAPACITY; ++i)
        if (state->peers[i].occupied &&
            session_equal(&state->peers[i].session, session)) return &state->peers[i];
    return NULL;
}

static response_cache *cached_request(client_peer *peer,
                                      const char *request_id) {
    unsigned int i;
    for (i = 0; i < MYGAMES_DEDUP_WINDOW; ++i) {
        response_cache *item = &peer->cache[i];
        if (item->used && strcmp(item->request_id, request_id) == 0) return item;
    }
    return NULL;
}

static void remember_request(client_peer *peer, const mygames_wire_command *command,
                             const void *bytes, size_t size, const char *response) {
    response_cache *item = &peer->cache[peer->next_cache % MYGAMES_DEDUP_WINDOW];
    peer->next_cache++;
    memset(item, 0, sizeof(*item));
    memcpy(item->request_id, command->request_id, strlen(command->request_id) + 1u);
    memcpy(item->request, bytes, size);
    item->request_bytes = size;
    memcpy(item->response, response, strlen(response) + 1u);
    item->used = true;
}

static int send_text(chttp_websocket *socket, const char *text) {
    return chttp_websocket_send_text(socket, text, strlen(text));
}

static int health(void *user, const chttp_server_request_view *request,
                  chttp_server_response *response) {
    static const char body[] = "{\"status\":\"ok\",\"protocol\":1,\"mode\":\"development\"}";
    (void)user;
    (void)request;
    return chttp_server_reply(response, 200u, "application/json", body, sizeof(body) - 1u);
}

static bool encode_snapshot(const mygames_room *room, char *out, size_t capacity) {
    size_t n = 0;
    unsigned int i;
    if (!room || !out || capacity == 0) return false;
    if (!append(out, capacity, &n,
                "{\"v\":1,\"type\":\"room.snapshot\",\"room_id\":\"%s\","
                "\"room_epoch\":1,\"revision\":%" PRIu64 ",\"phase\":\"%s\","
                "\"owner_id\":\"%" PRIu64 "\",\"seats\":[",
                room->id, room->revision,
                room->phase == MYGAMES_ROOM_WAITING ? "waiting" : "starting",
                room->owner)) return false;
    for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i) {
        const mygames_room_seat *seat = &room->seats[i];
        if (i && !append(out, capacity, &n, ",")) return false;
        if (!seat->player) {
            if (!append(out, capacity, &n, "null")) return false;
        } else if (!append(out, capacity, &n,
                           "{\"player_id\":\"%" PRIu64 "\",\"ready\":%s}",
                           seat->player, seat->ready ? "true" : "false")) {
            return false;
        }
    }
    return append(out, capacity, &n, "]}");
}

/* A failed delivery is not a mutation rollback. Client may request a current
 * snapshot, but server-side queue pressure must be visible in diagnostics. */
static void publish_snapshot(dev_server_state *state, const mygames_room *room) {
    char json[1024];
    unsigned int i;
    if (!room || !encode_snapshot(room, json, sizeof(json))) return;
    for (i = 0; i < MYGAMES_PEER_CAPACITY; ++i) {
        client_peer *p = &state->peers[i];
        const mygames_room *member_room;
        if (!p->occupied) continue;
        member_room = mygames_room_directory_get_player(&state->rooms, p->player);
        if (member_room == room) {
            const int rc = chttp_server_websocket_send_text(&p->session, json, strlen(json));
            if (rc != SALTS_OK)
                fprintf(stderr, "room snapshot queue admission failed: %d\n", rc);
        }
    }
}

static const char *status_string(mygames_room_status status) {
    switch (status) {
    case MYGAMES_ROOM_INVALID_ARGUMENT: return "invalid_argument";
    case MYGAMES_ROOM_ALREADY_PRESENT: return "already_present";
    case MYGAMES_ROOM_FULL: return "full";
    case MYGAMES_ROOM_NOT_FOUND: return "not_found";
    case MYGAMES_ROOM_WRONG_PHASE: return "wrong_phase";
    case MYGAMES_ROOM_NOT_OWNER: return "not_owner";
    case MYGAMES_ROOM_NOT_READY: return "not_ready";
    case MYGAMES_ROOM_IN_OTHER_ROOM: return "in_other_room";
    case MYGAMES_ROOM_OK: return "ok";
    default: return "internal_failure";
    }
}

static bool write_error(char *out, size_t cap, const char *request_id,
                        const char *code) {
    const int count = snprintf(out, cap,
        "{\"v\":1,\"type\":\"room.error\",\"request_id\":\"%s\",\"code\":\"%s\"}",
        request_id, code);
    return count > 0 && (size_t)count < cap;
}

static void ws_handle_command(dev_server_state *state, client_peer *client,
                              chttp_websocket *socket, const void *bytes, size_t size) {
    mygames_wire_command command;
    mygames_wire_status parse_result;
    response_cache *previous;
    const mygames_room *room = NULL;
    mygames_room_status result = MYGAMES_ROOM_OK;
    char reply[256];
    char room_id[MYGAMES_ROOM_ID_CAPACITY] = {0};
    unsigned int seat = 0u;
    uint64_t revision = 0u;
    bool changed = false;
    parse_result = mygames_wire_decode_command(bytes, size, &command);
    if (parse_result != MYGAMES_WIRE_OK) {
        const char *code = parse_result == MYGAMES_WIRE_TOO_LARGE ? "oversized_message" :
                           parse_result == MYGAMES_WIRE_UNSUPPORTED_VERSION ? "unsupported_version" :
                           parse_result == MYGAMES_WIRE_UNSUPPORTED_TYPE ? "unsupported_command" :
                           "invalid_command";
        char error[128];
        const int n = snprintf(error, sizeof(error),
            "{\"v\":1,\"type\":\"server.error\",\"code\":\"%s\"}", code);
        if (n > 0 && (size_t)n < sizeof(error)) (void)send_text(socket, error);
        return;
    }
    if (command.kind == MYGAMES_COMMAND_PING) {
        (void)send_text(socket, "{\"v\":1,\"type\":\"server.pong\"}");
        return;
    }
    previous = cached_request(client, command.request_id);
    if (previous) {
        if (size == previous->request_bytes &&
            memcmp(bytes, previous->request, size) == 0) {
            (void)send_text(socket, previous->response);
        } else {
            (void)send_text(socket,
                "{\"v\":1,\"type\":\"server.error\",\"code\":\"request_id_conflict\"}");
        }
        return;
    }
    room = mygames_room_directory_get_player(&state->rooms, client->player);
    if (command.kind == MYGAMES_COMMAND_CREATE_ROOM) {
        result = mygames_room_directory_create(&state->rooms, client->player, &room);
        changed = result == MYGAMES_ROOM_OK;
    } else if (command.kind == MYGAMES_COMMAND_JOIN_ROOM) {
        result = mygames_room_directory_join(&state->rooms, command.room_id,
                                              client->player, &seat);
        if (result == MYGAMES_ROOM_OK || result == MYGAMES_ROOM_ALREADY_PRESENT)
            room = mygames_room_directory_get_player(&state->rooms, client->player);
        changed = result == MYGAMES_ROOM_OK;
    } else if (command.kind == MYGAMES_COMMAND_READY) {
        result = mygames_room_directory_ready(&state->rooms, client->player, command.ready);
        changed = result == MYGAMES_ROOM_OK;
        room = mygames_room_directory_get_player(&state->rooms, client->player);
    } else if (command.kind == MYGAMES_COMMAND_LEAVE_ROOM) {
        if (room) memcpy(room_id, room->id, strlen(room->id) + 1u);
        result = mygames_room_directory_leave(&state->rooms, client->player);
        changed = result == MYGAMES_ROOM_OK;
        room = mygames_room_directory_get(&state->rooms, room_id);
        seat = MYGAMES_ROOM_SEATS;
    } else if (command.kind == MYGAMES_COMMAND_GET_SNAPSHOT) {
        result = room ? MYGAMES_ROOM_OK : MYGAMES_ROOM_NOT_FOUND;
    } else {
        result = MYGAMES_ROOM_INVALID_ARGUMENT;
    }
    if (result != MYGAMES_ROOM_OK && result != MYGAMES_ROOM_ALREADY_PRESENT) {
        if (!write_error(reply, sizeof(reply), command.request_id, status_string(result)))
            return;
        remember_request(client, &command, bytes, size, reply);
        (void)send_text(socket, reply);
        return;
    }
    if (room) {
        revision = room->revision;
        if (command.kind != MYGAMES_COMMAND_LEAVE_ROOM) {
            unsigned int current_seat;
            if (mygames_room_find_seat(room, client->player, &current_seat) == MYGAMES_ROOM_OK)
                seat = current_seat;
        }
        memcpy(room_id, room->id, strlen(room->id) + 1u);
    }
    {
        const int n = snprintf(reply, sizeof(reply),
            "{\"v\":1,\"type\":\"room.ack\",\"request_id\":\"%s\","
            "\"room_id\":\"%s\",\"revision\":%" PRIu64 ",\"seat\":%u}",
            command.request_id, room_id, revision, seat);
        if (n <= 0 || (size_t)n >= sizeof(reply)) return;
    }
    remember_request(client, &command, bytes, size, reply);
    (void)send_text(socket, reply);
    if (changed) {
        if (room) publish_snapshot(state, room);
        /* If the last participant leaves the room, the room is gone and no
         * subscribers remain. */
    } else if (room && command.kind == MYGAMES_COMMAND_GET_SNAPSHOT) {
        char snapshot[1024];
        if (encode_snapshot(room, snapshot, sizeof(snapshot)))
            (void)send_text(socket, snapshot);
    }
}

static int ws_open(void *user, chttp_websocket *socket,
                   const chttp_server_request_view *request,
                   chttp_server_response *response) {
    dev_server_state *state = (dev_server_state *)user;
    chttp_server_websocket_session session = {0};
    char hello[192];
    unsigned int i;
    int rc;
    (void)request;
    (void)response;
    if (!state || state->next_player == UINT64_MAX) return SALTS_ENOBUFS;
    rc = chttp_server_websocket_session_capture(socket, &session);
    if (rc != SALTS_OK) return rc;
    for (i = 0u; i < MYGAMES_PEER_CAPACITY; ++i) {
        client_peer *p = &state->peers[i];
        if (p->occupied) continue;
        memset(p, 0, sizeof(*p));
        p->occupied = true;
        p->player = ++state->next_player;
        p->session = session;
        {
            int n = snprintf(hello, sizeof(hello),
                "{\"v\":1,\"type\":\"server.hello\","
                "\"capabilities\":[\"ping\",\"rooms\"],\"player_id\":\"%" PRIu64 "\"}",
                p->player);
            if (n <= 0 || (size_t)n >= sizeof(hello)) {
                memset(p, 0, sizeof(*p));
                return SALTS_EINVAL;
            }
        }
        rc = send_text(socket, hello);
        if (rc != SALTS_OK) memset(p, 0, sizeof(*p));
        return rc;
    }
    return SALTS_ENOBUFS;
}

static void ws_event(void *user, chttp_websocket *socket,
                     const chttp_websocket_event *event) {
    dev_server_state *state = (dev_server_state *)user;
    chttp_server_websocket_session session = {0};
    client_peer *peer;
    if (!state || !event) return;
    if (chttp_server_websocket_session_capture(socket, &session) != SALTS_OK)
        return;
    peer = find_peer(state, &session);
    if (!peer) return;
    if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
        const mygames_room *old_room =
            mygames_room_directory_get_player(&state->rooms, peer->player);
        char room_id[MYGAMES_ROOM_ID_CAPACITY] = {0};
        if (old_room && old_room->phase == MYGAMES_ROOM_WAITING) {
            memcpy(room_id, old_room->id, strlen(old_room->id) + 1u);
            if (mygames_room_directory_leave(&state->rooms, peer->player) == MYGAMES_ROOM_OK)
                publish_snapshot(state, mygames_room_directory_get(&state->rooms, room_id));
        }
        memset(peer, 0, sizeof(*peer));
        return;
    }
    if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
        if (event->message_type != CHTTP_WEBSOCKET_MESSAGE_TEXT) {
            (void)chttp_websocket_close(socket, 1003u, "text_only", 9u);
            return;
        }
        ws_handle_command(state, peer, socket, event->data, event->size);
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
    config.host = "127.0.0.1";
    config.port = 1780u;
    config.backlog = 16u;
    config.network.backend = platform_backend();
    config.network.connection_capacity = MYGAMES_PEER_CAPACITY;
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
    chttp_server_execution_options owner = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_server_websocket_options ws = {0};
    dev_server_state state = {0};
    int rc;
    mygames_room_directory_init(&state.rooms);
    rc = chttp_server_init(&server, &config);
    if (rc != SALTS_OK) {
        fprintf(stderr, "CHttp init failed: %d\n", rc);
        return 1;
    }
    owner.owner_count = 1u; /* serializes room mutations on one CNet Owner */
    rc = chttp_server_set_execution_options(&server, &owner);
    if (rc == SALTS_OK) rc = chttp_server_get(&server, "/health", health, &state);
    ws.size = sizeof(ws);
    ws.path = "/ws";
    ws.max_frame_bytes = 1024u;
    ws.max_message_bytes = 1024u;
    ws.max_buffered_input_bytes = 2048u;
    ws.on_open = ws_open;
    ws.on_event = ws_event;
    ws.user = &state;
    if (rc == SALTS_OK) rc = chttp_server_websocket_with(&server, &ws);
    if (rc == SALTS_OK) rc = chttp_server_start(&server);
    if (rc != SALTS_OK) {
        fprintf(stderr, "CHttp setup/start failed: %d\n", rc);
        (void)chttp_server_destroy(&server);
        return 1;
    }
    puts("MyGames developer-only room server: http://127.0.0.1:1780");
    puts("Press Enter to stop.");
    (void)getchar();
    rc = chttp_server_stop(&server, 5000u);
    if (rc != SALTS_OK) {
        fprintf(stderr, "CHttp stop failed (owner not destroyed): %d\n", rc);
        return 1;
    }
    return chttp_server_destroy(&server) == SALTS_OK ? 0 : 1;
}
