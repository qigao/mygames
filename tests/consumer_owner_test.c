#include "mygames/consumer_owner.h"
#include "mygames/outbox_dispatch.h"
#include "mygames/recovery_wire.h"

#include <salts/error_codes.h>
#include <salts/thread.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kNamespace = "trusted-source-v1";
static const char *const kConsumer = "backend-projection-v1";

static void canonical(char *out, size_t cap, uint64_t sequence,
                      unsigned int room) {
    const int n = snprintf(out, cap,
        "{\"v\":1,\"type\":\"room.command_accepted\","
        "\"room_id\":\"room-%u\",\"room_epoch\":1,"
        "\"revision\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        room, sequence, sequence);
    assert(n > 0 && (size_t)n < cap);
    assert(mygames_outbox_public_event_validate(
        (const uint8_t *)out, (size_t)n, 1u, sequence));
}
static void assert_cursor(mygames_consumer_projection *ledger,
                          uint64_t expected) {
    uint64_t last = UINT64_MAX, effects = UINT64_MAX;
    assert(mygames_consumer_projection_snapshot(
        ledger, kNamespace, kConsumer, &last, &effects) ==
        MYGAMES_CONSUMER_APPLIED);
    assert(last == expected && effects == expected);
}
static void erase_test_db(const char *name) {
    char path[1100];
    int len;
    assert(strlen(name) < sizeof(path) - 6u);
    (void)remove(name);
    len = snprintf(path, sizeof(path), "%s-wal", name);
    assert(len > 0 && (size_t)len < sizeof(path));
    (void)remove(path);
    len = snprintf(path, sizeof(path), "%s-shm", name);
    assert(len > 0 && (size_t)len < sizeof(path));
    (void)remove(path);
}
static void make_page(char *wire, size_t cap, uint64_t after,
                      uint64_t until, size_t *out_size) {
    mygames_recovery_grant grant = {0};
    mygames_store_archive_page page = {0};
    uint64_t seq;
    assert(until >= after && until - after <= MYGAMES_STORE_ARCHIVE_PAGE_MAX);
    grant.generation = 1u;
    grant.after_committed_seq = after;
    grant.page_limit = MYGAMES_STORE_ARCHIVE_PAGE_MAX;
    (void)snprintf(grant.source_namespace,
                   sizeof(grant.source_namespace), "%s", kNamespace);
    (void)snprintf(grant.consumer_id, sizeof(grant.consumer_id),
                   "%s", kConsumer);
    page.epoch = 1u;
    page.checkpoint_sequence = until;
    page.retained_from_sequence = 1u;
    page.resume_after_sequence = until;
    for (seq = after + 1u; seq <= until; ++seq) {
        mygames_store_archive_item *item = &page.items[page.count++];
        char json[MYGAMES_STORE_EVENT_MAX];
        canonical(json, sizeof(json), seq, 1u);
        item->sequence = seq;
        item->size = strlen(json);
        memcpy(item->payload, json, item->size);
    }
    assert(mygames_recovery_encode_page(
        &grant, &page, mygames_outbox_public_event_validate,
        wire, cap, out_size) == MYGAMES_RECOVERY_WIRE_OK);
}
typedef struct producer {
    mygames_consumer_owner *owner;
    uint64_t start;
    mygames_owner_ticket tickets[4];
    mygames_owner_result results[4];
    mygames_owner_result cross_thread_pump;
} producer;
static void produce_mqtt(void *arg) {
    producer *p = (producer *)arg;
    uint64_t ignored = UINT64_MAX;
    size_t i;
    char event[MYGAMES_STORE_EVENT_MAX];
    p->cross_thread_pump =
        mygames_consumer_owner_pump_one(p->owner, &ignored);
    assert(ignored == UINT64_MAX);
    for (i = 0u; i < 4u; ++i) {
        const uint64_t seq = p->start + i;
        canonical(event, sizeof(event), seq, 1u);
        p->results[i] = mygames_consumer_owner_admit_mqtt(
            p->owner, MYGAMES_OUTBOX_MQTT_TOPIC, event, strlen(event),
            1u, seq, &p->tickets[i]);
    }
}
typedef struct history_producer {
    mygames_consumer_owner *owner;
    const char *body;
    size_t size;
    uint64_t requested_after;
    mygames_owner_ticket tickets[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
    size_t admitted;
    mygames_owner_result result;
} history_producer;
static void produce_history(void *arg) {
    history_producer *h = (history_producer *)arg;
    h->result = mygames_consumer_owner_admit_history(
        h->owner, h->body, h->size, h->requested_after,
        h->tickets, MYGAMES_STORE_ARCHIVE_PAGE_MAX, &h->admitted);
}
typedef struct closing_thread {
    mygames_consumer_owner *owner;
    mygames_owner_result outcome;
} closing_thread;
static void close_from_other_thread(void *arg) {
    closing_thread *ctx = (closing_thread *)arg;
    ctx->outcome = mygames_consumer_owner_begin_close(ctx->owner);
}
static mygames_consumer_owner *open_owner(
    mygames_consumer_projection *ledger) {
    mygames_consumer_owner *owner = NULL;
    assert(mygames_consumer_owner_open(
        ledger, kNamespace, kConsumer, &owner) == MYGAMES_OWNER_OK);
    assert(owner != NULL);
    return owner;
}
static mygames_owner_ticket mqtt(mygames_consumer_owner *owner,
                                 uint64_t seq, unsigned int room) {
    mygames_owner_ticket ticket = {0};
    char payload[MYGAMES_STORE_EVENT_MAX];
    canonical(payload, sizeof(payload), seq, room);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, MYGAMES_OUTBOX_MQTT_TOPIC, payload, strlen(payload),
        1u, seq, &ticket) == MYGAMES_OWNER_OK);
    assert(ticket.generation);
    return ticket;
}
static void pump_is(mygames_consumer_owner *owner,
                    mygames_owner_result expected, uint64_t expected_seq) {
    uint64_t cursor = UINT64_MAX;
    assert(mygames_consumer_owner_pump_one(owner, &cursor) == expected);
    if (expected == MYGAMES_OWNER_APPLIED ||
        expected == MYGAMES_OWNER_REPLAY ||
        expected == MYGAMES_OWNER_GAP ||
        expected == MYGAMES_OWNER_EMPTY)
        assert(cursor == expected_seq);
    else
        assert(cursor == UINT64_MAX);
}

