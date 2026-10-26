#ifndef MYGAMES_TEST_FLOWIE_OWNER_HTTP_H
#define MYGAMES_TEST_FLOWIE_OWNER_HTTP_H

#include "mygames/consumer_owner.h"
#include <salts/thread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Test-only CHttp/CNet TLS worker. Never borrows Flowie's MQTT callback
 * payload or ConsumerProjection, and never calls TurboDB. The verified
 * HTTP 200 page is admitted by value into the single Consumer Owner.
 */
typedef struct mygames_test_owner_http {
    mygames_consumer_owner *owner; /* caller must join before destroying */
    const char *ca_file;
    const char *client_cert;
    const char *client_key;
    uint16_t port;
    uint64_t committed_after; /* snapshot by Owner creator before launch */
    cmeta_thread_t thread;
    atomic_bool in_flight;
    bool started;
    bool successful; /* read only AFTER joining */
    size_t admitted; /* read only AFTER joining */
} mygames_test_owner_http;

int mygames_test_owner_http_start(mygames_test_owner_http *job);
int mygames_test_owner_http_join(mygames_test_owner_http *job);
#endif
