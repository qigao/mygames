/* Real Flowie v1.0.4 MQTT client against test-only authenticated broker.
 * Verifies that CONNECT is credential gated and the internal topic has
 * *separate* PUBLISH and SUBSCRIBE grants. No credential is printed.
 */
#include <flowie_mqtt_client.h>
#include "mygames/consumer_projection.h"
#include "mygames/outbox_dispatch.h"
#include "mygames/consumer_owner.h"
#include "mygames/recovery_wire.h"
#include "flowie_owner_http_worker.h"
#include <cmeta_crypto.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define TOPIC "mygames/internal/v1/room-events"

typedef struct probe {
    atomic_bool connect_done;
    atomic_bool connect_allowed;
    atomic_bool operation_done;
    atomic_bool operation_allowed;
    atomic_bool received;
    uint8_t received_payload[MYGAMES_STORE_EVENT_MAX + 1u];
    size_t received_size; /* published by received release store */
    mygames_consumer_owner *owner; /* callback-borrowed until Flowie destroyed */
    mygames_consumer_projection *projection;
    mygames_owner_ticket mqtt_ticket;
    mygames_test_owner_http *native_http; /* stack lifetime through Flowie */
    atomic_bool overlapped_http_request;
} probe;
static bool read_positive_field(const char *json, const char *field,
                                uint64_t *out);

static bool wait_bool(atomic_bool *flag, uint64_t deadline_ms) {
    const uint64_t start = cmeta_monotonic_ms();
    while (!atomic_load_explicit(flag, memory_order_acquire) &&
           cmeta_monotonic_ms() - start < deadline_ms)
        cmeta_sleep_ms(4u);
    return atomic_load_explicit(flag, memory_order_acquire);
}

static void on_connect(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response,
                       void *user) {
    probe *p = (probe *)user;
    const bool accepted = status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_CONNACK &&
        response->reason_code == 0u;
    (void)client;
    atomic_store_explicit(&p->connect_allowed, accepted, memory_order_relaxed);
    atomic_store_explicit(&p->connect_done, true, memory_order_release);
}

static void on_subscribe(flowie_mqtt_client_t *client, int status,
                         const flowie_mqtt_control_packet_view_t *response,
                         void *user) {
    probe *p = (probe *)user;
    const bool granted = status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_SUBACK &&
        response->reason_codes.size == 1u &&
        response->reason_codes.data &&
        response->reason_codes.data[0] <= 2u;
    (void)client;
    atomic_store_explicit(&p->operation_allowed, granted, memory_order_relaxed);
    atomic_store_explicit(&p->operation_done, true, memory_order_release);
}

static void on_publish(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response,
                       void *user) {
    probe *p = (probe *)user;
    const bool granted = status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_PUBACK &&
        response->reason_code < 0x80u;
    (void)client;
    atomic_store_explicit(&p->operation_allowed, granted, memory_order_relaxed);
    atomic_store_explicit(&p->operation_done, true, memory_order_release);
}

