/* Qualification-only: Flowie #73 native deferred QoS1 -> actual TurboDB COMMIT.
 * The persistent session is retained by a RUNNING test Broker. This does not
 * prove Broker process-restart durability, production IAM, or exactly-once.
 */
#include <flowie_mqtt_client.h>
#include "mygames/consumer_owner.h"
#include "mygames/consumer_projection.h"
#include "mygames/outbox_dispatch.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOPIC "mygames/internal/v1/room-events"
#define CLIENT_ID "mygames-consumer-v1"
#define CONSUMER_ID "backend-projection-v1"
#define TIMEOUT_MS 9000u

typedef struct deferred_probe {
    mygames_consumer_owner *owner; /* kept alive until client worker joins */
    flowie_mqtt_client_inbound_token_t token; /* copied in callback */
    atomic_bool received;
    atomic_bool error;
    atomic_int connected;
    atomic_int subscribed;
    atomic_int settled;
    atomic_int connect_raw_status;
    atomic_int connect_reason_code;
    atomic_int last_network_error;
    int session_present; /* published by connected release-store */
} deferred_probe;

static bool read_positive_field(const char *json, const char *key, uint64_t *value) {
    const char *cursor = json ? strstr(json, key) : NULL;
    uint64_t v = 0u;
    if (!cursor || !value) return false;
    cursor += strlen(key);
    if (*cursor < '1' || *cursor > '9') return false;
    while (*cursor >= '0' && *cursor <= '9') {
        const uint64_t digit = (uint64_t)(*cursor++ - '0');
        if (v > (UINT64_MAX - digit) / 10u) return false;
        v = v * 10u + digit;
    }
    if (*cursor != ',' && *cursor != '}') return false;
    *value = v;
    return true;
}

static int ordinary_receive(flowie_mqtt_client_t *client,
                            const flowie_mqtt_publish_view_t *message, void *user) {
    (void)client;
    (void)message;
    (void)user;
    /* QoS1 MUST arrive through the opted-in borrowed-token callback. */
    return SALTS_EPROTO;
}

static int on_delivery(flowie_mqtt_client_t *client,
                       const flowie_mqtt_publish_view_t *message,
                       const flowie_mqtt_client_inbound_token_t *token, void *user) {
    deferred_probe *p = (deferred_probe *)user;
    char copied[MYGAMES_STORE_EVENT_MAX + 1u];
    mygames_owner_ticket admitted = {0};
    uint64_t epoch = 0u, sequence = 0u;
    (void)client;
    if (!p || !message || !token || !p->owner || message->qos != 1u ||
        token->session_generation == 0u || token->delivery_sequence == 0u ||
        token->packet_id == 0u || !message->topic.data ||
        message->topic.size != sizeof(TOPIC) - 1u ||
        memcmp(message->topic.data, TOPIC, sizeof(TOPIC) - 1u) != 0 ||
        !message->payload.data || !message->payload.size ||
        message->payload.size > MYGAMES_STORE_EVENT_MAX ||
        atomic_load_explicit(&p->received, memory_order_acquire))
        return SALTS_EPROTO;
    memcpy(copied, message->payload.data, message->payload.size);
    copied[message->payload.size] = '\0';
    if (!read_positive_field(copied, "\"room_epoch\":", &epoch) ||
        !read_positive_field(copied, "\"seq\":", &sequence) ||
        !mygames_outbox_public_event_validate(
            (const uint8_t *)copied, message->payload.size, epoch, sequence) ||
        mygames_consumer_owner_admit_mqtt(
            p->owner, TOPIC, copied, message->payload.size,
            epoch, sequence, &admitted) != MYGAMES_OWNER_OK)
        return SALTS_ENOBUFS;
    p->token = *token; /* no borrowed Flowie pointers escape */
    atomic_store_explicit(&p->received, true, memory_order_release);
    return SALTS_OK; /* only admission: no DB operation and NO PUBACK */
}

