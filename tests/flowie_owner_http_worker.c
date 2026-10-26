#include "flowie_owner_http_worker.h"
#include "mygames/recovery_wire.h"
#include <http_client/http.h>
#include <salts/error_codes.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { OWNER_HTTP_TIMEOUT_MS = 7000u };

static native_io_backend_kind owner_http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}
static int same_ci(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) return 0;
    while (*lhs && *rhs) {
        unsigned char a = (unsigned char)*lhs++;
        unsigned char b = (unsigned char)*rhs++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 'a' - 'A');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 'a' - 'A');
        if (a != b) return 0;
    }
    return !*lhs && !*rhs;
}
static int exact_json_content_type(const chttp_response *response) {
    size_t i, matches = 0u;
    if (!response || !response->headers) return 0;
    for (i = 0u; i < response->header_count; ++i) {
        if (same_ci(response->headers[i].name, "Content-Type")) {
            if (!response->headers[i].value ||
                strcmp(response->headers[i].value, "application/json") != 0)
                return 0;
            ++matches;
        }
    }
    return matches == 1u;
}

static void native_http_worker(void *user) {
    static const char *const alpn[] = {"http/1.1"};
    mygames_test_owner_http *job = (mygames_test_owner_http *)user;
    cnet_tls_client_config tls_config = {0};
    chttp_tls_profile profile = {0};
    chttp_client_config cfg = {0};
    chttp_client client = {0};
    chttp_options opts = {0};
    chttp_response response = {0};
    chttp_error error = {0};
    mygames_owner_ticket tickets[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
    char uri[128];
    char target[180];
    size_t count = 0u;
    int profile_ready = 0, client_ready = 0, rc = SALTS_EINVAL;

    if (!job || !job->owner || !job->ca_file || !job->client_cert ||
        !job->client_key || !job->port ||
        job->committed_after > (uint64_t)INT64_MAX)
        goto done;
    if (snprintf(uri, sizeof(uri), "tls://127.0.0.1:%u",
                 (unsigned)job->port) <= 0 ||
        snprintf(target, sizeof(target),
                 MYGAMES_RECOVERY_PATH "?after=%" PRIu64 "&limit=1",
                 job->committed_after) <= 0)
        goto done;

    tls_config.size = sizeof(tls_config);
    tls_config.ca_file = job->ca_file;
    tls_config.cert_file = job->client_cert;
    tls_config.key_file = job->client_key;
    tls_config.server_name = "localhost";
    tls_config.alpn_protocols = alpn;
    tls_config.alpn_protocol_count = 1u;

    cfg.network.backend = owner_http_backend();
    cfg.network.connection_capacity = 2u;
    cfg.network.command_capacity = 32u;
    cfg.network.request_capacity = 8u;
    cfg.network.completion_batch_capacity = 4u;
    cfg.network.event_capacity = 32u;
    cfg.network.max_send_bytes = 4096u;
    cfg.network.receive_buffer_bytes = 4096u;
    cfg.network.tls_io_buffer_bytes = 32768u;
    cfg.network.tls_handshake_timeout_ms = OWNER_HTTP_TIMEOUT_MS;
    cfg.network.connect_timeout_ms = OWNER_HTTP_TIMEOUT_MS;
    cfg.network.read_timeout_ms = OWNER_HTTP_TIMEOUT_MS;
    cfg.network.write_timeout_ms = OWNER_HTTP_TIMEOUT_MS;
    cfg.request_capacity = 2u;
    cfg.max_start_line_bytes = 256u;
    cfg.max_header_count = 16u;
    cfg.max_header_bytes = 4096u;
    cfg.max_request_body_bytes = 128u;
    cfg.max_response_body_bytes = MYGAMES_RECOVERY_RESPONSE_MAX;
    cfg.max_informational_responses = 2u;

    if (chttp_tls_profile_init(&profile, &tls_config) != SALTS_OK)
        goto done;
    profile_ready = 1;
    if (chttp_client_init(&client, &cfg) != SALTS_OK)
        goto done;
    client_ready = 1;

    opts.connection_uri = uri;
    opts.authority = "localhost";
    opts.target = target;
    opts.timeout_ms = OWNER_HTTP_TIMEOUT_MS;
    opts.tls = &profile;

    /* Flowie callbacks may run while chttp_get() is in flight. No path
     * here can touch the borrowed ConsumerProjection. */
    atomic_store_explicit(&job->in_flight, true, memory_order_release);
    rc = chttp_get(&client, &opts, &response, &error);
    if (rc != SALTS_OK || response.status_code != 200u ||
        !response.body || response.body_size == 0u ||
        response.body_size > MYGAMES_RECOVERY_RESPONSE_MAX ||
        !exact_json_content_type(&response)) {
        fprintf(stderr,
            "owner-http: TLS/HTTP failure request=%d response=%u\n",
            rc, response.status_code);
        goto done;
    }

    if (mygames_consumer_owner_admit_history(
            job->owner, response.body, response.body_size,
            job->committed_after,
            tickets, MYGAMES_STORE_ARCHIVE_PAGE_MAX,
            &count) != MYGAMES_OWNER_OK || count != 1u) {
        fprintf(stderr, "owner-http: invalid/refused canonical page\n");
        goto done;
    }
    job->admitted = count;
    job->successful = true;
done:
    chttp_response_destroy(&response);
    if (client_ready &&
        chttp_client_destroy(&client, OWNER_HTTP_TIMEOUT_MS) != SALTS_OK)
        job->successful = false;
    if (profile_ready && chttp_tls_profile_destroy(&profile) != SALTS_OK)
        job->successful = false;
    /* HTTP owns no durable effect. Release its generation retain only AFTER
     * the CHttp client, TLS profile and response have been quiesced, so a
     * concurrent close/destroy cannot invalidate Owner state too early. */
    if (job->fetch_owned) {
        if (mygames_consumer_owner_finish_fetch(
                job->owner, job->fetch_ticket) != MYGAMES_OWNER_OK)
            job->successful = false;
        job->fetch_owned = false;
    }
    atomic_store_explicit(&job->in_flight, false, memory_order_release);
}

int mygames_test_owner_http_start(mygames_test_owner_http *job) {
    if (!job || !job->owner || job->started) return SALTS_EINVAL;
    atomic_init(&job->in_flight, false);
    job->successful = false;
    job->admitted = 0u;
    job->fetch_owned = false;
    memset(&job->fetch_ticket, 0, sizeof(job->fetch_ticket));
    if (mygames_consumer_owner_begin_fetch(
            job->owner, &job->fetch_ticket) != MYGAMES_OWNER_OK)
        return SALTS_EINVAL; /* no socket/ledger access on second worker */
    job->fetch_owned = true;
    if (job->fetch_ticket.committed_after != job->committed_after) {
        (void)mygames_consumer_owner_finish_fetch(
            job->owner, job->fetch_ticket);
        job->fetch_owned = false;
        return SALTS_EINVAL; /* stale GET cursor: no implicit retry */
    }
    if (cmeta_thread_create(&job->thread, native_http_worker, job) != SALTS_OK) {
        (void)mygames_consumer_owner_finish_fetch(
            job->owner, job->fetch_ticket);
        job->fetch_owned = false;
        return SALTS_EINVAL;
    }
    job->started = true;
    return SALTS_OK;
}
int mygames_test_owner_http_join(mygames_test_owner_http *job) {
    int rc;
    if (!job || !job->started) return SALTS_EINVAL;
    rc = cmeta_thread_join(&job->thread);
    cmeta_thread_destroy(&job->thread);
    job->started = false;
    if (rc != SALTS_OK || !job->successful || job->admitted != 1u)
        return SALTS_EPROTO;
    return SALTS_OK;
}