static int on_message(flowie_mqtt_client_t *client,
                      const flowie_mqtt_publish_view_t *message,
                      void *user) {
    probe *p = (probe *)user;
    static const char expected[] = "\"type\":\"room.command_accepted\"";
    size_t i;
    bool recognized = false;
    (void)client;
    if (!message || !message->topic.data ||
        message->topic.size != sizeof(TOPIC) - 1u ||
        memcmp(message->topic.data, TOPIC, sizeof(TOPIC) - 1u) != 0 ||
        !message->payload.data || message->payload.size > 1024u)
        return SALTS_EPROTO;
    for (i = 0u; i + sizeof(expected) - 1u <= message->payload.size; ++i) {
        if (memcmp(message->payload.data + i, expected,
                   sizeof(expected) - 1u) == 0)
            recognized = true;
    }
    if (!recognized || message->payload.size == 0u ||
        message->payload.size > MYGAMES_STORE_EVENT_MAX)
        return SALTS_EPROTO;
    if (atomic_load_explicit(&p->received, memory_order_acquire))
        return SALTS_ENOBUFS;
    memcpy(p->received_payload, message->payload.data,
           message->payload.size);
    p->received_payload[message->payload.size] = 0u;
    p->received_size = message->payload.size;
    if (p->owner) {
        uint64_t epoch = 0u, seq = 0u;
        /* Callback copied borrowed Flowie bytes above. This is queue
         * admission ONLY; Flowie 1.0.4 auto-PUBACK may precede DB COMMIT. */
        if (!read_positive_field((const char *)p->received_payload,
                                 "\"room_epoch\":", &epoch) ||
            !read_positive_field((const char *)p->received_payload,
                                 "\"seq\":", &seq) ||
            mygames_consumer_owner_admit_mqtt(
                p->owner, TOPIC, p->received_payload, p->received_size,
                epoch, seq, &p->mqtt_ticket) != MYGAMES_OWNER_OK)
            return SALTS_ENOBUFS;
        if (p->native_http &&
            atomic_load_explicit(&p->native_http->in_flight,
                                 memory_order_acquire))
            atomic_store_explicit(&p->overlapped_http_request, true,
                                  memory_order_release);
    }
    atomic_store_explicit(&p->received, true, memory_order_release);
    return SALTS_OK;
}

/* Parse integers from already fully copied, bounded event bytes. Only after
 * strict canonical schema validation are these values trusted. */
static bool read_positive_field(const char *json, const char *field,
                                uint64_t *out) {
    const char *where, *cursor;
    uint64_t value = 0u;
    if (!json || !field || !out) return false;
    where = strstr(json, field);
    if (!where) return false;
    cursor = where + strlen(field);
    if (*cursor < '1' || *cursor > '9') return false;
    while (*cursor >= '0' && *cursor <= '9') {
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        ++cursor;
    }
    if (*cursor != '}' && *cursor != ',') return false;
    *out = value;
    return true;
}

/* An explicitly test-only overlap of one genuine Flowie MQTT callback and
 * an independently validated canonical HISTORY page with the same seq.
 * No CHttp transport is faked here: the actual separate port14 HTTPS tests
 * qualify remote transport; this test qualifies one in-process ledger Owner.
 * Flowie #22 native delayed PUBACK remains NOT IMPLEMENTED. */