int main(int argc, char **argv) {
    mygames_consumer_projection *ledger = NULL;
    mygames_consumer_owner *owner = NULL;
    mygames_owner_ticket first = {0}, reused = {0};
    mygames_owner_ticket page_tickets[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
    char wire[MYGAMES_RECOVERY_RESPONSE_MAX];
    char invalid[MYGAMES_RECOVERY_RESPONSE_MAX];
    char event[MYGAMES_STORE_EVENT_MAX];
    size_t n = 0u, admitted = 0u, i;
    cmeta_thread_t threads[3] = {NULL, NULL, NULL};
    producer a = {0}, b = {0};
    history_producer h = {0};
    closing_thread close_job = {0};

    if (argc != 3) {
        fputs("usage: consumer_owner_test turbo-db-plugin consumer-db\n",stderr);
        return 2;
    }
    erase_test_db(argv[2]);
    assert(mygames_consumer_projection_open(argv[1], argv[2], &ledger) ==
           MYGAMES_CONSUMER_APPLIED);
    owner = open_owner(ledger);
    first = mqtt(owner, 3u, 1u);
    pump_is(owner, MYGAMES_OWNER_GAP, 0u);
    assert_cursor(ledger, 0u);

    /* Wrong topic and malformed events are refused before borrowing data. */
    canonical(event, sizeof(event), 1u, 1u);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, "mygames/public/room-events", event, strlen(event),
        1u, 1u, &reused) == MYGAMES_OWNER_INVALID);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, MYGAMES_OUTBOX_MQTT_TOPIC, "{bad}", 5u, 1u, 1u,
        &reused) == MYGAMES_OWNER_BAD_EVENT);
    make_page(wire, sizeof(wire), 0u, 2u, &n);
    assert(mygames_consumer_owner_admit_history(owner, wire, n, 0u,
        page_tickets, MYGAMES_STORE_ARCHIVE_PAGE_MAX, &admitted) ==
        MYGAMES_OWNER_OK);
    assert(admitted == 2u);
    /* Source buffer can now change: Owner copied canonical payload bytes. */
    memset(wire, '!', n);
    pump_is(owner, MYGAMES_OWNER_APPLIED, 1u);
    pump_is(owner, MYGAMES_OWNER_APPLIED, 2u);
    pump_is(owner, MYGAMES_OWNER_APPLIED, 3u);
    pump_is(owner, MYGAMES_OWNER_EMPTY, 3u);
    assert_cursor(ledger, 3u);
    assert(mygames_consumer_owner_cancel(owner, first) ==
           MYGAMES_OWNER_STALE);
    assert(mygames_consumer_owner_destroy(owner) == MYGAMES_OWNER_OK);

    /* Concurrent MQTT callbacks and HTTP response delivery have ONE copied,
     * thread-safe bounded admission queue. Only this main thread accesses
     * TurboDB through pump_one; crossing Owner affinity must fail closed. */
    owner = open_owner(ledger);
    make_page(wire, sizeof(wire), 3u, 19u, &n);
    a.owner = owner; a.start = 5u;
    b.owner = owner; b.start = 9u;
    h.owner = owner; h.body = wire; h.size = n; h.requested_after = 3u;
    assert(cmeta_thread_create(&threads[0], produce_mqtt, &a) == SALTS_OK);
    assert(cmeta_thread_create(&threads[1], produce_mqtt, &b) == SALTS_OK);
    assert(cmeta_thread_create(&threads[2], produce_history, &h) == SALTS_OK);
    for (i = 0u; i < 3u; ++i) {
        assert(cmeta_thread_join(&threads[i]) == SALTS_OK);
        cmeta_thread_destroy(&threads[i]);
    }
    assert(a.cross_thread_pump == MYGAMES_OWNER_INVALID);
    assert(b.cross_thread_pump == MYGAMES_OWNER_INVALID);
    for (i = 0u; i < 4u; ++i) {
        assert(a.results[i] == MYGAMES_OWNER_OK);
        assert(b.results[i] == MYGAMES_OWNER_OK);
    }
    assert(h.result == MYGAMES_OWNER_OK && h.admitted == 16u);
    canonical(event, sizeof(event), 13u, 1u);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, MYGAMES_OUTBOX_MQTT_TOPIC, event, strlen(event),
        1u, 13u, &reused) == MYGAMES_OWNER_FULL);
    assert(reused.generation == 0u);

    /* Cancellation uses an exact generation, not just recycled slot index.
     * One freed MQTT slot can be reused but the old grant stays stale. */
    assert(mygames_consumer_owner_cancel(owner, a.tickets[0]) ==
           MYGAMES_OWNER_OK);
    assert(mygames_consumer_owner_cancel(owner, a.tickets[0]) ==
           MYGAMES_OWNER_STALE);
    reused = mqtt(owner, 13u, 1u);
    assert(mygames_consumer_owner_cancel(owner, a.tickets[0]) ==
           MYGAMES_OWNER_STALE);
    assert(reused.generation != 0u);

    for (i = 4u; i <= 19u; ++i)
        pump_is(owner, MYGAMES_OWNER_APPLIED, (uint64_t)i);
    for (i = 0u; i < 8u; ++i)
        pump_is(owner, MYGAMES_OWNER_REPLAY, 19u);
    pump_is(owner, MYGAMES_OWNER_EMPTY, 19u);
    assert_cursor(ledger, 19u);

    /* This is an intentionally valid canonical DIFFERENT event with the
     * same sequence. A duplicate receipt with changed digest quarantines
     * the entire Owner instead of producing a second effect. */
    (void)mqtt(owner, 19u, 2u);
    pump_is(owner, MYGAMES_OWNER_QUARANTINED, UINT64_MAX);
    assert_cursor(ledger, 19u);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, MYGAMES_OUTBOX_MQTT_TOPIC, event, strlen(event),
        1u, 13u, &reused) == MYGAMES_OWNER_QUARANTINED);
    assert(mygames_consumer_owner_destroy(owner) == MYGAMES_OWNER_OK);

    /* Rollback after a real receipt INSERT but before its cursor UPDATE:
     * still no committed receipt/effect/cursor, Owner quarantines failure. */
    owner = open_owner(ledger);
    (void)mqtt(owner, 20u, 1u);
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
    mygames_consumer_projection_fail_after_receipt_once(ledger);
