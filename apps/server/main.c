/* Development-only room gateway. Requires CHttp + CNet installed SDK.
 * Unauthenticated, loopback-only. All WS callbacks use owner_count=1.
 * Never publish this handler to untrusted clients.
 */
#include <http_server/http.h>
#include "mygames/room_directory.h"
#include "mygames/wire.h"
#include "mygames/session_registry.h"
#include <salts/random.h>
#include <salts/clock.h>
#include <cflow/executor.h>
#if defined(MYGAMES_WITH_TURBODB)
#include "mygames/room_store.h"
#include <cmeta_crypto.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MYGAMES_PEER_CAPACITY 32u
#define MYGAMES_DEDUP_WINDOW 8u
#define MYGAMES_SESSION_GRACE_MS UINT64_C(30000)
#define MYGAMES_OWNER_QUEUE_CAPACITY 128u
#define MYGAMES_WS_MAX_INPUT_BYTES 1024u

typedef struct response_cache {
    bool used;
    char request_id[MYGAMES_REQUEST_ID_CAPACITY];
    unsigned char request[MYGAMES_WIRE_MAX_BYTES];
    size_t request_bytes;
    char response[256];
} response_cache;

typedef struct client_peer {
    bool occupied;
    size_t session_slot;
    uint64_t bound_generation;
    chttp_server_websocket_session session;
} client_peer;

typedef struct dev_server_state {
    cflow_executor room_executor;
    cflow_executor_control room_control;
    atomic_bool accepting;
    uint64_t room_epoch;
    uint64_t durable_sequence;
    bool store_halted;
#if defined(MYGAMES_WITH_TURBODB)
    mygames_room_store *store;
#endif
    mygames_room_directory rooms;
    mygames_session_registry sessions;
    client_peer peers[MYGAMES_PEER_CAPACITY];
    response_cache cache[MYGAMES_SESSION_CAPACITY][MYGAMES_DEDUP_WINDOW];
    unsigned int cache_next[MYGAMES_SESSION_CAPACITY];
} dev_server_state;
typedef enum room_task_kind { ROOM_TASK_OPEN, ROOM_TASK_MESSAGE, ROOM_TASK_CLOSE, ROOM_TASK_BINARY } room_task_kind;
typedef struct room_task {
    dev_server_state *state;
    chttp_server_websocket_session session;
    room_task_kind kind;
    size_t size;
    unsigned char payload[MYGAMES_WS_MAX_INPUT_BYTES];
} room_task;

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

static response_cache *cached_request(dev_server_state *state, size_t slot,
                                      const char *request_id) {
    unsigned int i;
    for (i = 0u; i < MYGAMES_DEDUP_WINDOW; ++i) {
        response_cache *item = &state->cache[slot][i];
        if (item->used && strcmp(item->request_id, request_id) == 0) return item;
    }
    return NULL;
}

static void remember_request(dev_server_state *state, size_t slot,
                             const mygames_wire_command *command,
                             const void *bytes, size_t size, const char *response) {
    response_cache *item = &state->cache[slot]
        [state->cache_next[slot]++ % MYGAMES_DEDUP_WINDOW];
    memset(item, 0, sizeof(*item));
    memcpy(item->request_id, command->request_id, strlen(command->request_id) + 1u);
    memcpy(item->request, bytes, size);
    item->request_bytes = size;
    memcpy(item->response, response, strlen(response) + 1u);
    item->used = true;
}

static void clear_cache(dev_server_state *state, size_t slot) {
    memset(state->cache[slot], 0, sizeof(state->cache[slot]));
    state->cache_next[slot] = 0u;
}

static const mygames_session_record *current_identity(
    const dev_server_state *state, const client_peer *peer) {
    const mygames_session_record *record;
    if (!state || !peer || !peer->occupied) return NULL;
    record = mygames_sessions_at(&state->sessions, peer->session_slot);
    return record && record->attached && record->generation == peer->bound_generation
               ? record : NULL;
}