static bool project_received_owner(const probe *p, const char *namespace_id,
                                   const char *expected,
                                   uint64_t owner_epoch, uint64_t seq) {
    mygames_store_archive_page page = {0};
    mygames_recovery_grant grant = {0};
    mygames_owner_ticket history[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
    char encoded[MYGAMES_RECOVERY_RESPONSE_MAX];
    uint64_t cursor = 0u, effects = 0u;
    size_t used = 0u, admitted = 0u;
    mygames_owner_result first, second;
    bool valid;
    int n;
    if (!p || !p->owner || !p->projection || !namespace_id ||
        !owner_epoch || !seq || seq > (uint64_t)INT64_MAX)
        return false;
    if (mygames_consumer_projection_snapshot(
            p->projection, namespace_id, "backend-projection-v1",
            &cursor, &effects) != MYGAMES_CONSUMER_APPLIED ||
        cursor != effects ||
        (cursor != seq && cursor + 1u != seq))
        return false;
    grant.generation = 1u;
    grant.after_committed_seq = seq - 1u;
    grant.page_limit = 1u;
    n = snprintf(grant.source_namespace, sizeof(grant.source_namespace),
                 "%s", namespace_id);
    if (n <= 0 || (size_t)n >= sizeof(grant.source_namespace)) return false;
    n = snprintf(grant.consumer_id, sizeof(grant.consumer_id),
                 "%s", "backend-projection-v1");
    if (n <= 0 || (size_t)n >= sizeof(grant.consumer_id)) return false;
    page.epoch = owner_epoch;
    page.checkpoint_sequence = seq;
    page.retained_from_sequence = 1u;
    page.resume_after_sequence = seq;
    page.count = 1u;
    page.items[0].sequence = seq;
    page.items[0].size = p->received_size;
    memcpy(page.items[0].payload, p->received_payload, p->received_size);
    if (mygames_recovery_encode_page(
            &grant, &page, mygames_outbox_public_event_validate,
            encoded, sizeof(encoded), &used) != MYGAMES_RECOVERY_WIRE_OK ||
        mygames_consumer_owner_admit_history(
            p->owner, encoded, used, seq - 1u,
            history, MYGAMES_STORE_ARCHIVE_PAGE_MAX, &admitted) !=
            MYGAMES_OWNER_OK || admitted != 1u)
        return false;

    first = mygames_consumer_owner_pump_one(p->owner, &cursor);
    second = mygames_consumer_owner_pump_one(p->owner, &cursor);
    if ((first != MYGAMES_OWNER_APPLIED &&
         first != MYGAMES_OWNER_REPLAY) ||
        second != MYGAMES_OWNER_REPLAY ||
        mygames_consumer_projection_snapshot(
            p->projection, namespace_id, "backend-projection-v1",
            &cursor, &effects) != MYGAMES_CONSUMER_APPLIED ||
        cursor != seq || effects != seq)
        return false;
    valid = (expected && strcmp(expected, "applied") == 0 &&
             first == MYGAMES_OWNER_APPLIED) ||
            (expected && strcmp(expected, "replay") == 0 &&
             first == MYGAMES_OWNER_REPLAY);
    if (valid)
        printf("FLOWIE_ACL_PROJECTION_%s count=%llu seq=%llu\n",
               first == MYGAMES_OWNER_REPLAY ? "REPLAY" : "APPLIED",
               (unsigned long long)effects, (unsigned long long)cursor);
    return valid;
}

/* Real CHttp recovered bytes are fed in via a trusted test harness stdin
 * pipe, while the independent Flowie network callback admits the matching
 * MQTT event. Both entries share exactly the same Owner and Ledger.
 * The harness, NOT this subscriber, verifies HTTPS TLS before injection. */
static bool accept_real_history_stdin(probe *p) {
    char length_line[32];
    char bytes[MYGAMES_RECOVERY_RESPONSE_MAX];
    char *end = NULL;
    unsigned long length;
    size_t n = 0u;
    mygames_owner_ticket tickets[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
    if (!p || !p->owner ||
        !fgets(length_line, sizeof(length_line), stdin))
        return false;
    length = strtoul(length_line, &end, 10);
    if (!end || (*end != '\n' && *end != 0) ||
        length == 0ul || length > sizeof(bytes))
        return false;
    if (fread(bytes, 1u, length, stdin) != length)
        return false;
    /* Explicit test source stream begins at durable zero, which is
     * independently checked after the first Owner receives the source. */
    return mygames_consumer_owner_admit_history(
               p->owner, bytes, length, 0u,
               tickets, MYGAMES_STORE_ARCHIVE_PAGE_MAX, &n) ==
               MYGAMES_OWNER_OK && n == 1u;
}
static bool project_real_history_and_flowie_owner(const probe *p) {
    const char *ns = getenv("MYGAMES_TEST_SOURCE_NAMESPACE");
    uint64_t last = 0u, effects = 0u;
    mygames_owner_result a, b;
    if (!p || !p->owner || !p->projection || !ns ||
        !p->mqtt_ticket.generation)
        return false;
    a = mygames_consumer_owner_pump_one(p->owner, &last);
    b = mygames_consumer_owner_pump_one(p->owner, &last);
    if (a != MYGAMES_OWNER_APPLIED ||
        b != MYGAMES_OWNER_REPLAY ||
        mygames_consumer_projection_snapshot(
            p->projection, ns, "backend-projection-v1",
            &last, &effects) != MYGAMES_CONSUMER_APPLIED ||
        last != 1u || effects != 1u)
        return false;
    puts("FLOWIE_OWNER_REAL_MTLS_HISTORY_PASS");
    return true;
}

static bool project_received_event(const probe *p) {
    const char *database = getenv("MYGAMES_TEST_PROJECTION_DB");
    const char *driver = getenv("MYGAMES_TEST_PROJECTION_DRIVER");
    const char *ns = getenv("MYGAMES_TEST_SOURCE_NAMESPACE");
    const char *expected = getenv("MYGAMES_TEST_EXPECT_PROJECTION");
    mygames_consumer_projection *projection = NULL;
    uint8_t hash[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES];
    uint64_t epoch = 0u, seq = 0u, applied = 0u;
    mygames_consumer_status status;
    bool ok;
    if (!database && !driver && !ns) return true; /* ACL-only test */
    if (!database || !driver || !ns || !*database || !*driver || !*ns ||
        !read_positive_field((const char *)p->received_payload,
                             "\"room_epoch\":", &epoch) ||
        !read_positive_field((const char *)p->received_payload,
                             "\"seq\":", &seq) ||
        !mygames_outbox_public_event_validate(p->received_payload,
                                              p->received_size, epoch, seq))
        return false;
    if (p->owner)
        return project_received_owner(p, ns, expected, epoch, seq);
    if (cmeta_sha256(p->received_payload, p->received_size, hash) != SALTS_OK)
        return false;
    status = mygames_consumer_projection_open(driver, database, &projection);
    if (status != MYGAMES_CONSUMER_APPLIED || !projection) return false;
    status = mygames_consumer_projection_apply(
        projection, ns, "backend-projection-v1", seq, hash, &applied);
    ok = status == MYGAMES_CONSUMER_APPLIED ||
         status == MYGAMES_CONSUMER_REPLAY;
    if (expected && strcmp(expected,"applied") == 0)
        ok = ok && status == MYGAMES_CONSUMER_APPLIED;
    if (expected && strcmp(expected,"replay") == 0)
        ok = ok && status == MYGAMES_CONSUMER_REPLAY;
    if (ok)
        printf("FLOWIE_ACL_PROJECTION_%s count=%llu seq=%llu\n",
               status == MYGAMES_CONSUMER_REPLAY ? "REPLAY" : "APPLIED",
               (unsigned long long)applied, (unsigned long long)seq);
    else
        fprintf(stderr, "Subscriber projection rejected event: status=%d\n",
                (int)status);
    mygames_consumer_projection_close(projection);
    memset(hash, 0, sizeof(hash));
    return ok;
}

int main(int argc, char **argv) {
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t connect = {0};
    flowie_mqtt_subscribe_packet_t subscribe = {0};
    flowie_mqtt_subscription_t subscription = {0};
    flowie_mqtt_client_publish_topic_t item = {0};
    flowie_mqtt_client_publish_topic_vec_t items = {0};
    flowie_mqtt_client_topic_handler_t handler = {0};
    flowie_mqtt_client_t *client = NULL;
    probe p;
    mygames_test_owner_http native_http = {0};
    const char *identity = NULL, *password = NULL, *client_id = NULL;
    const char *mode;
    int rc = 1;
    bool expect_connect, expect_subscribe = false, expect_publish = false;
    static const char payload[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":1,\"seq\":1}";
    if (argc != 2) return 2;
    mode = argv[1];
    expect_connect = strcmp(mode, "bad-secret") != 0;
    if (strcmp(mode, "subscriber") == 0 ||
        strcmp(mode, "subscriber-listen") == 0 ||
        strcmp(mode, "subscriber-no-commit") == 0 ||
        strcmp(mode, "subscriber-owner") == 0 ||
        (strcmp(mode, "subscriber-owner-mtls") == 0 || strcmp(mode, "subscriber-owner-native") == 0)) {
        identity = "svc-consumer";
        password = getenv("MYGAMES_TEST_CONSUMER_SECRET");
        client_id = "mygames-consumer-v1";
        expect_subscribe = true;
    } else if (strcmp(mode, "subscriber-wildcard-denied") == 0 ||
               strcmp(mode, "consumer-publish-denied") == 0) {
        identity = "svc-consumer";
        password = getenv("MYGAMES_TEST_CONSUMER_SECRET");
        client_id = "mygames-consumer-v1";
        expect_subscribe = strcmp(mode, "subscriber-wildcard-denied") == 0;
        expect_publish = !expect_subscribe;
    } else if (strcmp(mode, "publisher-other-topic-denied") == 0) {
        identity = "svc-outbox";
        password = getenv("MYGAMES_TEST_PUBLISHER_SECRET");
        client_id = "mygames-outbox-v1";
        expect_publish = true;
    } else if (strcmp(mode, "player-wildcard-denied") == 0) {
        identity = "player-browser";
        password = getenv("MYGAMES_TEST_PLAYER_SECRET");
        client_id = "mygames-player-v1";
        expect_subscribe = true;
    } else if (strcmp(mode, "publisher-subscribe-denied") == 0) {
        identity = "svc-outbox";
        password = getenv("MYGAMES_TEST_PUBLISHER_SECRET");
        client_id = "mygames-outbox-v1";
        expect_subscribe = true;
    } else if (strcmp(mode, "player-subscribe-denied") == 0) {
        identity = "player-browser";
        password = getenv("MYGAMES_TEST_PLAYER_SECRET");
        client_id = "mygames-player-v1";
        expect_subscribe = true;
    } else if (strcmp(mode, "player-publish-denied") == 0) {
        identity = "player-browser";
        password = getenv("MYGAMES_TEST_PLAYER_SECRET");
        client_id = "mygames-player-v1";
        expect_publish = true;
    } else if (strcmp(mode, "bad-secret") == 0) {
        identity = "svc-outbox";
        password = "invalid-test-password-no-credential";
        client_id = "mygames-outbox-v1";
    } else return 2;
    if (!password || !password[0]) return 2;
    p.owner = NULL;
    p.projection = NULL;
    p.mqtt_ticket = (mygames_owner_ticket){0};
    p.native_http = NULL;
    atomic_init(&p.overlapped_http_request, false);
    if (strcmp(mode, "subscriber-owner") == 0 ||
        (strcmp(mode, "subscriber-owner-mtls") == 0 || strcmp(mode, "subscriber-owner-native") == 0)) {
        const char *db = getenv("MYGAMES_TEST_PROJECTION_DB");
        const char *driver = getenv("MYGAMES_TEST_PROJECTION_DRIVER");
        const char *ns = getenv("MYGAMES_TEST_SOURCE_NAMESPACE");
        if (!db || !driver || !ns ||
            mygames_consumer_projection_open(
                driver, db, &p.projection) != MYGAMES_CONSUMER_APPLIED)
            return 2;
        if (mygames_consumer_owner_open(
                p.projection, ns, "backend-projection-v1",
                &p.owner) != MYGAMES_OWNER_OK) {
            mygames_consumer_projection_close(p.projection);
            return 2;
        }
    }
    atomic_init(&p.connect_done, false);
    atomic_init(&p.connect_allowed, false);
    atomic_init(&p.operation_done, false);
    atomic_init(&p.operation_allowed, false);
    atomic_init(&p.received, false);
    config.size = sizeof(config);
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    config.host = "127.0.0.1";
    config.port = 18893;
    config.timeout_ms = 4000u;
    config.max_packet_size = 8192u;
    config.command_queue_capacity = 3u;
    config.command_queue_max_bytes = 8192u;
    config.on_connect = on_connect;
    config.on_subscribe = on_subscribe;
    config.on_publish = on_publish;
    config.user_data = &p;
    if ((strcmp(mode, "subscriber-listen") == 0 ||
         strcmp(mode, "subscriber-no-commit") == 0 ||
         strcmp(mode, "subscriber-owner") == 0 ||
         (strcmp(mode, "subscriber-owner-mtls") == 0 || strcmp(mode, "subscriber-owner-native") == 0))) {
        handler.filter = (flowie_mqtt_span_t){
            (const uint8_t *)TOPIC, sizeof(TOPIC) - 1u};
        handler.on_message = on_message;
        config.topic_handlers.data = &handler;
        config.topic_handlers.count = 1u;
    }
    {
        const int created = flowie_mqtt_client_create(&config, &client);
        if (created != SALTS_OK || !client) {
            fprintf(stderr, "Flowie ACL Client create status=%d\n", created);
            return 3;
        }
    }
    connect.size = sizeof(connect);
    connect.abi_version = FLOWIE_MQTT_PROTOCOL_ABI_V1;
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 1u;
    connect.keep_alive = 10u;
    connect.client_id = (flowie_mqtt_span_t){
        (const uint8_t *)client_id, strlen(client_id)};
    connect.has_username = 1u;
    connect.has_password = 1u;
    connect.username = (flowie_mqtt_span_t){
        (const uint8_t *)identity, strlen(identity)};
    connect.password = (flowie_mqtt_span_t){
        (const uint8_t *)password, strlen(password)};
    if (flowie_mqtt_client_connect(client, &connect) != SALTS_OK ||
        !wait_bool(&p.connect_done, 6000u))
        goto cleanup;
    if (atomic_load_explicit(&p.connect_allowed, memory_order_acquire) !=
        expect_connect)
        goto cleanup;
    if (!expect_connect) { rc = 0; goto cleanup; }
    if (expect_subscribe) {
        subscribe.size = sizeof(subscribe);
        subscribe.abi_version = FLOWIE_MQTT_PROTOCOL_ABI_V1;
        subscribe.version = FLOWIE_MQTT_VERSION_5;
        {
            const char *filter = TOPIC;
            if (strcmp(mode, "subscriber-wildcard-denied") == 0)
                filter = "mygames/internal/v1/#";
            else if (strcmp(mode, "player-wildcard-denied") == 0)
                filter = "mygames/#";
            subscription.filter = (flowie_mqtt_span_t){
                (const uint8_t *)filter, strlen(filter)};
        }
        subscription.qos = 1u;
        subscribe.subscriptions = &subscription;
        subscribe.subscription_count = 1u;
        if (flowie_mqtt_client_subscribe(client, &subscribe) != SALTS_OK ||
            !wait_bool(&p.operation_done, 6000u))
            goto cleanup;
        if (atomic_load_explicit(&p.operation_allowed, memory_order_acquire) !=
            (strcmp(mode, "subscriber") == 0 ||
             strcmp(mode, "subscriber-listen") == 0 ||
             strcmp(mode, "subscriber-no-commit") == 0 ||
             strcmp(mode, "subscriber-owner") == 0 ||
             (strcmp(mode, "subscriber-owner-mtls") == 0 || strcmp(mode, "subscriber-owner-native") == 0)))
            goto cleanup;
        if ((strcmp(mode, "subscriber-listen") == 0 ||
         strcmp(mode, "subscriber-no-commit") == 0 ||
         strcmp(mode, "subscriber-owner") == 0 ||
         (strcmp(mode, "subscriber-owner-mtls") == 0 || strcmp(mode, "subscriber-owner-native") == 0))) {
            puts("FLOWIE_ACL_SUBSCRIBER_READY");
            fflush(stdout);
            if (strcmp(mode, "subscriber-owner-native") == 0) {
                const char *port_s = getenv("MYGAMES_TEST_RECOVERY_PORT");
                const char *ca = getenv("MYGAMES_TEST_RECOVERY_CA");
                const char *crt = getenv("MYGAMES_TEST_RECOVERY_CERT");
                const char *key = getenv("MYGAMES_TEST_RECOVERY_KEY");
                const char *ns = getenv("MYGAMES_TEST_SOURCE_NAMESPACE");
                uint64_t after = 0u, effects = 0u;
                unsigned long parsed_port;
                char *end = NULL;
                if (!p.owner || !p.projection || !port_s || !ca || !crt ||
                    !key || !ns ||
                    mygames_consumer_projection_snapshot(
                        p.projection, ns, "backend-projection-v1",
                        &after, &effects) != MYGAMES_CONSUMER_APPLIED ||
                    after != effects || after != 0u)
                    goto cleanup;
                parsed_port = strtoul(port_s, &end, 10);
                if (!end || *end || parsed_port == 0ul ||
                    parsed_port > 65535ul) goto cleanup;
                native_http.owner = p.owner;
                native_http.port = (uint16_t)parsed_port;
                native_http.ca_file = ca;
                native_http.client_cert = crt;
                native_http.client_key = key;
                native_http.committed_after = after;
                p.native_http = &native_http;
                if (mygames_test_owner_http_start(&native_http) != SALTS_OK)
                    goto cleanup;
                puts("FLOWIE_OWNER_NATIVE_HTTP_STARTED");
                fflush(stdout);
                if (!wait_bool(&p.received, 9000u)) goto cleanup;
                if (mygames_test_owner_http_join(&native_http) != SALTS_OK)
                    goto cleanup;
                if (!atomic_load_explicit(
                        &p.overlapped_http_request, memory_order_acquire))
                    goto cleanup;
                if (!project_real_history_and_flowie_owner(&p))
                    goto cleanup;
                puts("FLOWIE_OWNER_NATIVE_CHTTP_AND_MQTT_PASS");
                fflush(stdout);
                goto subscription_finished;
            }
            if (strcmp(mode, "subscriber-owner-mtls") == 0) {
                if (!accept_real_history_stdin(&p) ||
                    !wait_bool(&p.received, 9000u) ||
                    !project_real_history_and_flowie_owner(&p))
                    goto cleanup;
                /* No duplicate projection and no MQTT ACK control here:
                 * Flowie 1.0.4 auto-PUBACK is still pre-COMMIT. */
                goto subscription_finished;
            }
            if (!wait_bool(&p.received, 9000u)) goto cleanup;
            if (strcmp(mode, "subscriber-no-commit") == 0) {
                /* Explicitly simulate a crash window after Flowie's
                 * on_message returned SUCCESS (auto PUBACK happens on the
                 * client worker) but before the consumer's DB transaction.
                 * The harness kills this process after Publisher PUBACK. */
                puts("FLOWIE_RECEIVED_BUT_NOT_COMMITTED");
                fflush(stdout);
                while (true) cmeta_sleep_ms(100u);
            }
            if (!project_received_event(&p)) goto cleanup;
        }
    }
subscription_finished:
    if (expect_publish) {
        item.qos = 1u;
        {
            const char *topic = TOPIC;
            if (strcmp(mode, "publisher-other-topic-denied") == 0)
                topic = "mygames/internal/v1/room-events-extra";
            item.topic = (flowie_mqtt_span_t){
                (const uint8_t *)topic, strlen(topic)};
        }
        item.payload = (flowie_mqtt_span_t){
            (const uint8_t *)payload, sizeof(payload) - 1u};
        items.size = sizeof(items);
        items.version = FLOWIE_MQTT_VERSION_5;
        items.data = &item;
        items.count = 1u;
        if (flowie_mqtt_client_publish(client, &items) != SALTS_OK ||
            !wait_bool(&p.operation_done, 6000u))
            goto cleanup;
        if (atomic_load_explicit(&p.operation_allowed, memory_order_acquire))
            goto cleanup;
    }
    rc = 0;
cleanup:
    if (native_http.started &&
        mygames_test_owner_http_join(&native_http) != SALTS_OK)
        rc = 1;
    if (client && flowie_mqtt_client_try_destroy(client, 5000u) != SALTS_OK)
        return 5;
    /* Flowie callback borrows the Owner pointer; stop/join Flowie before
     * freeing Owner and its private consumer DB connection. */
    if (p.owner &&
        mygames_consumer_owner_destroy(p.owner) != MYGAMES_OWNER_OK)
        return 5;
    mygames_consumer_projection_close(p.projection);
    if (!rc)
        printf("FLOWIE_ACL_TEST_PASS %s\n", mode);
    else
        fprintf(stderr, "Flowie ACL rejected expected state: %s\n", mode);
    return rc;
}
