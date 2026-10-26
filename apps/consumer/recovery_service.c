/* Isolated HTTPS/mTLS read-only recovery endpoint. NOT a public game gateway.
 * One pinned service principal for the current CI/local qualification.
 * This process never opens a writer connection or settles MQTT/Outbox ACK.
 * The peer SHA256 is sourced only from CHttp's verified TLS request view.
 */
#include <http_server/http.h>
#include "mygames/recovery_gate.h"
#include "mygames/recovery_wire.h"
#include "mygames/room_store.h"
#include "mygames/outbox_dispatch.h"
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_WORK_CAPACITY 8u
#define RECOVERY_MAX_CONNECTIONS 8u
#define RECOVERY_TIMEOUT_MS 5000u

typedef enum recovery_job_stage {
    RECOVERY_WAITING = 0, RECOVERY_READY = 1, RECOVERY_ABORTED = 2
} recovery_job_stage;
struct recovery_app;
typedef struct recovery_job {
    struct recovery_app *app;
    mygames_recovery_grant grant;
    chttp_server_deferred deferred;
    atomic_bool in_use;
    atomic_int stage;
} recovery_job;
typedef struct recovery_app {
    const char *sqlite_driver;
    const char *source_database;
    mygames_recovery_gate gate;
    cmeta_mutex_t gate_lock;
    cmeta_threadpool_t *pool;
    recovery_job jobs[RECOVERY_WORK_CAPACITY];
} recovery_app;

static native_io_backend_kind native_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_server_config recovery_config(void) {
    chttp_server_config c = {0};
    c.host = "127.0.0.1"; /* no accidental public development listener */
    c.port = 0u;
    c.backlog = RECOVERY_MAX_CONNECTIONS;
    c.network.backend = native_backend();
    c.network.connection_capacity = RECOVERY_MAX_CONNECTIONS;
    c.network.command_capacity = 32u;
    c.network.request_capacity = 16u;
    c.network.completion_batch_capacity = 8u;
    c.network.event_capacity = 32u;
    c.network.max_send_bytes = 65536u;
    c.network.receive_buffer_bytes = 4096u;
    c.network.connect_timeout_ms = RECOVERY_TIMEOUT_MS;
    c.network.read_timeout_ms = RECOVERY_TIMEOUT_MS;
    c.network.write_timeout_ms = RECOVERY_TIMEOUT_MS;
    c.network.tls_io_buffer_bytes = 32768u;
    c.network.tls_handshake_timeout_ms = RECOVERY_TIMEOUT_MS;
    c.route_capacity = 1u;
    c.middleware_capacity = 1u;
    c.max_route_middleware_count = 1u;
    c.max_route_param_count = 1u;
    c.max_route_param_bytes = 32u;
    c.max_target_bytes = 256u;
    c.max_header_count = 16u;
    c.max_header_bytes = 4096u;
    c.max_request_body_bytes = 128u;
    c.max_response_header_count = 8u;
    c.max_response_header_bytes = 512u;
    c.max_response_body_bytes = MYGAMES_RECOVERY_RESPONSE_MAX;
    c.max_buffered_response_body_bytes = MYGAMES_RECOVERY_RESPONSE_MAX;
    c.buffer_capacity_bytes = 2u * 1024u * 1024u;
    c.poll_slice_ms = 2u;
    return c;
}

static int small_reply(chttp_server_response *response, unsigned int code) {
    static const char bad[] = "{\"error\":\"bad_request\"}";
    static const char unauth[] = "{\"error\":\"unauthenticated\"}";
    static const char forbidden[] = "{\"error\":\"forbidden\"}";
    static const char limit[] = "{\"error\":\"capacity_or_rate\"}";
    static const char stale[] = "{\"error\":\"cursor_conflict\"}";
    static const char unavailable[] = "{\"error\":\"source_unavailable\"}";
    const char *body = unavailable;
    size_t n = sizeof(unavailable) - 1u;
    if (code == 400u) { body = bad; n = sizeof(bad) - 1u; }
    else if (code == 401u) { body = unauth; n = sizeof(unauth) - 1u; }
    else if (code == 403u) { body = forbidden; n = sizeof(forbidden) - 1u; }
    else if (code == 409u) { body = stale; n = sizeof(stale) - 1u; }
    else if (code == 429u) { body = limit; n = sizeof(limit) - 1u; }
    return chttp_server_reply(response, code, "application/json", body, n);
}

