#include "mygames/recovery_wire.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char kNamespace[] = "trusted-source-v1";
static const char kConsumer[] = "backend-projection-v1";

static bool public_event(const uint8_t *bytes, size_t length,
                         uint64_t owner_epoch, uint64_t sequence) {
    char canonical[MYGAMES_STORE_EVENT_MAX];
    int n;
    if (!bytes || owner_epoch != 1u || sequence < 1u || sequence > 16u)
        return false;
    n = snprintf(canonical, sizeof(canonical),
        "{\"v\":1,\"type\":\"room.command_accepted\","
        "\"room_id\":\"room-1\",\"room_epoch\":1,"
        "\"revision\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        sequence, sequence);
    return n > 0 && (size_t)n < sizeof(canonical) &&
           (size_t)n == length && memcmp(bytes, canonical, length) == 0;
}

static void fill_event(mygames_store_archive_item *event, uint64_t seq) {
    int n;
    assert(event != NULL);
    event->sequence = seq;
    n = snprintf((char *)event->payload, sizeof(event->payload),
        "{\"v\":1,\"type\":\"room.command_accepted\","
        "\"room_id\":\"room-1\",\"room_epoch\":1,"
        "\"revision\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        seq, seq);
    assert(n > 0 && (size_t)n < sizeof(event->payload));
    event->size = (size_t)n;
    assert(public_event(event->payload, event->size, 1u, seq));
}

static void assert_fail(const void *buffer, size_t length,
                        const char *namespace_id, const char *consumer_id,
                        uint64_t after,
                        mygames_recovery_wire_status want) {
    mygames_store_archive_page out;
    size_t i;
    const uint8_t *raw = (const uint8_t *)&out;
    memset(&out, 0xa5, sizeof(out));
    assert(mygames_recovery_decode_page(
        buffer, length, namespace_id, consumer_id,
        after, public_event, &out) == want);
    for (i = 0u; i < sizeof(out); ++i) {
        /* Rejects publish no partial data to a caller-owned page. */
        assert(raw[i] == 0xa5u);
    }
}

static void assert_corrupt(const char *json) {
    assert_fail(json, strlen(json), kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
}

int main(void) {
    mygames_recovery_grant grant = {0};
    mygames_store_archive_page original = {0};
    mygames_store_archive_page parsed = {0};
    char json[MYGAMES_RECOVERY_RESPONSE_MAX];
    char mutated[MYGAMES_RECOVERY_RESPONSE_MAX];
    char huge[MYGAMES_RECOVERY_RESPONSE_MAX + 1u];
    size_t n = 0u;
    char *p;

    grant.generation = 5u;
    grant.page_limit = 16u;
    (void)snprintf(grant.source_namespace, sizeof(grant.source_namespace),
                   "%s", kNamespace);
    (void)snprintf(grant.consumer_id, sizeof(grant.consumer_id),
                   "%s", kConsumer);
    original.epoch = 1u;
    original.checkpoint_sequence = 2u;
    original.retained_from_sequence = 1u;
    original.count = 2u;
    original.resume_after_sequence = 2u;
    fill_event(&original.items[0], 1u);
    fill_event(&original.items[1], 2u);

    assert(mygames_recovery_encode_page(
        &grant, &original, public_event, json, sizeof(json), &n) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(n == strlen(json) && n > 100u);
    assert(mygames_recovery_decode_page(
        json, n, kNamespace, kConsumer, 0u, public_event, &parsed) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(parsed.epoch == 1u && parsed.checkpoint_sequence == 2u);
    assert(parsed.retained_from_sequence == 1u &&
           parsed.resume_after_sequence == 2u);
    assert(parsed.count == 2u && !parsed.has_more);
    assert(parsed.items[0].sequence == 1u &&
           parsed.items[1].sequence == 2u);
    assert(parsed.items[0].size == original.items[0].size);
    assert(memcmp(parsed.items[0].payload, original.items[0].payload,
                  original.items[0].size) == 0);

    /* Neither an incorrect policy identity nor an unrelated consumer may
     * adopt a valid signed/transport-verified page into their ledger. */
    assert_fail(json, n, "another-source", kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL);
    assert_fail(json, n, kNamespace, "another-consumer", 0u,
                MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL);
    assert_fail(json, n, kNamespace, kConsumer, 1u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(json, n, kNamespace, kConsumer, (uint64_t)INT64_MAX + 1u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(json, n, "forged\"", kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(json, n, kNamespace, "", 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(json, n - 1u, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(json, 0u, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    assert_fail(NULL, n, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    memset(huge, 'x', sizeof(huge));
    assert_fail(huge, sizeof(huge), kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);

    /* Exact wire grammar refuses an altered version, source namespace,
     * consumer identity, checkpoint, retention, digest-relevant content,
     * sequence, resume, boolean, duplicate field, or tail. */
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"v\":1");
    assert(p);
    p[4] = '2';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"source_namespace\":\"");
    assert(p);
    p[strlen("\"source_namespace\":\"")] = 'x';
    assert_fail(mutated, n, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"consumer_id\":\"");
    assert(p);
    p[strlen("\"consumer_id\":\"")] = 'x';
    assert_fail(mutated, n, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"owner_epoch\":1");
    assert(p);
    p[strlen("\"owner_epoch\":")] = '0';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"checkpoint_seq\":2");
    assert(p);
    p[strlen("\"checkpoint_seq\":")] = '1';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"retained_from_seq\":1");
    assert(p);
    p[strlen("\"retained_from_seq\":")] = '2';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"room_id\":\"room-1\"");
    assert(p);
    p[strlen("\"room_id\":\"room-")] = '2';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"revision\":2");
    assert(p);
    p[strlen("\"revision\":")] = '3';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"resume_after\":2");
    assert(p);
    p[strlen("\"resume_after\":")] = '3';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"has_more\":false");
    assert(p);
    p[strlen("\"has_more\":")] = 't';
    assert_corrupt(mutated);
    memcpy(mutated, json, n + 1u);
    mutated[n] = '!';
    mutated[n + 1u] = '\0';
    assert_corrupt(mutated);

    /* A page may complete mid-checkpoint; the HTTP resume hint is never
     * durable settlement and a new invocation can request after=1. */
    grant.after_committed_seq = 1u;
    original.items[0] = original.items[1];
    original.count = 1u;
    original.resume_after_sequence = 2u;
    assert(mygames_recovery_encode_page(
        &grant, &original, public_event, json, sizeof(json), &n) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(mygames_recovery_decode_page(
        json, n, kNamespace, kConsumer, 1u, public_event, &parsed) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(parsed.count == 1u && parsed.items[0].sequence == 2u);
    assert_fail(json, n, kNamespace, kConsumer, 0u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);

    original.count = 0u;
    original.resume_after_sequence = 2u;
    grant.after_committed_seq = 2u;
    assert(mygames_recovery_encode_page(
        &grant, &original, public_event, json, sizeof(json), &n) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(mygames_recovery_decode_page(
        json, n, kNamespace, kConsumer, 2u, public_event, &parsed) ==
        MYGAMES_RECOVERY_WIRE_OK);
    assert(parsed.count == 0u && parsed.resume_after_sequence == 2u &&
           !parsed.has_more);

    /* An empty 200 response must never claim that events remain buffered. */
    memcpy(mutated, json, n + 1u);
    p = strstr(mutated, "\"has_more\":false");
    assert(p);
    p[strlen("\"has_more\":")] = 't';
    assert_fail(mutated, n, kNamespace, kConsumer, 2u,
                MYGAMES_RECOVERY_WIRE_INVALID_PAGE);

    puts("mygames.recovery_decode: PASS");
    return 0;
}
