#include "mygames/recovery_consumer.h"
#include "mygames/recovery_wire.h"
#include "mygames/outbox_dispatch.h"
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

static void event_for(mygames_store_archive_item *event, uint64_t seq) {
    int count;
    assert(event != NULL);
    event->sequence = seq;
    count = snprintf((char *)event->payload, sizeof(event->payload),
        "{\"v\":1,\"type\":\"room.command_accepted\","
        "\"room_id\":\"room-1\",\"room_epoch\":1,"
        "\"revision\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        seq, seq);
    assert(count > 0 && (size_t)count < sizeof(event->payload));
    event->size = (size_t)count;
    assert(mygames_outbox_public_event_validate(
        event->payload, event->size, 1u, seq));
}
static void page_for(mygames_store_archive_page *page,
                     uint64_t after, uint64_t checkpoint) {
    uint64_t i;
    assert(page && checkpoint >= after && checkpoint - after <= 16u);
    memset(page, 0, sizeof(*page));
    page->epoch = 1u;
    page->checkpoint_sequence = checkpoint;
    page->retained_from_sequence = 1u;
    for (i = after + 1u; i <= checkpoint; ++i) {
        event_for(&page->items[page->count], i);
        ++page->count;
    }
    page->resume_after_sequence = checkpoint;
}
static size_t response_for(char *output, size_t capacity,
                           mygames_recovery_grant *grant,
                           uint64_t after, uint64_t checkpoint) {
    mygames_store_archive_page page;
    size_t n = 0u;
    page_for(&page, after, checkpoint);
    grant->after_committed_seq = after;
    assert(mygames_recovery_encode_page(
        grant, &page, mygames_outbox_public_event_validate,
        output, capacity, &n) == MYGAMES_RECOVERY_WIRE_OK);
    assert(n > 0u && n < capacity);
    return n;
}
static void cursor_is(mygames_consumer_projection *consumer,
                      uint64_t expected) {
    uint64_t seq = UINT64_MAX, count = UINT64_MAX;
    assert(mygames_consumer_projection_snapshot(
        consumer, kNamespace, kConsumer, &seq, &count) ==
        MYGAMES_CONSUMER_APPLIED);
    assert(seq == expected && count == expected);
}
static void remove_database(const char *path) {
    char name[1100];
    int n;
    assert(path && strlen(path) < sizeof(name)-8u);
    (void)remove(path);
    n = snprintf(name, sizeof(name), "%s-wal", path);
    assert(n > 0 && (size_t)n < sizeof(name));
    (void)remove(name);
    n = snprintf(name, sizeof(name), "%s-shm", path);
    assert(n > 0 && (size_t)n < sizeof(name));
    (void)remove(name);
}

int main(int argc, char **argv) {
    mygames_consumer_projection *ledger = NULL;
    mygames_recovery_grant grant = {0};
    mygames_recovery_consumer_report report = {0};
    char json[MYGAMES_RECOVERY_RESPONSE_MAX];
    char old_json[MYGAMES_RECOVERY_RESPONSE_MAX];
    size_t n, old_n;
    if (argc != 3) {
        fputs("usage: recovery-consumer-test sqlite-driver consumer-db\n", stderr);
        return 2;
    }
    remove_database(argv[2]);
    grant.generation = 1u;
    grant.page_limit = MYGAMES_STORE_ARCHIVE_PAGE_MAX;
    (void)snprintf(grant.source_namespace, sizeof(grant.source_namespace),
                   "%s", kNamespace);
    (void)snprintf(grant.consumer_id, sizeof(grant.consumer_id),
                   "%s", kConsumer);
    old_n = response_for(old_json, sizeof(old_json), &grant, 0u, 3u);

    assert(mygames_consumer_projection_open(argv[1], argv[2], &ledger) ==
           MYGAMES_CONSUMER_APPLIED);
    cursor_is(ledger, 0u);

    /* HTTP page receipt is NOT a COMMIT. A deliberate one-event budget
     * simulates process shutdown after precisely one durable effect. */
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, old_json, old_n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_MORE);
    assert(report.committed_before == 0u && report.committed_after == 1u &&
           report.newly_committed == 1u &&
           report.source_checkpoint_sequence == 3u && report.has_more);
    cursor_is(ledger, 1u);

    /* A forged page or wrong principal cannot bypass cursor validation. */
    report.committed_after = UINT64_MAX;
    assert(mygames_recovery_consumer_apply_response(
        ledger, "wrong-source", kConsumer, old_json, old_n,
        1u, &report) == MYGAMES_RECOVERY_CONSUMER_BAD_PAGE);
    assert(report.committed_after == UINT64_MAX);
    cursor_is(ledger, 1u);

    mygames_consumer_projection_close(ledger);
    ledger = NULL;
    assert(mygames_consumer_projection_open(argv[1], argv[2], &ledger) ==
           MYGAMES_CONSUMER_APPLIED);
    cursor_is(ledger, 1u);

    /* Old HTTP response cannot force the consumer backwards; the client
     * MUST issue the next GET after its newly loaded committed seq=1. */
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, old_json, old_n,
        2u, &report) == MYGAMES_RECOVERY_CONSUMER_BAD_PAGE);
    cursor_is(ledger, 1u);
    n = response_for(json, sizeof(json), &grant, 1u, 3u);
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, json, n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_MORE);
    assert(report.committed_before == 1u && report.committed_after == 2u &&
           report.newly_committed == 1u && report.has_more);
    cursor_is(ledger, 2u);

    /* Real TurboDB rollback after receipt INSERT but before cursor UPDATE.
     * No partial receipt, count or cursor may survive; recovery may retry. */
    n = response_for(json, sizeof(json), &grant, 2u, 3u);
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
    mygames_consumer_projection_fail_after_receipt_once(ledger);
#endif
    report.committed_after = UINT64_MAX;
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, json, n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR);
    assert(report.committed_after == UINT64_MAX);
    cursor_is(ledger, 2u);
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, json, n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_OK);
    assert(report.committed_before == 2u && report.committed_after == 3u &&
           report.newly_committed == 1u && !report.has_more);
    cursor_is(ledger, 3u);

    /* A successful retry cannot duplicate event effects after restart. */
    mygames_consumer_projection_close(ledger);
    ledger = NULL;
    assert(mygames_consumer_projection_open(argv[1], argv[2], &ledger) ==
           MYGAMES_CONSUMER_APPLIED);
    cursor_is(ledger, 3u);
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, json, n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_BAD_PAGE);
    cursor_is(ledger, 3u);

    n = response_for(json, sizeof(json), &grant, 3u, 3u);
    assert(mygames_recovery_consumer_apply_response(
        ledger, kNamespace, kConsumer, json, n, 1u, &report) ==
        MYGAMES_RECOVERY_CONSUMER_OK);
    assert(report.committed_before == 3u && report.committed_after == 3u &&
           report.newly_committed == 0u && !report.has_more);
    cursor_is(ledger, 3u);

    mygames_consumer_projection_close(ledger);
    remove_database(argv[2]);
    puts("mygames.recovery_consumer: PASS persistent partial-page and fault rollback");
    return 0;
}
