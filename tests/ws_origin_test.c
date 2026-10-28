/* Test the real development WebSocket origin rule without CHttp or a GUI. */
#include "../apps/server/ws_origin.h"

#include <stdio.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "ws Origin contract failed at line %d: %s\n", \
                __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void) {
    static const char *const permitted[] = {
        "http://127.0.0.1", "https://127.0.0.1:443",
        "http://127.0.0.1:1780", "http://localhost:12345",
        "https://localhost", "http://[::1]", "http://[::1]:65535"
    };
    static const char *const rejected[] = {
        "", "null", "file://", "ws://127.0.0.1",
        "http://example.com", "https://evil.example",
        "http://localhost.evil.example", "http://127.0.0.1.evil.example",
        "http://127.0.0.11", "http://127.0.0.1@evil.example",
        "http://evil.example@localhost", "http://localhost.",
        "http://localhost/", "http://localhost/?q=1",
        "http://localhost#fragment", "http://localhost:0",
        "http://localhost:", "http://localhost:00080",
        "http://localhost:65536", "http://localhost:999999",
        "http://localhost:80/", "http://localhost:80?x=1",
        "HTTP://localhost", "http://LOCALHOST", "http://[::ffff:127.0.0.1]",
        "http://[::1", "http://[::1]:-1", "http://localhost\r",
        "http://localhost\nOrigin: https://evil.example"
    };
    size_t i;
    CHECK(mygames_dev_ws_origin_allowed(NULL));
    for (i = 0u; i < sizeof(permitted) / sizeof(permitted[0]); ++i)
        CHECK(mygames_dev_ws_origin_allowed(permitted[i]));
    for (i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i)
        CHECK(!mygames_dev_ws_origin_allowed(rejected[i]));
    puts("mygames.ws_origin: PASS (canonical loopback browser origins only)");
    return 0;
}
