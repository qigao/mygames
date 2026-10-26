/* Real loopback Flowie MQTT broker test fixture. Security mode is intentionally
 * test-only, activated with MYGAMES_TEST_BROKER_SECURE. The three passwords
 * MUST come from the CI parent process, never a checked-in literal.
 * Production broker identity and ACL policy must be provisioned separately.
 */
#include <flowie.h>
#include <salts/error_codes.h>
#include <orm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_POLICY_VERSION UINT64_C(7)
#define INTERNAL_TOPIC "mygames/internal/v1/room-events"

static int test_equal_secret(const uint8_t *secret, size_t len, const char *expected) {
    size_t i, n = expected ? strlen(expected) : 0u;
    volatile uint8_t diff = 0u;
    if (!secret || !expected || len != n || n < 24u || n > 128u) return 0;
    for (i = 0u; i < len; ++i) diff |= (uint8_t)(secret[i] ^ (uint8_t)expected[i]);
    return diff == 0u;
}

static int test_authenticate(void *ctx, const flowie_security_auth_request_t *req,
                             flowie_security_principal_t *out) {
    const char *env = NULL;
    const char *type = NULL;
    flowie_security_principal_t p = {0};
    (void)ctx;
    if (!req || !req->identity || !req->method || !out ||
        strcmp(req->method, "password") != 0)
        return SALTS_EPERM;
    if (strcmp(req->identity, "svc-outbox") == 0) {
        env = getenv("MYGAMES_TEST_PUBLISHER_SECRET");
        type = "service";
    } else if (strcmp(req->identity, "svc-consumer") == 0) {
        env = getenv("MYGAMES_TEST_CONSUMER_SECRET");
        type = "service";
    } else if (strcmp(req->identity, "player-browser") == 0) {
        env = getenv("MYGAMES_TEST_PLAYER_SECRET");
        type = "player";
    } else return SALTS_EPERM;
    if (!test_equal_secret(req->secret, req->secret_size, env))
        return SALTS_EPERM;
    p.size = sizeof(p);
    p.abi_version = FLOWIE_SECURITY_ABI_V3;
    p.scope = FLOWIE_SECURITY_SCOPE_SELF;
    p.policy_version = TEST_POLICY_VERSION;
    (void)snprintf(p.principal_id, sizeof(p.principal_id), "%s", req->identity);
    (void)snprintf(p.principal_type, sizeof(p.principal_type), "%s", type);
    (void)snprintf(p.auth_method, sizeof(p.auth_method), "%s", "password");
    (void)snprintf(p.domain_id, sizeof(p.domain_id), "%s", "mygames");
    *out = p;
    return SALTS_OK;
}

