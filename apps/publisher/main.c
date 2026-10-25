/* Development Flowie MQTT Outbox publisher; separate from the CFlow Room
 * Owner. Never re-enter RoomDirectory, and never execute SQL from a Flowie
 * callback. Flowie::Client owns the network worker and command lifetime.
 *
 * Usage: publisher sqlite-driver-path database-path [--once|--drain|--watch]
 * Environment: MYGAMES_MQTT_PORT (default 18893). TCP loopback ONLY.
 */
#include "mygames/outbox_dispatch.h"
#include <flowie_mqtt_client.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/error_codes.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

typedef struct publisher {
    flowie_mqtt_client_t *client;
    atomic_bool connect_done;
    atomic_bool connect_success;
    atomic_bool publish_done;
    atomic_bool publish_success;
    atomic_int network_error;
} publisher;

static volatile sig_atomic_t publisher_stop = 0;
static void stop_signal(int code) { (void)code; publisher_stop = 1; }

static bool complete_before(atomic_bool *flag, uint64_t budget_ms) {
    const uint64_t now = cmeta_monotonic_ms();
    const uint64_t end = now + budget_ms;
    if (end < now) return false;
    while (!atomic_load_explicit(flag, memory_order_acquire) &&
           cmeta_monotonic_ms() < end && !publisher_stop)
        cmeta_sleep_ms(5u);
    return atomic_load_explicit(flag, memory_order_acquire);
}
static void on_connect(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response,
                       void *user) {
    publisher *pub = (publisher *)user;
    (void)client;
    atomic_store_explicit(&pub->connect_success,
        status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_CONNACK &&
        response->version == FLOWIE_MQTT_VERSION_5 &&
        response->reason_code == 0u, memory_order_relaxed);
    atomic_store_explicit(&pub->connect_done, true, memory_order_release);
}
static void on_publish(flowie_mqtt_client_t *client, int status,
                       const flowie_mqtt_control_packet_view_t *response,
                       void *user) {
    publisher *pub = (publisher *)user;
    const bool confirmed = status == SALTS_OK && response &&
        response->type == FLOWIE_MQTT_PACKET_PUBACK &&
        response->version == FLOWIE_MQTT_VERSION_5 &&
        response->reason_code < 0x80u;
    (void)client;
    atomic_store_explicit(&pub->publish_success, confirmed, memory_order_relaxed);
    atomic_store_explicit(&pub->publish_done, true, memory_order_release);
}
static void on_error(flowie_mqtt_client_t *client, int status, void *user) {
    publisher *pub = (publisher *)user;
    (void)client;
    atomic_store_explicit(&pub->network_error,
                          status ? status : SALTS_EIO, memory_order_release);
}

/* Flowie client performs a deep copy when the publish command is admitted.
 * Return SETTLED only after the asynchronous MQTT PUBACK completion. */
static mygames_outbox_delivery publish_qos1(
    void *context, const char *topic_name, const uint8_t *payload,
    size_t payload_size, uint64_t epoch, uint64_t sequence) {
    publisher *pub = (publisher *)context;
    flowie_mqtt_client_publish_topic_t topic = {0};
    flowie_mqtt_client_publish_topic_vec_t topics =
        FLOWIE_MQTT_CLIENT_PUBLISH_TOPIC_VEC_INIT;
    int rc;
    (void)epoch; (void)sequence;
    if (!pub || !pub->client || !topic_name || !payload ||
        !payload_size || payload_size > MYGAMES_STORE_EVENT_MAX)
        return MYGAMES_OUTBOX_DELIVERY_FATAL;
    if (!flowie_mqtt_client_is_connected(pub->client))
        return MYGAMES_OUTBOX_DELIVERY_RETRY;
    atomic_store_explicit(&pub->publish_done, false, memory_order_release);
    atomic_store_explicit(&pub->publish_success, false, memory_order_relaxed);
    topic.qos = 1u;
    topic.retain = 0u; /* Outbox is source of truth, not MQTT retained topics */
    topic.topic = (flowie_mqtt_span_t){
        (const uint8_t *)topic_name, strlen(topic_name)};
    topic.payload = (flowie_mqtt_span_t){payload, payload_size};
    topics.version = FLOWIE_MQTT_VERSION_5;
    topics.count = 1u;
    topics.data = &topic;
    rc = flowie_mqtt_client_publish(pub->client, &topics);
    if (rc != SALTS_OK) return MYGAMES_OUTBOX_DELIVERY_RETRY;
    if (!complete_before(&pub->publish_done, 5000u))
        return MYGAMES_OUTBOX_DELIVERY_RETRY;
    if (!atomic_load_explicit(&pub->publish_success, memory_order_acquire))
        return MYGAMES_OUTBOX_DELIVERY_RETRY;
#ifdef MYGAMES_OUTBOX_TEST_FAULTS
    if (getenv("MYGAMES_TEST_CRASH_AFTER_PUBACK"))
        _Exit(77); /* test only: force redelivery when PUBACK preceded DB ACK */
#endif
    return MYGAMES_OUTBOX_DELIVERY_SETTLED;
}