#endif
    pump_is(owner, MYGAMES_OWNER_QUARANTINED, UINT64_MAX);
    assert_cursor(ledger, 19u);
    assert(mygames_consumer_owner_destroy(owner) == MYGAMES_OWNER_OK);

    /* Close from another thread cancels QUEUED slots. It never requires a
     * DB COMMIT from the callback thread nor frees an in-flight buffer. */
    owner = open_owner(ledger);
    first = mqtt(owner, 20u, 1u);
    close_job.owner = owner;
    assert(cmeta_thread_create(
        &threads[0], close_from_other_thread, &close_job) == SALTS_OK);
    assert(cmeta_thread_join(&threads[0]) == SALTS_OK);
    cmeta_thread_destroy(&threads[0]);
    assert(close_job.outcome == MYGAMES_OWNER_OK);
    pump_is(owner, MYGAMES_OWNER_CLOSED, UINT64_MAX);
    assert(mygames_consumer_owner_cancel(owner, first) ==
           MYGAMES_OWNER_STALE);
    canonical(event, sizeof(event), 20u, 1u);
    assert(mygames_consumer_owner_admit_mqtt(
        owner, MYGAMES_OUTBOX_MQTT_TOPIC, event, strlen(event),
        1u, 20u, &reused) == MYGAMES_OWNER_CLOSED);
    assert(mygames_consumer_owner_destroy(owner) == MYGAMES_OWNER_OK);
    assert_cursor(ledger, 19u);

    owner = open_owner(ledger);
    (void)mqtt(owner, 20u, 1u);
    pump_is(owner, MYGAMES_OWNER_APPLIED, 20u);
    assert_cursor(ledger, 20u);
    assert(mygames_consumer_owner_destroy(owner) == MYGAMES_OWNER_OK);
    mygames_consumer_projection_close(ledger);
    erase_test_db(argv[2]);
    puts("mygames.consumer_owner: PASS capacity, concurrent ingress, "
         "gap/replay/conflict, cancellation, close and TurboDB rollback");
    return 0;
}