static void on_connect(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response, void *user) {
    deferred_probe *p = (deferred_probe *)user;
    (void)client;
    p->session_present = response ? response->session_present : -1;
    atomic_store_explicit(&p->connect_raw_status, status, memory_order_relaxed);
    atomic_store_explicit(&p->connect_reason_code,
                          response ? response->reason_code : -1, memory_order_relaxed);
    atomic_store_explicit(&p->connected,
        status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_CONNACK &&
        response->reason_code == 0u ? 1 : 0, memory_order_release);
}

static void on_subscribe(flowie_mqtt_client_t *client, int status,
                         const flowie_mqtt_control_packet_view_t *response, void *user) {
    deferred_probe *p = (deferred_probe *)user;
    (void)client;
    atomic_store_explicit(&p->subscribed,
        status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_SUBACK &&
        response->reason_codes.data && response->reason_codes.size == 1u &&
        response->reason_codes.data[0] <= 2u ? 1 : 0, memory_order_release);
}

static void on_settled(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response, void *user) {
    deferred_probe *p = (deferred_probe *)user;
    (void)client;
    atomic_store_explicit(&p->settled,
        status == SALTS_OK && response == NULL ? 1 : 0,
        memory_order_release);
}

static void on_error(flowie_mqtt_client_t *client, int status, void *user) {
    deferred_probe *p = (deferred_probe *)user;
    (void)client;
    atomic_store_explicit(&p->last_network_error, status, memory_order_relaxed);
    atomic_store_explicit(&p->error, true, memory_order_release);
}

static bool wait_status(atomic_int *value, uint64_t max_ms) {
    const uint64_t start = cmeta_monotonic_ms();
    while (atomic_load_explicit(value, memory_order_acquire) < 0 &&
           cmeta_monotonic_ms() - start < max_ms)
        cmeta_sleep_ms(3u);
    return atomic_load_explicit(value, memory_order_acquire) == 1;
}

static bool wait_delivery(deferred_probe *p, uint64_t max_ms) {
    const uint64_t start = cmeta_monotonic_ms();
    while (!atomic_load_explicit(&p->received, memory_order_acquire) &&
           !atomic_load_explicit(&p->error, memory_order_acquire) &&
           cmeta_monotonic_ms() - start < max_ms)
        cmeta_sleep_ms(3u);
    return atomic_load_explicit(&p->received, memory_order_acquire);
}

