/* A real Flowie::Flowie in-process MQTT broker test fixture, loopback
 * only. No auth and no durable broker data: never install/deploy this binary.
 * Production ACL and TLS must be qualified against the deployed service.
 */
#include <flowie.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int on_message(flowie_endpoint_core_t *endpoint, flowie_message_t *message,
                      flowie_publish_result_t *result, void *user) {
    int rc;
    (void)user;
    if (!endpoint || !message || !result ||
        result->size < sizeof(*result)) return SALTS_EINVAL;
    rc = flowie_endpoint_core_send_message(endpoint, message);
    result->status = rc;
    if (rc == SALTS_OK) {
        result->protocol_settlement = FLOWIE_PROTOCOL_SETTLE_ACCEPTED;
        puts("BROKER_EVENT_ACCEPTED");
        fflush(stdout);
    }
    return rc;
}

int main(void) {
    flowie_endpoint_config_t config = FLOWIE_ENDPOINT_CONFIG_INIT;
    flowie_endpoint_core_options_t options = FLOWIE_ENDPOINT_CORE_OPTIONS_INIT;
    flowie_endpoint_core_t *endpoint = NULL;
    int status;
    config.host = "127.0.0.1";
    config.port = 18893;
    config.manage_sessions = 1;
    config.max_connections = 16u;
    config.max_sessions = 16u;
    config.max_packet_size = 8192u;
    config.max_subscriptions_per_session = 8u;
    config.max_inflight_per_session = 8u;
    config.reuse_port = 1;
    options.on_message = on_message;
    status = flowie_endpoint_core_create("mygames-ci-local-mqtt", &config,
                                         &options, &endpoint);
    if (status != SALTS_OK || !endpoint) {
        fprintf(stderr, "Flowie endpoint create: %d\n", status);
        return 1;
    }
    status = flowie_endpoint_core_start(endpoint);
    if (status != SALTS_OK) {
        fprintf(stderr, "Flowie endpoint start: %d\n", status);
        flowie_endpoint_core_destroy(endpoint);
        return 1;
    }
    puts("FLOWIE_BROKER_READY");
    fflush(stdout);
    (void)getchar();
    status = flowie_endpoint_core_stop(endpoint);
    flowie_endpoint_core_destroy(endpoint);
    return status == SALTS_OK ? 0 : 1;
}