static recovery_job *acquire_job(recovery_app *app) {
    size_t i;
    for (i = 0u; i < RECOVERY_WORK_CAPACITY; ++i) {
        bool expected = false;
        recovery_job *job = &app->jobs[i];
        if (atomic_compare_exchange_strong_explicit(
                &job->in_use, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            memset(&job->grant, 0, sizeof(job->grant));
            job->deferred =
                (chttp_server_deferred)CHTTP_SERVER_DEFERRED_INIT;
            job->app = app;
            atomic_store_explicit(&job->stage, RECOVERY_WAITING,
                                  memory_order_release);
            return job;
        }
    }
    return NULL;
}

static void terminal_job(recovery_job *job) {
    cmeta_mutex_lock(&job->app->gate_lock);
    (void)mygames_recovery_gate_release(&job->app->gate, &job->grant);
    cmeta_mutex_unlock(&job->app->gate_lock);
    memset(&job->grant, 0, sizeof(job->grant));
    atomic_store_explicit(&job->in_use, false, memory_order_release);
}

static void recovery_worker(void *user) {
    recovery_job *job = (recovery_job *)user;
    mygames_store_archive_page page = {0};
    mygames_room_store *source = NULL;
    char body[MYGAMES_RECOVERY_RESPONSE_MAX];
    size_t bytes = 0u;
    unsigned int status_code = 503u;
    int stage;
    int reply_rc;

    do {
        stage = atomic_load_explicit(&job->stage, memory_order_acquire);
        if (stage == RECOVERY_WAITING) cmeta_thread_yield();
    } while (stage == RECOVERY_WAITING);
    if (stage != RECOVERY_READY) {
        terminal_job(job);
        return;
    }

    {
        const mygames_store_status st = mygames_room_store_open_readonly(
            job->app->sqlite_driver, job->app->source_database, &source);
        if (st == MYGAMES_STORE_OK) {
            const mygames_store_status read_status =
                mygames_room_store_read_archive_page(
                    source, job->grant.after_committed_seq,
                    job->grant.page_limit, &page);
            if (read_status == MYGAMES_STORE_STALE_SEQUENCE)
                status_code = 409u;
            else if (read_status == MYGAMES_STORE_OK &&
                     mygames_recovery_encode_page(
                         &job->grant, &page,
                         mygames_outbox_public_event_validate,
                         body, sizeof(body), &bytes) ==
                         MYGAMES_RECOVERY_WIRE_OK)
                status_code = 200u;
        }
    }
    mygames_room_store_close(source);

    if (status_code != 200u) {
        static const char stale[] = "{\"error\":\"cursor_conflict\"}";
        static const char fail[] = "{\"error\":\"source_unavailable\"}";
        const char *error_body = status_code == 409u ? stale : fail;
        bytes = status_code == 409u ? sizeof(stale)-1u : sizeof(fail)-1u;
        memcpy(body, error_body, bytes);
    }

    {
        chttp_server_deferred_response reply = {0};
        reply.size = sizeof(reply);
        reply.status_code = status_code;
        reply.content_type = "application/json";
        reply.body = body;
        reply.body_size = bytes;
        reply_rc = chttp_server_deferred_reply(&job->deferred, &reply);
    }
#ifdef MYGAMES_RECOVERY_TEST_TRACE
    fprintf(stderr, "mygames.recovery: worker status=%u reply_result=%d\\n",
            status_code, reply_rc);
#endif
    if (reply_rc != SALTS_OK)
        (void)chttp_server_deferred_cancel(&job->deferred);
    terminal_job(job);
}

static int recovery_get(void *user, const chttp_server_request_view *request,
                        chttp_server_response *response) {
    recovery_app *app = (recovery_app *)user;
    mygames_recovery_gate_status grant_result;
    mygames_recovery_wire_status parsed;
    recovery_job *job;
    uint64_t after = 0u;
    uint32_t limit = 0u;
    int rc;

    if (!app || !request || !response) return SALTS_EINVAL;
#ifdef MYGAMES_RECOVERY_TEST_TRACE
    fputs("mygames.recovery: HTTP route reached\\n", stderr);
#endif
    if (!request->peer_certificate_sha256)
        return small_reply(response, 401u);
    if (request->body_size != 0u || request->body_streamed)
        return small_reply(response, 400u);
    parsed = mygames_recovery_parse_target(request->target, &after, &limit);
    if (parsed != MYGAMES_RECOVERY_WIRE_OK)
        return small_reply(response, 400u);
    job = acquire_job(app);
    if (!job) return small_reply(response, 429u);

    cmeta_mutex_lock(&app->gate_lock);
    grant_result = mygames_recovery_gate_admit_read(
        &app->gate, request->peer_certificate_sha256,
        after, limit, cmeta_monotonic_ms(), &job->grant);
    cmeta_mutex_unlock(&app->gate_lock);
    if (grant_result != MYGAMES_RECOVERY_GATE_OK) {
        atomic_store_explicit(&job->in_use, false, memory_order_release);
        if (grant_result == MYGAMES_RECOVERY_GATE_UNAUTHENTICATED)
            return small_reply(response, 401u);
        if (grant_result == MYGAMES_RECOVERY_GATE_FORBIDDEN)
            return small_reply(response, 403u);
        if (grant_result == MYGAMES_RECOVERY_GATE_INVALID)
            return small_reply(response, 400u);
        return small_reply(response, 429u);
    }
    rc = cmeta_threadpool_try_submit(app->pool, recovery_worker, job);
    if (rc != SALTS_OK) {
        terminal_job(job);
        return small_reply(response, 429u);
    }
    rc = chttp_server_response_defer(response, &job->deferred);
    if (rc != SALTS_OK) {
        atomic_store_explicit(&job->stage, RECOVERY_ABORTED,
                              memory_order_release);
        return rc; /* worker owns terminal release */
    }
    atomic_store_explicit(&job->stage, RECOVERY_READY, memory_order_release);
    return SALTS_OK;
}

int main(int argc, char **argv) {
    static const char *const h1[] = {"http/1.1"};
    chttp_server server = {0};
    chttp_server_config cfg = recovery_config();
    chttp_server_execution_options owner = CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
    cnet_tls_server_config tls = {0};
    const cmeta_threadpool_config_t pool_cfg = {
        .num_threads = 1u, .queue_capacity = RECOVERY_WORK_CAPACITY};
    mygames_recovery_principal principal = {0};
    recovery_app app = {0};
    uint16_t port = 0u;
    int rc = SALTS_EINVAL;
    int result = EXIT_FAILURE;
    bool initialized = false;
    bool started = false;
    size_t i;

    if (argc != 9) {
        fprintf(stderr, "usage: recovery_service sqlite-driver source-db "
                        "server-cert server-key client-ca pinned-client-sha256 "
                        "source-namespace consumer-id\n");
        return EXIT_FAILURE;
    }
    app.sqlite_driver = argv[1];
    app.source_database = argv[2];
    if (strlen(argv[6]) != 64u ||
        strlen(argv[7]) >= sizeof(principal.source_namespace) ||
        strlen(argv[8]) >= sizeof(principal.consumer_id))
        return EXIT_FAILURE;
    memcpy(principal.peer_certificate_sha256, argv[6], 65u);
    (void)snprintf(principal.source_namespace,
                   sizeof(principal.source_namespace), "%s", argv[7]);
    (void)snprintf(principal.consumer_id,
                   sizeof(principal.consumer_id), "%s", argv[8]);
    principal.allow_read = true;
    principal.rate_per_second = 8u;
    principal.burst = 16u;
    if (mygames_recovery_gate_init(&app.gate, &principal, 1u) !=
        MYGAMES_RECOVERY_GATE_OK)
        return EXIT_FAILURE;
    cmeta_mutex_init(&app.gate_lock);
    if (!app.gate_lock) return EXIT_FAILURE;
    for (i = 0u; i < RECOVERY_WORK_CAPACITY; ++i) {
        atomic_init(&app.jobs[i].in_use, false);
        atomic_init(&app.jobs[i].stage, RECOVERY_ABORTED);
    }
    app.pool = cmeta_threadpool_create_with_config(&pool_cfg);
    if (!app.pool) goto cleanup;

    /* Enforce mTLS before any HTTP route; never downgrade to plaintext.
     * CA verifies certificate chain/purpose; RecoveryGate pins the verified
     * leaf digest to exactly one service role + namespace + consumer. */
    tls.size = sizeof(tls);
    tls.cert_file = argv[3];
    tls.key_file = argv[4];
    tls.ca_file = argv[5];
    tls.client_auth = CNET_TLS_CLIENT_AUTH_REQUIRED;
    tls.alpn_protocols = h1;
    tls.alpn_protocol_count = 1u;
    cfg.tls = &tls;
    rc = chttp_server_init(&server, &cfg);
    if (rc != SALTS_OK) goto cleanup;
    initialized = true;
    owner.owner_count = 1u;
    rc = chttp_server_set_execution_options(&server, &owner);
    if (rc == SALTS_OK)
        rc = chttp_server_get(&server, MYGAMES_RECOVERY_PATH,
                              recovery_get, &app);
    if (rc == SALTS_OK) rc = chttp_server_start(&server);
    if (rc != SALTS_OK) goto cleanup;
    started = true;
    if (chttp_server_port(&server, &port) != SALTS_OK || port == 0u)
        goto cleanup;
    printf("MYGAMES_RECOVERY_TLS_PORT=%u\n", (unsigned int)port);
    fflush(stdout);
    (void)getchar(); /* local test/admin shutdown only */
    result = EXIT_SUCCESS;

cleanup:
    if (started) {
        if (chttp_server_stop(&server, RECOVERY_TIMEOUT_MS) != SALTS_OK)
            return EXIT_FAILURE; /* do not free worker-owned deferred state */
    }
    if (app.pool) {
        const int shutdown = cmeta_threadpool_shutdown_with_policy(
            app.pool, SALTS_THREADPOOL_SHUTDOWN_DRAIN);
        const int idle = shutdown == SALTS_OK
            ? cmeta_threadpool_wait_status(app.pool) : shutdown;
        if (idle != SALTS_OK) return EXIT_FAILURE;
        cmeta_threadpool_destroy(app.pool);
    }
    if (initialized && chttp_server_destroy(&server) != SALTS_OK)
        result = EXIT_FAILURE;
    cmeta_mutex_destroy(&app.gate_lock);
    return result;
}