static void allow_rule(flowie_security_rule_t *rule, const char *principal,
                       uint32_t action, flowie_security_resource_type_t resource,
                       const char *exact_pattern) {
    memset(rule, 0, sizeof(*rule));
    rule->size = sizeof(*rule);
    rule->abi_version = FLOWIE_SECURITY_ABI_V3;
    rule->effect = FLOWIE_SECURITY_ALLOW;
    rule->subject_kind = FLOWIE_SECURITY_SUBJECT_PRINCIPAL;
    rule->action_mask = action;
    rule->resource_type = resource;
    rule->match_kind = FLOWIE_SECURITY_MATCH_EXACT;
    (void)snprintf(rule->domain_id, sizeof(rule->domain_id), "%s", "mygames");
    (void)snprintf(rule->subject, sizeof(rule->subject), "%s", principal);
    (void)snprintf(rule->pattern, sizeof(rule->pattern), "%s", exact_pattern);
}

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
    const int secure = getenv("MYGAMES_TEST_BROKER_SECURE") != NULL;
    flowie_security_rule_t rules[5] = {{0}};
    flowie_security_realm_config_t realm_config = FLOWIE_SECURITY_REALM_CONFIG_INIT;
    flowie_security_realm_t *realm = NULL;
    flowie_security_auth_provider_t auth = {0};
    flowie_endpoint_security_binding_t security = FLOWIE_ENDPOINT_SECURITY_BINDING_INIT;
    flowie_endpoint_bindings_t bindings = FLOWIE_ENDPOINT_BINDINGS_INIT;
    flowie_execution_binding_t execution = FLOWIE_EXECUTION_BINDING_INIT;
    flowie_protocol_repository_t *repository = NULL;
    flowie_protocol_repository_config_t repository_config = {0};
    flowie_endpoint_persistence_binding_t persistence =
        FLOWIE_ENDPOINT_PERSISTENCE_BINDING_INIT;
    orm_config_t database_config;
    orm_option_t database_options[2] = {{0}};
    const char *broker_database = getenv("MYGAMES_TEST_BROKER_DB");
    const char *broker_driver = getenv("MYGAMES_TEST_BROKER_DRIVER");
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

    /* Test-only disk-backed protocol facts, separate from authoritative
     * Room Outbox and ConsumerProjection. Requires explicit absolute paths;
     * refusing a missing driver avoids implicit native-plugin fallback. */
    if (broker_database || broker_driver) {
        if (!secure || !broker_database || !broker_driver ||
            broker_database[0] != '/' || broker_driver[0] != '/') {
            fputs("Invalid disk-backed Broker fixture configuration\n", stderr);
            return 2;
        }
        orm_config(&database_config);
        database_options[0].keyword = orm_view("filename");
        database_options[0].value = orm_view(broker_database);
        database_options[1].keyword = orm_view("flowie.driver_module");
        database_options[1].value = orm_view(broker_driver);
        database_config.driver = orm_view("sqlite");
        database_config.options = database_options;
        database_config.option_count = 2u;
        repository_config.size = sizeof(repository_config);
        repository_config.database = &database_config;
        repository_config.namespace_name = "mygames_ci_broker";
        repository_config.create_schema = 1;
        repository_config.limits.max_sessions = config.max_sessions;
        repository_config.limits.max_subscriptions_per_session =
            config.max_subscriptions_per_session;
        repository_config.limits.max_inflight_per_session =
            config.max_inflight_per_session;
        repository_config.limits.max_retained_messages = 16u;
        repository_config.limits.max_client_id_size = 128u;
        repository_config.limits.max_topic_size = 512u;
        repository_config.limits.max_packet_size = config.max_packet_size;
        status = flowie_protocol_repository_open(&repository_config, &repository);
        if (status != SALTS_OK || !repository) {
            fprintf(stderr, "Disk-backed Broker repository open failed: %d\n", status);
            return 1;
        }
        persistence.repository = repository;
        bindings.persistence = &persistence;
    }

    if (secure) {
        if (!getenv("MYGAMES_TEST_PUBLISHER_SECRET") ||
            !getenv("MYGAMES_TEST_CONSUMER_SECRET") ||
            !getenv("MYGAMES_TEST_PLAYER_SECRET")) {
            fputs("Missing test broker secrets\n", stderr);
            return 2;
        }
        allow_rule(&rules[0], "svc-outbox", FLOWIE_SECURITY_ACTION_CONNECT,
                   FLOWIE_SECURITY_RESOURCE_GENERIC, "mygames-outbox-v1");
        allow_rule(&rules[1], "svc-outbox", FLOWIE_SECURITY_ACTION_PUBLISH,
                   FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC, INTERNAL_TOPIC);
        allow_rule(&rules[2], "svc-consumer", FLOWIE_SECURITY_ACTION_CONNECT,
                   FLOWIE_SECURITY_RESOURCE_GENERIC, "mygames-consumer-v1");
        allow_rule(&rules[3], "svc-consumer", FLOWIE_SECURITY_ACTION_SUBSCRIBE,
                   FLOWIE_SECURITY_RESOURCE_MQTT_TOPIC, INTERNAL_TOPIC);
        allow_rule(&rules[4], "player-browser", FLOWIE_SECURITY_ACTION_CONNECT,
                   FLOWIE_SECURITY_RESOURCE_GENERIC, "mygames-player-v1");
        realm_config.policy_version = TEST_POLICY_VERSION;
        realm_config.rules = rules;
        realm_config.rule_count = sizeof(rules) / sizeof(rules[0]);
        status = flowie_security_realm_create(&realm_config, &realm);
        if (status != SALTS_OK || !realm) {
            fprintf(stderr, "Flowie security realm create: %d\n", status);
            flowie_protocol_repository_close(repository);
            return 1;
        }
        auth.size = sizeof(auth);
        auth.authenticate = test_authenticate;
        security.realm_channel = "mygames.security";
        security.auth_method = "password";
        security.auth_provider = &auth;
        security.realm = realm;
        bindings.security = &security;
        status = flowie_endpoint_core_create_ex(
            "mygames-ci-secure-mqtt", &config, &options,
            &execution, &bindings, &endpoint);
    } else {
        status = flowie_endpoint_core_create(
            "mygames-ci-local-mqtt", &config, &options, &endpoint);
    }
    if (status != SALTS_OK || !endpoint) {
        fprintf(stderr, "Flowie endpoint create: %d\n", status);
        flowie_security_realm_destroy(realm);
        flowie_protocol_repository_close(repository);
        return 1;
    }
    status = flowie_endpoint_core_start(endpoint);
    if (status != SALTS_OK) {
        fprintf(stderr, "Flowie endpoint start: %d\n", status);
        flowie_endpoint_core_destroy(endpoint);
        flowie_security_realm_destroy(realm);
        flowie_protocol_repository_close(repository);
        return 1;
    }
    puts(secure ? "FLOWIE_SECURE_BROKER_READY" : "FLOWIE_BROKER_READY");
    fflush(stdout);
    (void)getchar();
    status = flowie_endpoint_core_stop(endpoint);
    flowie_endpoint_core_destroy(endpoint);
    flowie_security_realm_destroy(realm);
    flowie_protocol_repository_close(repository);
    return status == SALTS_OK ? 0 : 1;
}
