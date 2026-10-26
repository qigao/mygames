/* M4.3 qualification-only, single-owner remote recovery consumer.
 * Consumes authenticated CHttp TLS GET pages and commits canonical event
 * receipts/effects/cursor into its OWN TurboDB SQLite ledger.
 * It NEVER accepts a source database filename, sends MQTT PUBACK, or
 * advances a cursor because a page was merely read from a socket.
 */
#include "mygames/recovery_consumer.h"
#include "mygames/recovery_wire.h"
#include "mygames/outbox_dispatch.h"

#include <http_client/http.h>
#include <salts/error_codes.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_REMOTE_TIMEOUT_MS 7000u
#define RECOVERY_REMOTE_MAX_PAGES 16u

static native_io_backend_kind backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static int decimal(const char *text, uint32_t cap, uint32_t *out) {
    uint64_t n = 0u;
    const unsigned char *p = (const unsigned char *)text;
    if (!text || !text[0] || !out) return 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        if (n > ((uint64_t)cap - (uint64_t)(*p - '0')) / 10u)
            return 0;
        n = n * 10u + (uint64_t)(*p - '0');
    }
    if (n == 0u || n > cap) return 0;
    *out = (uint32_t)n;
    return 1;
}

static int equal_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++;
        unsigned char y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a'-'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a'-'A'));
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

static int json_content_type(const chttp_response *response) {
    size_t i;
    size_t count = 0u;
    if (!response || !response->headers) return 0;
    for (i = 0u; i < response->header_count; ++i) {
        if (equal_ci(response->headers[i].name, "Content-Type")) {
            if (!response->headers[i].value ||
                strcmp(response->headers[i].value, "application/json") != 0)
                return 0;
            ++count;
        }
    }
    return count == 1u; /* no ambiguous duplicate Content-Type */
}

/* A single exact attempt. 429 / future cursor / broker outage / unavailable
 * archive are terminal to this run, requiring a supervisor decision. */
static int fetch_page(chttp_client *client, const chttp_tls_profile *tls,
                      const char *uri, uint64_t durable_after,
                      uint32_t page_limit, chttp_response *response) {
    char target[160];
    chttp_options options = {0};
    chttp_error error = {0};
    int n, rc;

    if (!client || !tls || !uri || !response ||
        durable_after > (uint64_t)INT64_MAX)
        return SALTS_EINVAL;
    n = snprintf(target, sizeof(target),
                 MYGAMES_RECOVERY_PATH "?after=%" PRIu64 "&limit=%" PRIu32,
                 durable_after, page_limit);
    if (n < 1 || (size_t)n >= sizeof(target))
        return SALTS_EINVAL;
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = target;
    options.timeout_ms = RECOVERY_REMOTE_TIMEOUT_MS;
    options.tls = tls;
    rc = chttp_get(client, &options, response, &error);
    if (rc != SALTS_OK) {
        fprintf(stderr, "remote recovery: TLS/HTTP request failed status=%d\n", rc);
        return rc;
    }
    if (response->status_code != 200u || !response->body ||
        response->body_size == 0u ||
        response->body_size > MYGAMES_RECOVERY_RESPONSE_MAX ||
        !json_content_type(response)) {
        fprintf(stderr, "remote recovery: source denied or malformed status=%u\n",
                response->status_code);
        return SALTS_EPROTO;
    }
    return SALTS_OK;
}