/* Admits a copied, generation-fenced command on the CHttp owner. */
static int send_text(const chttp_server_websocket_session *session, const char *text) {
    return chttp_server_websocket_send_text(session, text, strlen(text));
}

static int health(void *user, const chttp_server_request_view *request,
                  chttp_server_response *response) {
    static const char body[] = "{\"status\":\"ok\",\"protocol\":1,\"mode\":\"development\",\"room_owner\":\"cflow_serial\"}";
    (void)user;
    (void)request;
    return chttp_server_reply(response, 200u, "application/json", body, sizeof(body) - 1u);
}

static bool encode_snapshot(const dev_server_state *state, const mygames_room *room,
                            char *out, size_t capacity) {
    size_t n = 0;
    unsigned int i;
    if (!room || !out || capacity == 0) return false;
    if (!append(out, capacity, &n,
                "{\"v\":1,\"type\":\"room.snapshot\",\"room_id\":\"%s\","
                "\"room_epoch\":%" PRIu64 ",\"revision\":%" PRIu64 ",\"phase\":\"%s\","
                "\"owner_id\":\"%" PRIu64 "\",\"seats\":[",
                room->id, state->room_epoch, room->revision,
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
    if (!room || !encode_snapshot(state, room, json, sizeof(json))) return;
    for (i = 0; i < MYGAMES_PEER_CAPACITY; ++i) {
        client_peer *p = &state->peers[i];
        const mygames_room *member_room;
        const mygames_session_record *record = current_identity(state, p);
        if (!record) continue;
        member_room = mygames_room_directory_get_player(&state->rooms, record->player);
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


/* Single CHttp owner: room changes and session expiry happen in serial.
 * Lazy expiry is progress on ingress, not an autonomous time-based timer. */
static void collect_expired(dev_server_state *state) {
#if defined(MYGAMES_WITH_TURBODB)
    /* Never perform non-transactional room eviction in durable mode.
     * Timed eviction needs a committed system command and remains a gate. */
    if (state->store) return;
#endif
    mygames_player_id expired[MYGAMES_SESSION_CAPACITY] = {0};
    size_t i, n = mygames_sessions_collect_expired(
        &state->sessions, cmeta_monotonic_ms(), expired);
    for (i = 0u; i < n; ++i) {
        const mygames_room *room = mygames_room_directory_get_player(
            &state->rooms, expired[i]);
        char id[MYGAMES_ROOM_ID_CAPACITY] = {0};
        if (!room || room->phase != MYGAMES_ROOM_WAITING) continue;
        memcpy(id, room->id, strlen(room->id) + 1u);
        if (mygames_room_directory_leave(&state->rooms, expired[i]) == MYGAMES_ROOM_OK)
            publish_snapshot(state, mygames_room_directory_get(&state->rooms, id));
    }
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i)
        if (!mygames_sessions_at(&state->sessions, i)) clear_cache(state, i);
}

static void handle_session_resume(dev_server_state *state, client_peer *peer,
                                  const chttp_server_websocket_session *session,
                                  const mygames_wire_command *command) {
    static const char rejection[] =
        "{\"v\":1,\"type\":\"session.error\",\"code\":\"resume_rejected\"}";
    uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES];
    const mygames_session_record *original = current_identity(state, peer);
    const mygames_session_record *recovered;
    size_t claimed = MYGAMES_SESSION_CAPACITY;
    size_t temporary_slot = peer->session_slot;
    mygames_session_status status;
    char response[256];
    const mygames_room *room;
    int length;
    if (!original ||
        mygames_room_directory_get_player(&state->rooms, original->player)) {
        (void)send_text(session, rejection);
        return;
    }
    if (cmeta_platform_secure_random(entropy, sizeof(entropy)) != SALTS_OK) {
        (void)send_text(session, rejection);
        return;
    }
    status = mygames_sessions_resume(&state->sessions, command->resume_token,
                                    entropy, cmeta_monotonic_ms(), &claimed);
    memset(entropy, 0, sizeof(entropy));
    if (status != MYGAMES_SESSION_OK) {
        (void)send_text(session, rejection);
        return;
    }
    /* Revoke the freshly issued provisional credential on this connection. */
    (void)mygames_sessions_discard(&state->sessions, temporary_slot);
    clear_cache(state, temporary_slot);
    peer->session_slot = claimed;
    recovered = mygames_sessions_at(&state->sessions, claimed);
    if (!recovered) return;
    peer->bound_generation = recovered->generation;
    length = snprintf(response, sizeof(response),
        "{\"v\":1,\"type\":\"session.ready\","
        "\"player_id\":\"%" PRIu64 "\",\"generation\":%" PRIu64 ","
        "\"resume_token\":\"%s\",\"resumed\":true}",
        recovered->player, recovered->generation, recovered->ticket);
    if (length <= 0 || (size_t)length >= sizeof(response)) return;
    if (send_text(session, response) != SALTS_OK) return;
    room = mygames_room_directory_get_player(&state->rooms, recovered->player);
    if (room) {
        char snapshot[1024];
        if (encode_snapshot(state, room, snapshot, sizeof(snapshot)))
            (void)send_text(session, snapshot);
    }
}

#if defined(MYGAMES_WITH_TURBODB)
static bool normalize_command_digest(const mygames_wire_command *command,
                                      uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES]) {
    char normalized[192];
    const int n = snprintf(normalized, sizeof(normalized), "v1|%d|%s|%u",
                           (int)command->kind, command->room_id,
                           command->ready ? 1u : 0u);
    return n > 0 && (size_t)n < sizeof(normalized) &&
           cmeta_sha256(normalized, (size_t)n, digest) == SALTS_OK;
}

static void storage_rejection(dev_server_state *state,
                              const chttp_server_websocket_session *session,
                              const mygames_wire_command *command) {
    char message[160];
    state->store_halted = true; /* single CFlow Owner stops issuing game ACKs */
    if (write_error(message, sizeof(message), command->request_id,
                    "storage_unavailable")) (void)send_text(session, message);
}
#endif

static void ws_handle_command(dev_server_state *state, client_peer *client,
                              const chttp_server_websocket_session *session,
                              const void *bytes, size_t size) {
    mygames_wire_command command;
    mygames_wire_status parse_result;
    response_cache *previous;
    const mygames_session_record *identity;
    const mygames_room *room = NULL;
    mygames_room_status result = MYGAMES_ROOM_OK;
    mygames_room_directory *target = &state->rooms;
    char reply[256];
    char room_id[MYGAMES_ROOM_ID_CAPACITY] = {0};
    unsigned int seat = 0u;
    uint64_t revision = 0u;
    bool changed = false;
#if defined(MYGAMES_WITH_TURBODB)
    mygames_room_directory staged;
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    bool persistent_command = false;
#endif

    parse_result = mygames_wire_decode_command(bytes, size, &command);
    if (parse_result != MYGAMES_WIRE_OK) {
        const char *code = parse_result == MYGAMES_WIRE_TOO_LARGE ? "oversized_message" :
                           parse_result == MYGAMES_WIRE_UNSUPPORTED_VERSION ? "unsupported_version" :
                           parse_result == MYGAMES_WIRE_UNSUPPORTED_TYPE ? "unsupported_command" :
                           "invalid_command";
        char error[128];
        const int n = snprintf(error, sizeof(error),
            "{\"v\":1,\"type\":\"server.error\",\"code\":\"%s\"}", code);
        if (n > 0 && (size_t)n < sizeof(error)) (void)send_text(session, error);
        return;
    }
    if (command.kind == MYGAMES_COMMAND_PING) {
        (void)send_text(session, "{\"v\":1,\"type\":\"server.pong\"}");
        return;
    }
    if (command.kind == MYGAMES_COMMAND_RESUME_SESSION) {
        handle_session_resume(state, client, session, &command);
        return;
    }
    identity = current_identity(state, client);
    if (!identity) {
        (void)send_text(session,
            "{\"v\":1,\"type\":\"server.error\",\"code\":\"session_stale\"}");
        return;
    }

#if defined(MYGAMES_WITH_TURBODB)
    persistent_command = state->store != NULL &&
                         command.kind != MYGAMES_COMMAND_GET_SNAPSHOT;
    if (state->store && state->store_halted) {
        storage_rejection(state, session, &command);
        return;
    }
    if (persistent_command) {
        unsigned char saved[MYGAMES_STORE_RESULT_MAX];
        size_t saved_size = 0u;
        uint64_t saved_seq = 0u;
        mygames_store_status status;
        if (identity->player > (uint64_t)INT64_MAX ||
            !normalize_command_digest(&command, digest)) {
            storage_rejection(state, session, &command);
            return;
        }
        /* The durable journal, not a per-socket byte comparison, determines
         * semantic replay for previously committed commands. */
        status = mygames_room_store_get_outcome(
            state->store, identity->player, command.request_id, digest,
            saved, sizeof(saved), &saved_size, &saved_seq);
        if (status == MYGAMES_STORE_OK) {
            if (!saved_size) { storage_rejection(state, session, &command); return; }
            (void)chttp_server_websocket_send_text(session, saved, saved_size);
            return;
        }
        if (status == MYGAMES_STORE_REQUEST_CONFLICT) {
            (void)send_text(session,
                "{\"v\":1,\"type\":\"server.error\",\"code\":\"request_id_conflict\"}");
            return;
        }
        if (status != MYGAMES_STORE_NOT_FOUND) {
            storage_rejection(state, session, &command);
            return;
        }
    }
#endif
    previous = cached_request(state, client->session_slot, command.request_id);
    if (previous) {
        if (size == previous->request_bytes &&
            memcmp(bytes, previous->request, size) == 0) {
            (void)send_text(session, previous->response);
        } else {
            (void)send_text(session,
                "{\"v\":1,\"type\":\"server.error\",\"code\":\"request_id_conflict\"}");
        }
        return;
    }
#if defined(MYGAMES_WITH_TURBODB)
    if (persistent_command) {
        staged = state->rooms;
        target = &staged; /* no live mutation before database commit */
    }
#endif
    room = mygames_room_directory_get_player(target, identity->player);
    if (command.kind == MYGAMES_COMMAND_CREATE_ROOM) {
        result = mygames_room_directory_create(target, identity->player, &room);
        changed = result == MYGAMES_ROOM_OK;
    } else if (command.kind == MYGAMES_COMMAND_JOIN_ROOM) {
        result = mygames_room_directory_join(target, command.room_id,
                                              identity->player, &seat);
        if (result == MYGAMES_ROOM_OK || result == MYGAMES_ROOM_ALREADY_PRESENT)
            room = mygames_room_directory_get_player(target, identity->player);
        changed = result == MYGAMES_ROOM_OK;
    } else if (command.kind == MYGAMES_COMMAND_READY) {
        result = mygames_room_directory_ready(target, identity->player, command.ready);
        changed = result == MYGAMES_ROOM_OK;
        room = mygames_room_directory_get_player(target, identity->player);
    } else if (command.kind == MYGAMES_COMMAND_LEAVE_ROOM) {
        if (room) memcpy(room_id, room->id, strlen(room->id) + 1u);
        result = mygames_room_directory_leave(target, identity->player);
        changed = result == MYGAMES_ROOM_OK;
        room = mygames_room_directory_get(target, room_id);
        seat = MYGAMES_ROOM_SEATS;
    } else if (command.kind == MYGAMES_COMMAND_GET_SNAPSHOT) {
        result = room ? MYGAMES_ROOM_OK : MYGAMES_ROOM_NOT_FOUND;
    } else {
        result = MYGAMES_ROOM_INVALID_ARGUMENT;
    }
    if (result != MYGAMES_ROOM_OK && result != MYGAMES_ROOM_ALREADY_PRESENT) {
        if (!write_error(reply, sizeof(reply), command.request_id, status_string(result)))
            return;
        remember_request(state, client->session_slot, &command, bytes, size, reply);
        (void)send_text(session, reply);
        return;
    }
    if (room) {
        revision = room->revision;
        if (command.kind != MYGAMES_COMMAND_LEAVE_ROOM) {
            unsigned int current_seat;
            if (mygames_room_find_seat(room, identity->player, &current_seat) == MYGAMES_ROOM_OK)
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

#if defined(MYGAMES_WITH_TURBODB)
    if (persistent_command) {
        char sanitized_event[MYGAMES_STORE_EVENT_MAX];
        uint64_t committed_sequence = 0u;
        mygames_store_status status;
        int length;
        if (state->durable_sequence >= (uint64_t)INT64_MAX) {
            storage_rejection(state, session, &command);
            return;
        }
        length = snprintf(sanitized_event, sizeof(sanitized_event),
            "{\"v\":1,\"type\":\"room.command_accepted\","
            "\"room_id\":\"%s\",\"room_epoch\":%" PRIu64 ","
            "\"revision\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
            room_id, state->room_epoch, revision, state->durable_sequence + 1u);
        if (length <= 0 || (size_t)length >= sizeof(sanitized_event)) {
            storage_rejection(state, session, &command);
            return;
        }
        status = mygames_room_store_commit(
            state->store, identity->player, target,
            state->room_epoch, state->durable_sequence,
            command.request_id, digest, reply, strlen(reply),
            sanitized_event, (size_t)length, &committed_sequence);
        if (status == MYGAMES_STORE_REPLAY) {
            uint8_t saved[MYGAMES_STORE_RESULT_MAX];
            size_t saved_size = 0u;
            uint64_t saved_sequence = 0u;
            status = mygames_room_store_get_outcome(
                state->store, identity->player, command.request_id, digest,
                saved, sizeof(saved), &saved_size, &saved_sequence);
            if (status != MYGAMES_STORE_OK || !saved_size) {
                storage_rejection(state, session, &command);
                return;
            }
            (void)chttp_server_websocket_send_text(session, saved, saved_size);
            return;
        }
        if (status != MYGAMES_STORE_OK) {
            fprintf(stderr, "Room transaction rejected (%d), halting admissions\n",
                    (int)status);
            storage_rejection(state, session, &command);
            return;
        }
        /* This assignment is after confirmed COMMIT, before Game ACK. No
         * mutable pointer into staged survives the copy. */
        state->rooms = staged;
        state->durable_sequence = committed_sequence;
        room = mygames_room_directory_get(&state->rooms, room_id);
    }
#endif

    remember_request(state, client->session_slot, &command, bytes, size, reply);
    (void)send_text(session, reply);
    if (changed) {
        if (room) publish_snapshot(state, room);
    } else if (room && command.kind == MYGAMES_COMMAND_GET_SNAPSHOT) {
        char snapshot[1024];
        if (encode_snapshot(state, room, snapshot, sizeof(snapshot)))
            (void)send_text(session, snapshot);
    }
}

/* Every room and session mutation happens on the single CFlow SerialExecutor
 * worker. Network callbacks only copy bounded input into admitted work. */
static void room_open_apply(dev_server_state *state,
                            const chttp_server_websocket_session *session) {
    uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES];
    const mygames_session_record *identity;
    char hello[272];
    size_t issued = MYGAMES_SESSION_CAPACITY;
    unsigned int i;
    int rc = SALTS_ENOBUFS;
    collect_expired(state);
    for (i = 0u; i < MYGAMES_PEER_CAPACITY; ++i) {
        client_peer *peer = &state->peers[i];
        mygames_session_status status;
        int n;
        if (peer->occupied) continue;
        if (cmeta_platform_secure_random(entropy, sizeof(entropy)) != SALTS_OK) {
            rc = SALTS_EIO;
            break;
        }
        status = mygames_sessions_issue(&state->sessions, entropy, &issued);
        memset(entropy, 0, sizeof(entropy));
        if (status != MYGAMES_SESSION_OK) break;
        identity = mygames_sessions_at(&state->sessions, issued);
        if (!identity) { rc = SALTS_EIO; break; }
        clear_cache(state, issued);
        n = snprintf(hello, sizeof(hello),
            "{\"v\":1,\"type\":\"server.hello\",\"capabilities\":[\"ping\",\"rooms\",\"resume\"],"
            "\"player_id\":\"%" PRIu64 "\",\"resume_token\":\"%s\"}",
            identity->player, identity->ticket);
        if (n <= 0 || (size_t)n >= sizeof(hello)) {
            (void)mygames_sessions_discard(&state->sessions, issued);
            rc = SALTS_EINVAL;
            break;
        }
        peer->occupied = true;
        peer->session_slot = issued;
        peer->bound_generation = identity->generation;
        peer->session = *session;
        rc = send_text(session, hello);
        if (rc != SALTS_OK) {
            memset(peer, 0, sizeof(*peer));
            (void)mygames_sessions_discard(&state->sessions, issued);
        }
        break;
    }
    if (rc != SALTS_OK) {
        static const char reason[] = "room_owner_unavailable";
        (void)chttp_server_websocket_close(session, 1013u,
                                            reason, sizeof(reason) - 1u);
    }
}
static void room_event_apply(dev_server_state *state,
                             const chttp_server_websocket_session *session,
                             room_task_kind kind, const void *payload, size_t length) {
    client_peer *peer = find_peer(state, session);
    if (!peer) return;
    if (kind == ROOM_TASK_CLOSE) {
        if (current_identity(state, peer)) {
            (void)mygames_sessions_detach(&state->sessions, peer->session_slot,
                peer->bound_generation, cmeta_monotonic_ms(), MYGAMES_SESSION_GRACE_MS);
        }
        memset(peer, 0, sizeof(*peer));
    } else if (kind == ROOM_TASK_BINARY) {
        static const char reason[] = "text_only";
        (void)chttp_server_websocket_close(session, 1003u,
                                            reason, sizeof(reason) - 1u);
    } else if (kind == ROOM_TASK_MESSAGE) {
        ws_handle_command(state, peer, session, payload, length);
    }
}
static void room_task_run(void *user) {
    room_task *task = (room_task *)user;
    if (!task || !task->state) return;
    if (task->kind == ROOM_TASK_OPEN) room_open_apply(task->state, &task->session);
    else room_event_apply(task->state, &task->session, task->kind,
                          task->payload, task->size);
}
static void room_task_cancel(void *user) { (void)user; }
static void room_task_finalize(void *user) { free(user); }

/* Exact nonblocking admission: on success CFlow owns the descriptor and the
 * heap task until run/cancel+finalize. No borrowed socket packet escapes. */
static cflow_admission_status room_submit(
    dev_server_state *state, const chttp_server_websocket_session *session,
    room_task_kind kind, const void *payload, size_t length) {
    cflow_executor_task descriptor;
    cflow_admission_status admission;
    room_task *task;
    if (!state || !session || (!payload && length) ||
        length > MYGAMES_WS_MAX_INPUT_BYTES) return CFLOW_ADMISSION_INVALID_ARGUMENT;
    if (!atomic_load_explicit(&state->accepting, memory_order_acquire))
        return CFLOW_ADMISSION_CLOSED;
    task = (room_task *)calloc(1u, sizeof(*task));
    if (!task) return CFLOW_ADMISSION_ALLOCATION_FAILED;
    task->state = state;
    task->session = *session;
    task->kind = kind;
    task->size = length;
    if (length) memcpy(task->payload, payload, length);
    descriptor = (cflow_executor_task){
        .run = room_task_run, .cancel = room_task_cancel,
        .finalize = room_task_finalize, .user = task};
    admission = cflow_executor_try_post_task(&state->room_executor, &descriptor);
    if (admission != CFLOW_ADMISSION_ACCEPTED) free(task);
    return admission;
}
static int ws_open(void *user, chttp_websocket *socket,
                   const chttp_server_request_view *request,
                   chttp_server_response *response) {
    dev_server_state *state = (dev_server_state *)user;
    chttp_server_websocket_session session = {0};
    cflow_admission_status admitted;
    int status;
    (void)request; (void)response;
    status = chttp_server_websocket_session_capture(socket, &session);
    if (status != SALTS_OK) return status;
    admitted = room_submit(state, &session, ROOM_TASK_OPEN, NULL, 0u);
    if (admitted == CFLOW_ADMISSION_ACCEPTED) return SALTS_OK;
    return admitted == CFLOW_ADMISSION_FULL ? SALTS_ENOBUFS : SALTS_ESHUTDOWN;
}
static void ws_event(void *user, chttp_websocket *socket,
                     const chttp_websocket_event *event) {
    dev_server_state *state = (dev_server_state *)user;
    chttp_server_websocket_session session = {0};
    room_task_kind kind;
    cflow_admission_status admitted;
    if (!state || !event) return;
    if (chttp_server_websocket_session_capture(socket, &session) != SALTS_OK) return;
    if (event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE) kind = ROOM_TASK_CLOSE;
    else if (event->kind != CHTTP_WEBSOCKET_EVENT_MESSAGE) return;
    else if (event->message_type == CHTTP_WEBSOCKET_MESSAGE_TEXT)
        kind = ROOM_TASK_MESSAGE;
    else kind = ROOM_TASK_BINARY;
    admitted = room_submit(state, &session, kind, event->data, event->size);
    if (admitted != CFLOW_ADMISSION_ACCEPTED && kind != ROOM_TASK_CLOSE) {
        static const char reason[] = "room_queue_full";
        (void)chttp_websocket_close(socket, 1013u, reason, sizeof(reason) - 1u);
    } else if (admitted != CFLOW_ADMISSION_ACCEPTED) {
        fprintf(stderr, "room CLOSE task rejected: %d\n", (int)admitted);
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

#if defined(MYGAMES_WITH_TURBODB)
static bool prepare_room_store(dev_server_state *state) {
    const char *driver = getenv("MYGAMES_SQLITE_PLUGIN");
    const char *database = getenv("MYGAMES_DB_PATH");
    mygames_room_directory recovered;
    mygames_player_id highest = 0u;
    uint64_t epoch = 0u, sequence = 0u, claimed = 0u;
    mygames_store_status status;
    unsigned int i;
    if ((!driver || !driver[0]) && (!database || !database[0]))
        return true; /* explicit memory-only development mode */
    if (!driver || !driver[0] || !database || !database[0]) {
        fprintf(stderr, "MYGAMES_SQLITE_PLUGIN and MYGAMES_DB_PATH must both be set\n");
        return false;
    }
    status = mygames_room_store_open(driver, database, &state->store);
    if (status != MYGAMES_STORE_OK) {
        fprintf(stderr, "TurboDB store open rejected: %d\n", (int)status);
        return false;
    }
    status = mygames_room_store_load(state->store, &recovered, &epoch, &sequence);
    if (status != MYGAMES_STORE_OK) {
        fprintf(stderr, "TurboDB checkpoint rejected: %d\n", (int)status);
        return false;
    }
    /* Tickets and principal associations are not persisted. Never silently
     * reassign an orphaned seat to an unauthenticated new socket identity. */
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        if (recovered.entries[i].used) {
            fprintf(stderr, "Durable lobby contains orphaned seats; refusing "
                            "unauthenticated recovery\n");
            return false;
        }
    }
    status = mygames_room_store_player_high_watermark(state->store, &highest);
    if (status != MYGAMES_STORE_OK || highest >= (uint64_t)INT64_MAX - 1u) {
        fprintf(stderr, "Durable player high-watermark invalid: %d\n", (int)status);
        return false;
    }
    status = mygames_room_store_claim_epoch(state->store, epoch, &claimed);
    if (status != MYGAMES_STORE_OK) {
        fprintf(stderr, "Durable owner epoch claim rejected: %d\n", (int)status);
        return false;
    }
    state->rooms = recovered;
    state->room_epoch = claimed;
    state->durable_sequence = sequence;
    state->sessions.next_player = highest;
    return true;
}
#else
static bool prepare_room_store(dev_server_state *state) {
    (void)state;
    if (getenv("MYGAMES_SQLITE_PLUGIN") || getenv("MYGAMES_DB_PATH")) {
        fprintf(stderr, "TurboDB store requested but build is not enabled\n");
        return false;
    }
    return true;
}
#endif

static void release_room_store(dev_server_state *state) {
#if defined(MYGAMES_WITH_TURBODB)
    if (state->store) {
        mygames_room_store_close(state->store);
        state->store = NULL;
    }
#else
    (void)state;
#endif
}

/* Stop socket admission before draining CFlow and before freeing server impl. */
int main(void) {
    chttp_server server = {0};
    chttp_server_config config = development_config();
    chttp_server_execution_options owner = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    chttp_server_websocket_options ws = {0};
    dev_server_state state = {0};
    int rc, final_rc;
    mygames_room_directory_init(&state.rooms);
    mygames_sessions_init(&state.sessions);
    state.room_epoch = 1u;
    atomic_init(&state.accepting, true);
    if (!prepare_room_store(&state)) {
        release_room_store(&state);
        return 1;
    }
    if (!cflow_executor_serial_init_with_capacity(
            &state.room_executor, MYGAMES_OWNER_QUEUE_CAPACITY)) {
        fprintf(stderr, "CFlow SerialExecutor init failed\n");
        release_room_store(&state);
        return 1;
    }
    if (!cflow_executor_as_control(&state.room_executor, &state.room_control)) {
        cflow_executor_destroy(&state.room_executor);
        release_room_store(&state);
        return 1;
    }
    rc = chttp_server_init(&server, &config);
    if (rc != SALTS_OK) {
        fprintf(stderr, "CHttp init failed: %d\n", rc);
        (void)cflow_executor_control_shutdown(&state.room_control,
                                                CFLOW_EXECUTOR_SHUTDOWN_DRAIN);
        (void)cflow_executor_control_wait_idle(&state.room_control);
        cflow_executor_destroy(&state.room_executor);
        release_room_store(&state);
        return 1;
    }
    owner.owner_count = 1u; /* CHttp transport Owner, not the Room Owner */
    rc = chttp_server_set_execution_options(&server, &owner);
    if (rc == SALTS_OK) rc = chttp_server_get(&server, "/health", health, &state);
    ws.size = sizeof(ws);
    ws.path = "/ws";
    ws.max_frame_bytes = MYGAMES_WS_MAX_INPUT_BYTES;
    ws.max_message_bytes = MYGAMES_WS_MAX_INPUT_BYTES;
    ws.max_buffered_input_bytes = 2048u;
    ws.on_open = ws_open;
    ws.on_event = ws_event;
    ws.user = &state;
    if (rc == SALTS_OK) rc = chttp_server_websocket_with(&server, &ws);
    if (rc == SALTS_OK) rc = chttp_server_start(&server);
    if (rc == SALTS_OK) {
        puts("MyGames local CFlow room service at http://127.0.0.1:1780");
        puts("Press Enter to stop.");
        (void)getchar();
    } else {
        fprintf(stderr, "CHttp setup/start failed: %d\n", rc);
    }
    atomic_store_explicit(&state.accepting, false, memory_order_release);
    final_rc = chttp_server_stop(&server, 5000u);
    if (final_rc != SALTS_OK) {
        fprintf(stderr, "CHttp stop failed, process must terminate: %d\n", final_rc);
        return 1; /* do not free server while producer callbacks might survive */
    }
    if (!cflow_executor_control_shutdown(
            &state.room_control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN) ||
        cflow_executor_control_wait_idle(&state.room_control)
            != CFLOW_EXECUTOR_WAIT_IDLE) {
        fprintf(stderr, "CFlow drain incomplete\n");
        return 1; /* never release state used by accepted work */
    }
    cflow_executor_destroy(&state.room_executor);
    final_rc = chttp_server_destroy(&server);
    release_room_store(&state);
    return (rc == SALTS_OK && final_rc == SALTS_OK) ? 0 : 1;
}
