/* Real Flowie v1.0.4 MQTT client against test-only authenticated broker.
 * Verifies that CONNECT is credential gated and the internal topic has
 * *separate* PUBLISH and SUBSCRIBE grants. No credential is printed.
 */
#include <flowie_mqtt_client.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOPIC "mygames/internal/v1/room-events"

typedef struct probe {
    atomic_bool connect_done;
    atomic_bool connect_allowed;
    atomic_bool operation_done;
    atomic_bool operation_allowed;
    atomic_bool received;
} probe;

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
    if (!recognized) return SALTS_EPROTO;
    atomic_store_explicit(&p->received, true, memory_order_release);
    return SALTS_OK;
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
    if (strcmp(mode, "subscriber") == 0 || strcmp(mode, "subscriber-listen") == 0) {
        identity = "svc-consumer";
        password = getenv("MYGAMES_TEST_CONSUMER_SECRET");
        client_id = "mygames-consumer-v1";
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
    if (strcmp(mode, "subscriber-listen") == 0) {
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
        subscription.filter = (flowie_mqtt_span_t){
            (const uint8_t *)TOPIC, sizeof(TOPIC) - 1u};
        subscription.qos = 1u;
        subscribe.subscriptions = &subscription;
        subscribe.subscription_count = 1u;
        if (flowie_mqtt_client_subscribe(client, &subscribe) != SALTS_OK ||
            !wait_bool(&p.operation_done, 6000u))
            goto cleanup;
        if (atomic_load_explicit(&p.operation_allowed, memory_order_acquire) !=
            (strcmp(mode, "subscriber") == 0 ||
             strcmp(mode, "subscriber-listen") == 0))
            goto cleanup;
        if (strcmp(mode, "subscriber-listen") == 0) {
            puts("FLOWIE_ACL_SUBSCRIBER_READY");
            fflush(stdout);
            if (!wait_bool(&p.received, 9000u)) goto cleanup;
        }
    }
    if (expect_publish) {
        item.qos = 1u;
        item.topic = (flowie_mqtt_span_t){
            (const uint8_t *)TOPIC, sizeof(TOPIC) - 1u};
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
    if (client && flowie_mqtt_client_try_destroy(client, 5000u) != SALTS_OK)
        return 5;
    if (!rc)
        printf("FLOWIE_ACL_TEST_PASS %s\n", mode);
    else
        fprintf(stderr, "Flowie ACL rejected expected state: %s\n", mode);
    return rc;
}