static bool valid_port(const char *arg, int *out) {
    char *end = NULL;
    long n;
    if (!arg || !*arg || !out) return false;
    n = strtol(arg, &end, 10);
    if (!end || *end != '\0' || n < 1 || n > 65535) return false;
    *out = (int)n;
    return true;
}
static int publisher_main(int argc, char **argv) {
    flowie_mqtt_client_config_t cfg = FLOWIE_MQTT_CLIENT_CONFIG_INIT;
    flowie_mqtt_connect_packet_t conn = FLOWIE_MQTT_CONNECT_PACKET_INIT;
    publisher pub = {0};
    mygames_room_store *store = NULL;
    const char *port_override = getenv("MYGAMES_MQTT_PORT");
    const char *const client_id = "mygames-outbox-v1";
    const char *const user = getenv("MYGAMES_MQTT_USER");
    const char *const password = getenv("MYGAMES_MQTT_PASSWORD");
    int port = 18893, rc, exit_status = 1;
    bool drain, watch;
    unsigned int sent_count = 0u;
    mygames_store_status storage;
    uint64_t last_sequence = 0u;
    if (argc != 4) {
        fprintf(stderr, "usage: %s sqlite-driver database [--once|--drain|--watch]\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[3], "--once") != 0 &&
        strcmp(argv[3], "--drain") != 0 &&
        strcmp(argv[3], "--watch") != 0) return 2;
    if (port_override && !valid_port(port_override, &port)) {
        fprintf(stderr, "MYGAMES_MQTT_PORT must be 1..65535\n");
        return 2;
    }
    if ((user && *user) != (password && *password)) {
        fprintf(stderr, "MQTT username and password must both be provided\n");
        return 2;
    }
    drain = strcmp(argv[3], "--drain") == 0;
    watch = strcmp(argv[3], "--watch") == 0;
    storage = mygames_room_store_open(argv[1], argv[2], &store);
    if (storage != MYGAMES_STORE_OK || !store) {
        fprintf(stderr, "TurboDB Outbox open failed: %d\n", (int)storage);
        return 3;
    }
    atomic_init(&pub.connect_done, false);
    atomic_init(&pub.connect_success, false);
    atomic_init(&pub.publish_done, false);
    atomic_init(&pub.publish_success, false);
    atomic_init(&pub.network_error, SALTS_OK);
    cfg.transport = FLOWIE_MQTT_CLIENT_TRANSPORT_TCP;
    cfg.host = "127.0.0.1"; /* never send credentials over public plaintext TCP */
    cfg.port = port;
    cfg.timeout_ms = 4000u;
    cfg.command_queue_capacity = 2u;
    cfg.command_queue_max_bytes = 8192u;
    cfg.max_packet_size = 8192u;
    cfg.on_connect = on_connect;
    cfg.on_publish = on_publish;
    cfg.on_error = on_error;
    cfg.user_data = &pub;
    rc = flowie_mqtt_client_create(&cfg, &pub.client);
    if (rc != SALTS_OK || !pub.client) {
        fprintf(stderr, "Flowie MQTT client init failed: %d\n", rc);
        goto cleanup;
    }
    conn.version = FLOWIE_MQTT_VERSION_5;
    conn.clean_start = 1u;
    conn.keep_alive = 10u;
    conn.client_id = (flowie_mqtt_span_t){
        (const uint8_t *)client_id, strlen(client_id)};
    if (user && *user) {
        conn.has_username = 1u;
        conn.has_password = 1u;
        conn.username = (flowie_mqtt_span_t){
            (const uint8_t *)user, strlen(user)};
        conn.password = (flowie_mqtt_span_t){
            (const uint8_t *)password, strlen(password)};
    }
    rc = flowie_mqtt_client_connect(pub.client, &conn);
    if (rc != SALTS_OK || !complete_before(&pub.connect_done, 6000u) ||
        !atomic_load_explicit(&pub.connect_success, memory_order_acquire)) {
        fprintf(stderr, "Flowie MQTT CONNECT not acknowledged; Outbox retained\n");
        exit_status = 4;
        goto cleanup;
    }
    while (!publisher_stop) {
        const mygames_outbox_result step = mygames_outbox_dispatch_one(
            store, publish_qos1, &pub, &last_sequence);
        if (step == MYGAMES_OUTBOX_SENT) {
            ++sent_count;
            if (!drain && !watch) { exit_status = 0; break; }
            continue;
        }
        if (step == MYGAMES_OUTBOX_EMPTY) {
            if (watch) { cmeta_sleep_ms(250u); continue; }
            exit_status = 0;
            break;
        }
        if (step == MYGAMES_OUTBOX_RETRY) {
            fprintf(stderr, "No definitive QoS1 PUBACK; event remains in Outbox\n");
            exit_status = 5;
        } else {
            fprintf(stderr, "Outbox integrity/storage/settlement failure\n");
            exit_status = 6;
        }
        break;
    }
    if (publisher_stop) exit_status = 0;
cleanup:
    /* No callback may retain stack-owned publisher after this boundary.
     * A stuck Flowie worker is fatal to this process, not freed under it. */
    if (pub.client) {
        rc = flowie_mqtt_client_try_destroy(pub.client, 5000u);
        if (rc != SALTS_OK) {
            fprintf(stderr, "Flowie client failed to quiesce, terminating\n");
            _Exit(7);
        }
    }
    mygames_room_store_close(store);
    if (!exit_status)
        fprintf(stdout, "mygames.flowie_outbox: settled=%u last_seq=%" PRIu64 "\n",
                sent_count, last_sequence);
    return exit_status;
}
int main(int argc, char **argv) {
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    return publisher_main(argc, argv);
}
