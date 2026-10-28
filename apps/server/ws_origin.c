#include "ws_origin.h"

#include <stddef.h>
#include <string.h>

/* Exact serialized browser origin, never URL-prefix or substring matching.
 * Do not accept file:/opaque "null", cross-site hosts, userinfo, DNS
 * rebinding suffixes, path/query/fragment, or noncanonical ports. */
bool mygames_dev_ws_origin_allowed(const char *origin) {
    static const char *const hosts[] = {
        "127.0.0.1", "localhost", "[::1]"
    };
    const char *authority;
    size_t i;
    unsigned int port = 0u;
    unsigned int digits = 0u;
    if (origin == NULL) return true; /* Non-browser native callers only. */
    if (strncmp(origin, "http://", 7u) == 0)
        authority = origin + 7u;
    else if (strncmp(origin, "https://", 8u) == 0)
        authority = origin + 8u;
    else return false;
    for (i = 0u; i < sizeof(hosts) / sizeof(hosts[0]); ++i) {
        const size_t host_len = strlen(hosts[i]);
        if (strncmp(authority, hosts[i], host_len) != 0 ||
            (authority[host_len] != '\0' && authority[host_len] != ':'))
            continue;
        authority += host_len;
        if (*authority == '\0') return true;
        ++authority; /* A colon is present. Require a canonical port. */
        if (*authority < '1' || *authority > '9') return false;
        while (*authority >= '0' && *authority <= '9') {
            if (++digits > 5u) return false;
            port = port * 10u + (unsigned int)(*authority - '0');
            ++authority;
        }
        return *authority == '\0' && port > 0u && port <= 65535u;
    }
    return false;
}