static void marker(const char *message) {
    puts(message);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *password = getenv("MYGAMES_TEST_CONSUMER_SECRET");
    const char *mode, *namespace_id;
    const bool first = argc == 5 && strcmp(argv[1], "before-commit") == 0;
    const bool second = argc == 5 && strcmp(argv[1], "after-commit-before-ack") == 0;
    const bool third = argc == 5 && strcmp(argv[1], "replay-and-ack") == 0;
    const bool fourth = argc == 5 && strcmp(argv[1], "post-ack-no-redelivery") == 0;
    const bool rollback_fault = argc == 5 && strcmp(argv[1], "rollback-before-cursor") == 0;
    mygames_consumer_projection *projection = NULL;
    mygames_consumer_owner *owner = NULL;
    flowie_mqtt_client_t *client = NULL;
    flowie_mqtt_client_config_t config = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_client_receive_policy_t policy = FLOWIE_MQTT_CLIENT_RECEIVE_POLICY_INIT;
    flowie_mqtt_client_topic_handler_t handler = {0};
    flowie_mqtt_connect_packet_t connect = {0};
    flowie_mqtt_subscribe_packet_t subscribe = {0};
    flowie_mqtt_subscription_t subscription = {0};
    /* MQTT 5 CONNECT session expiry interval = 3600 seconds (0x11). */
    const uint8_t expiry[] = {0x11u, 0x00u, 0x00u, 0x0eu, 0x10u};
    deferred_probe p = {0};
    uint64_t cursor = 0u, effects = 0u;
    mygames_owner_result committed;
    int rc = 1;
    int connect_enqueue = -1;
    if ((!first && !second && !third && !fourth && !rollback_fault) ||
        !password || !*password ||
        !argv[2][0] || !argv[3][0] || !argv[4][0])
        return 2;
    mode = argv[1];
    namespace_id = argv[4];
    atomic_init(&p.received, false);
    atomic_init(&p.error, false);
    atomic_init(&p.connected, -1);
    atomic_init(&p.subscribed, -1);
    atomic_init(&p.settled, -1);
    atomic_init(&p.connect_raw_status, -1);
    atomic_init(&p.connect_reason_code, -1);
    atomic_init(&p.last_network_error, -1);
    if (mygames_consumer_projection_open(argv[2], argv[3], &projection) !=
            MYGAMES_CONSUMER_APPLIED ||
        mygames_consumer_owner_open(projection, namespace_id, CONSUMER_ID,
                                    &owner) != MYGAMES_OWNER_OK)
        goto cleanup;
    p.owner = owner;
    marker("MYGAMES_DEFERRED_OWNER_READY");
    handler.filter = (flowie_mqtt_span_t){
        (const uint8_t *)TOPIC, sizeof(TOPIC) - 1u};
    handler.on_message = ordinary_receive;
    config.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    config.host = "127.0.0.1";
    config.port = 18893;
    config.timeout_ms = 4500u;
    config.max_packet_size = 8192u;
    config.command_queue_capacity = 4u;
    config.command_queue_max_bytes = 8192u;
    config.topic_handlers.data = &handler;
    config.topic_handlers.count = 1u;
    config.on_connect = on_connect;
    config.on_subscribe = on_subscribe;
    config.on_error = on_error;
    config.user_data = &p;
    {
        const int created = flowie_mqtt_client_create(&config, &client);
        if (created != SALTS_OK || !client) {
            fprintf(stderr, "Deferred Client create failed: %d\n", created);
            goto cleanup;
        }
    }
    policy.max_pending_qos1 = 1u;
    policy.on_delivery = on_delivery;
    {
        const int configured = flowie_mqtt_client_set_receive_policy(client, &policy);
        if (configured != SALTS_OK) {
            fprintf(stderr, "Deferred receive policy failed: %d\n", configured);
            goto cleanup;
        }
    }
    marker("MYGAMES_DEFERRED_POLICY_READY");
    connect.size = sizeof(connect);
    connect.abi_version = FLOWIE_MQTT_PROTOCOL_ABI_V1;
    connect.version = FLOWIE_MQTT_VERSION_5;
    connect.clean_start = 0u;
    connect.keep_alive = 10u;
    connect.client_id = (flowie_mqtt_span_t){
        (const uint8_t *)CLIENT_ID, sizeof(CLIENT_ID) - 1u};
    connect.properties = (flowie_mqtt_span_t){expiry, sizeof(expiry)};
    connect.has_username = 1u;
    connect.username = (flowie_mqtt_span_t){
        (const uint8_t *)"svc-consumer", sizeof("svc-consumer") - 1u};
    connect.has_password = 1u;
    connect.password = (flowie_mqtt_span_t){
        (const uint8_t *)password, strlen(password)};
    connect_enqueue = flowie_mqtt_client_connect(client, &connect);
    if (connect_enqueue != SALTS_OK ||
        !wait_status(&p.connected, TIMEOUT_MS) ||
        p.session_present != (first ? 0 : 1)) {
        fprintf(stderr,
            "Deferred CONNECT refused: enqueue=%d callback=%d raw_status=%d "
            "reason=%d session_present=%d expected_present=%d net_error=%d\n",
            connect_enqueue,
            atomic_load_explicit(&p.connected, memory_order_acquire),
            atomic_load_explicit(&p.connect_raw_status, memory_order_relaxed),
            atomic_load_explicit(&p.connect_reason_code, memory_order_relaxed),
            p.session_present, first ? 0 : 1,
            atomic_load_explicit(&p.last_network_error, memory_order_relaxed));
        goto cleanup;
    }
    if (first) {
        subscribe.size = sizeof(subscribe);
        subscribe.abi_version = FLOWIE_MQTT_PROTOCOL_ABI_V1;
        subscribe.version = FLOWIE_MQTT_VERSION_5;
        subscription.filter = (flowie_mqtt_span_t){
            (const uint8_t *)TOPIC, sizeof(TOPIC) - 1u};
        subscription.qos = 1u;
        subscribe.subscriptions = &subscription;
        subscribe.subscription_count = 1u;
        if (flowie_mqtt_client_subscribe(client, &subscribe) != SALTS_OK ||
            !wait_status(&p.subscribed, TIMEOUT_MS))
            goto cleanup;
    }
    marker("MYGAMES_DEFERRED_SESSION_READY");
    if (fourth) {
        /* No new PUBLISH is issued: any delivery now is an unretired
         * duplicate after successful prior PUBACK. */
        if (wait_delivery(&p, 2500u) ||
            mygames_consumer_projection_snapshot(
                projection, namespace_id, CONSUMER_ID, &cursor, &effects) !=
                MYGAMES_CONSUMER_APPLIED || cursor != 1u || effects != 1u)
            goto cleanup;
        marker("MYGAMES_DEFERRED_AFTER_ACK_NO_REDELIVERY");
    } else {
        if (!wait_delivery(&p, TIMEOUT_MS)) goto cleanup;
        if (first) {
            if (mygames_consumer_projection_snapshot(
                    projection, namespace_id, CONSUMER_ID,
                    &cursor, &effects) != MYGAMES_CONSUMER_APPLIED ||
                cursor != 0u || effects != 0u)
                goto cleanup;
            marker("MYGAMES_DEFERRED_BEFORE_COMMIT");
            for (;;) cmeta_sleep_ms(100u); /* killed by CI, never settles */
        }
        if (rollback_fault) {
            /* Test-only true TurboDB failure AFTER receipt INSERT but
             * BEFORE cursor+effect UPDATE. No native PUBACK is submitted,
             * and the Broker's original QoS1 must remain unretired. */
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
            mygames_consumer_projection_fail_after_receipt_once(projection);
#else
            goto cleanup;
#endif
            committed = mygames_consumer_owner_pump_one(owner, &cursor);
            if (committed != MYGAMES_OWNER_QUARANTINED ||
                mygames_consumer_projection_snapshot(
                    projection, namespace_id, CONSUMER_ID,
                    &cursor, &effects) != MYGAMES_CONSUMER_APPLIED ||
                cursor != 0u || effects != 0u)
                goto cleanup;
            marker("MYGAMES_DEFERRED_ROLLBACK_NO_ACK");
            for (;;) cmeta_sleep_ms(100u); /* SIGKILL by supervised CI */
        }
        committed = mygames_consumer_owner_pump_one(owner, &cursor);
        if (committed != (second ? MYGAMES_OWNER_APPLIED : MYGAMES_OWNER_REPLAY) ||
            mygames_consumer_projection_snapshot(
                projection, namespace_id, CONSUMER_ID, &cursor, &effects) !=
                MYGAMES_CONSUMER_APPLIED || cursor != 1u || effects != 1u)
            goto cleanup;
        if (second) {
            marker("MYGAMES_DEFERRED_AFTER_COMMIT_BEFORE_ACK");
            for (;;) cmeta_sleep_ms(100u); /* killed by CI, never settles */
        }
        /* Only COMMIT or matching durable receipt REPLAY may authorize ACK. */
        if (flowie_mqtt_client_settle_inbound_qos1(
                client, &p.token, on_settled, &p) != SALTS_OK ||
            !wait_status(&p.settled, TIMEOUT_MS))
            goto cleanup;
        marker("MYGAMES_DEFERRED_REPLAY_AND_NATIVE_PUBACK");
    }
    rc = 0;
cleanup:
    if (client && flowie_mqtt_client_try_destroy(client, 5000u) != SALTS_OK)
        return 5;
    if (owner && mygames_consumer_owner_destroy(owner) != MYGAMES_OWNER_OK)
        return 5;
    mygames_consumer_projection_close(projection);
    if (rc != 0)
        fprintf(stderr, "deferred TurboDB qualification failed mode=%s\n",
                argc > 1 ? mode : "(missing)");
    return rc;
}
