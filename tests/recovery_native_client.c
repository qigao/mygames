/* Diagnostic only: real installed CHttp/CNet/GmSSL mTLS client.
 * No insecure TLS option or certificate-verification override.
 * Compare with independent OpenSSL client against SAME recovery endpoint.
 */
#include <http_client/http.h>
#include <salts/error_codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_TLS_TIMEOUT_MS 5000u

static native_io_backend_kind test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

int main(int argc, char **argv) {
    static const char *const alpn[] = {"http/1.1"};
    cnet_tls_client_config tls = {0};
    chttp_client_config config = {0};
    chttp_tls_profile profile = {0};
    chttp_client client = {0};
    chttp_response response = {0};
    chttp_options options = {0};
    chttp_error error = {0};
    char uri[128];
    int status, result = EXIT_FAILURE;
    int client_ready = 0, profile_ready = 0;

    if (argc != 5) {
        fprintf(stderr, "usage: native-client TLS-port ca.pem client.pem client.key\n");
        return EXIT_FAILURE;
    }
    if (snprintf(uri, sizeof(uri), "tls://127.0.0.1:%s", argv[1]) < 0 ||
        strlen(uri) >= sizeof(uri)) return EXIT_FAILURE;
    tls.size = sizeof(tls);
    tls.ca_file = argv[2];
    tls.cert_file = argv[3];
    tls.key_file = argv[4];
    tls.server_name = "localhost";
    tls.alpn_protocols = alpn;
    tls.alpn_protocol_count = 1u;

    config.network.backend = test_backend();
    config.network.connection_capacity = 2u;
    config.network.command_capacity = 32u;
    config.network.request_capacity = 8u;
    config.network.completion_batch_capacity = 4u;
    config.network.event_capacity = 32u;
    config.network.max_send_bytes = 4096u;
    config.network.receive_buffer_bytes = 4096u;
    config.network.tls_io_buffer_bytes = 32768u;
    config.network.tls_handshake_timeout_ms = RECOVERY_TLS_TIMEOUT_MS;
    config.network.connect_timeout_ms = RECOVERY_TLS_TIMEOUT_MS;
    config.network.read_timeout_ms = RECOVERY_TLS_TIMEOUT_MS;
    config.network.write_timeout_ms = RECOVERY_TLS_TIMEOUT_MS;
    config.request_capacity = 2u;
    config.max_start_line_bytes = 256u;
    config.max_header_count = 16u;
    config.max_header_bytes = 4096u;
    config.max_request_body_bytes = 128u;
    config.max_response_body_bytes = 32768u;
    config.max_informational_responses = 2u;

    status = chttp_tls_profile_init(&profile, &tls);
    if (status != SALTS_OK) {
        fprintf(stderr, "native-mtls: TLS profile refused code=%d\n", status);
        goto done;
    }
    profile_ready = 1;
    status = chttp_client_init(&client, &config);
    if (status != SALTS_OK) {
        fprintf(stderr, "native-mtls: Client init refused code=%d\n", status);
        goto done;
    }
    client_ready = 1;
    options.connection_uri = uri;
    options.authority = "localhost";
    options.target = "/internal/recovery/v1/events?after=0&limit=1";
    options.timeout_ms = RECOVERY_TLS_TIMEOUT_MS;
    options.tls = &profile;
    status = chttp_get(&client, &options, &response, &error);
    if (status != SALTS_OK) {
        fprintf(stderr, "native-mtls: GET failed code=%d detail=%d\n",
                status, error.status);
        goto done;
    }
    if (response.status_code != 200u || response.body == NULL ||
        response.body_size < 24u) {
        fprintf(stderr, "native-mtls: response mismatch HTTP=%u size=%zu\n",
                response.status_code, response.body_size);
        goto done;
    }
    printf("mygames.recovery_native_mtls: PASS HTTP=%u bytes=%zu\n",
           response.status_code, response.body_size);
    result = EXIT_SUCCESS;

done:
    chttp_response_destroy(&response);
    if (client_ready && chttp_client_destroy(&client, RECOVERY_TLS_TIMEOUT_MS) != SALTS_OK)
        result = EXIT_FAILURE;
    if (profile_ready && chttp_tls_profile_destroy(&profile) != SALTS_OK)
        result = EXIT_FAILURE;
    return result;
}