int main(int argc, char **argv) {
    static const char *const alpn[] = {"http/1.1"};
    mygames_consumer_projection *ledger = NULL;
    cnet_tls_client_config tls_cfg = {0};
    chttp_tls_profile profile = {0};
    chttp_client client = {0};
    chttp_client_config cfg = {0};
    char uri[128];
    uint32_t port = 0u, limit = 0u, pages = 0u;
    uint32_t j;
    int tls_ready = 0, client_ready = 0;
    int result = EXIT_FAILURE;
    const char *mode = "normal";

    if (argc != 11 && argc != 12) {
        fputs("usage: remote-consumer sqlite-driver consumer-db TLS-port "
              "ca.pem client.pem client.key source-namespace consumer-id "
              "page-limit max-pages [normal|before-commit|commit-one]\n", stderr);
        return 2;
    }
    if (argc == 12) mode = argv[11];
    if (strcmp(mode, "normal") != 0 &&
        strcmp(mode, "before-commit") != 0 &&
        strcmp(mode, "commit-one") != 0)
        return 2;
    if (!decimal(argv[3], UINT16_MAX, &port) ||
        !decimal(argv[9], MYGAMES_STORE_ARCHIVE_PAGE_MAX, &limit) ||
        !decimal(argv[10], RECOVERY_REMOTE_MAX_PAGES, &pages))
        return 2;
    if (snprintf(uri, sizeof(uri), "tls://127.0.0.1:%" PRIu32, port) < 1 ||
        strlen(uri) >= sizeof(uri))
        return 2;

    if (mygames_consumer_projection_open(argv[1], argv[2], &ledger) !=
        MYGAMES_CONSUMER_APPLIED)
        goto done;
    tls_cfg.size = sizeof(tls_cfg);
    tls_cfg.ca_file = argv[4];
    tls_cfg.cert_file = argv[5];
    tls_cfg.key_file = argv[6];
    tls_cfg.server_name = "localhost";
    tls_cfg.alpn_protocols = alpn;
    tls_cfg.alpn_protocol_count = 1u;
    if (chttp_tls_profile_init(&profile, &tls_cfg) != SALTS_OK)
        goto done;
    tls_ready = 1;

    cfg.network.backend = backend();
    cfg.network.connection_capacity = 2u;
    cfg.network.command_capacity = 32u;
    cfg.network.request_capacity = 8u;
    cfg.network.completion_batch_capacity = 4u;
    cfg.network.event_capacity = 32u;
    cfg.network.max_send_bytes = 4096u;
    cfg.network.receive_buffer_bytes = 4096u;
    cfg.network.tls_io_buffer_bytes = 32768u;
    cfg.network.tls_handshake_timeout_ms = RECOVERY_REMOTE_TIMEOUT_MS;
    cfg.network.connect_timeout_ms = RECOVERY_REMOTE_TIMEOUT_MS;
    cfg.network.read_timeout_ms = RECOVERY_REMOTE_TIMEOUT_MS;
    cfg.network.write_timeout_ms = RECOVERY_REMOTE_TIMEOUT_MS;
    cfg.request_capacity = 2u;
    cfg.max_start_line_bytes = 256u;
    cfg.max_header_count = 16u;
    cfg.max_header_bytes = 4096u;
    cfg.max_request_body_bytes = 128u;
    cfg.max_response_body_bytes = MYGAMES_RECOVERY_RESPONSE_MAX;
    cfg.max_informational_responses = 2u;
    if (chttp_client_init(&client, &cfg) != SALTS_OK)
        goto done;
    client_ready = 1;

    for (j = 0u; j < pages; ++j) {
        chttp_response response = {0};
        uint64_t cursor = 0u, count = 0u;
        mygames_recovery_consumer_report report = {0};
        mygames_recovery_consumer_status st;
        int network_result;

        if (mygames_consumer_projection_snapshot(
                ledger, argv[7], argv[8], &cursor, &count) !=
            MYGAMES_CONSUMER_APPLIED ||
            cursor != count || cursor > (uint64_t)INT64_MAX)
            goto done;
        network_result = fetch_page(
            &client, &profile, uri, cursor, limit, &response);
        if (network_result != SALTS_OK) {
            chttp_response_destroy(&response);
            goto done;
        }

        /* Test-only simulated process death AFTER a genuine, complete HTTP
         * page but BEFORE any business COMMIT; supervisor must reread DB. */
        if (strcmp(mode, "before-commit") == 0) {
            mygames_store_archive_page decoded = {0};
            const mygames_recovery_wire_status ws =
                mygames_recovery_decode_page(
                    response.body, response.body_size, argv[7], argv[8],
                    cursor, mygames_outbox_public_event_validate, &decoded);
            chttp_response_destroy(&response);
            if (ws != MYGAMES_RECOVERY_WIRE_OK) goto done;
            printf("mygames.remote_recovery: STOPPED_BEFORE_COMMIT cursor=%" PRIu64 "\n",
                   cursor);
            result = 4;
            goto done;
        }

        st = mygames_recovery_consumer_apply_response(
            ledger, argv[7], argv[8], response.body, response.body_size,
            strcmp(mode, "commit-one") == 0 ? 1u : limit, &report);
        chttp_response_destroy(&response);
        if (st != MYGAMES_RECOVERY_CONSUMER_OK &&
            st != MYGAMES_RECOVERY_CONSUMER_MORE) {
            /* Never silently continue on COMMIT_UNKNOWN or malformed page. */
            fprintf(stderr, "remote recovery: refused page, status=%d\n", (int)st);
            goto done;
        }
        printf("mygames.remote_recovery: COMMITTED before=%" PRIu64
               " after=%" PRIu64 " new=%" PRIu64 "\n",
               report.committed_before, report.committed_after,
               report.newly_committed);
        fflush(stdout);
        if (st == MYGAMES_RECOVERY_CONSUMER_OK) {
            result = EXIT_SUCCESS;
            goto done;
        }
        if (strcmp(mode, "commit-one") == 0) break;
    }

    /* Explicit "more work" exit, never claim success simply because the
     * per-run page budget or owned work capacity was exhausted. */
    result = 3;
done:
    if (client_ready &&
        chttp_client_destroy(&client, RECOVERY_REMOTE_TIMEOUT_MS) != SALTS_OK)
        result = EXIT_FAILURE;
    if (tls_ready && chttp_tls_profile_destroy(&profile) != SALTS_OK)
        result = EXIT_FAILURE;
    mygames_consumer_projection_close(ledger);
    return result;
}
