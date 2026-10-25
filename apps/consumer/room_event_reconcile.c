/* Explicit LOCAL ADMINISTRATION / CI reconciliation tool.
 *
 * Read the immutable committed source Outbox archive (INCLUDING published
 * events) using TurboDB, then apply missing events to an independent
 * ConsumerProjection with receipt+effect+cursor in one transaction.
 *
 * Source access uses a verified SQLITE_OPEN_READONLY ORM connection.
 * This tool assumes a TRUSTED source database path. It is not a public
 * backfill HTTP endpoint, broker Subscriber or identity authorization layer;
 * deployed consumers must instead use an authenticated read-only service.
 * Never use broker PUBACK as evidence that the consumer committed.
 */
#include "mygames/room_store.h"
#include "mygames/consumer_projection.h"
#include "mygames/outbox_dispatch.h"
#include <cmeta_crypto.h>
#include <salts/error_codes.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MYGAMES_RECONCILE_MAX_BATCH 128u

static int reconcile_main(int argc, char **argv) {
    mygames_room_store *source = NULL;
    mygames_consumer_projection *consumer = NULL;
    mygames_room_directory rooms;
    uint8_t hash[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES];
    uint64_t source_epoch = 0u, source_seq = 0u;
    uint64_t last_seq = 0u, count = 0u, applied = 0u, replayed = 0u;
    uint64_t begin_seq, current;
    bool verify_all;
    unsigned int budget = 0u, processed = 0u;
    mygames_store_status st;
    mygames_consumer_status cs;
    int rc = 1;

    if (argc != 8 ||
        (strcmp(argv[6], "--catchup") != 0 &&
         strcmp(argv[6], "--verify-all") != 0)) {
        fprintf(stderr, "usage: reconcile sqlite-driver source-db consumer-db "
                        "trusted-namespace consumer-id --catchup|--verify-all max-1..128\n");
        return 2;
    }
    {
        char *end = NULL;
        const unsigned long n = strtoul(argv[7], &end, 10);
        if (!end || *end || n == 0ul || n > MYGAMES_RECONCILE_MAX_BATCH)
            return 2;
        budget = (unsigned int)n;
    }
    verify_all = strcmp(argv[6], "--verify-all") == 0;
    /* The source archive is never opened through a writer-capable handle.
     * This does not grant remote consumer authorization; CLI remains local
     * administrator/CI-only. */
    st = mygames_room_store_open_readonly(argv[1], argv[2], &source);
    if (st != MYGAMES_STORE_OK || !source) {
        fprintf(stderr, "Reconcile: cannot read exact source archive (%d)\n", st);
        goto done;
    }
    cs = mygames_consumer_projection_open(argv[1], argv[3], &consumer);
    if (cs != MYGAMES_CONSUMER_APPLIED || !consumer) {
        fprintf(stderr, "Reconcile: cannot open consumer ledger (%d)\n", cs);
        goto done;
    }
    st = mygames_room_store_load(source, &rooms, &source_epoch, &source_seq);
    if (st != MYGAMES_STORE_OK || !source_epoch ||
        source_seq > (uint64_t)INT64_MAX) {
        fprintf(stderr, "Reconcile: unverified source checkpoint (%d)\n", st);
        goto done;
    }
    cs = mygames_consumer_projection_snapshot(
        consumer, argv[4], argv[5], &last_seq, &count);
    if (cs != MYGAMES_CONSUMER_APPLIED || last_seq > source_seq) {
        fprintf(stderr, "Reconcile: invalid source namespace / consumer cursor\n");
        goto done;
    }
    begin_seq = verify_all ? 1u : last_seq + 1u;
    current = begin_seq;
    while (current <= source_seq && processed < budget) {
        mygames_store_archive_page page = {0};
        uint64_t remaining = source_seq - current + 1u;
        uint64_t cap = (uint64_t)budget - (uint64_t)processed;
        size_t i;
        if (cap > MYGAMES_STORE_ARCHIVE_PAGE_MAX)
            cap = MYGAMES_STORE_ARCHIVE_PAGE_MAX;
        if (cap > remaining) cap = remaining;
        /* One pinned READONLY SQLite transaction per bounded page.
         * Consumer effects/cursor are still committed individually and may
         * recover after a crash at any partial page boundary. */
        st = mygames_room_store_read_archive_page(
            source, current - 1u, (uint32_t)cap, &page);
        if (st != MYGAMES_STORE_OK || !page.count ||
            page.checkpoint_sequence < source_seq ||
            page.epoch < source_epoch || page.count > cap) {
            fprintf(stderr, "Reconcile: missing/inconsistent archive page"
                            " seq=%" PRIu64 " status=%d\n", current, st);
            goto done;
        }
        for (i = 0u; i < page.count; ++i) {
            const mygames_store_archive_item *event = &page.items[i];
            uint64_t new_count = UINT64_MAX;
            if (event->sequence != current ||
                !mygames_outbox_public_event_validate(
                    event->payload, event->size, page.epoch, current) ||
                cmeta_sha256(event->payload, event->size, hash) != SALTS_OK) {
                fprintf(stderr, "Reconcile: missing/corrupt committed event"
                                " seq=%" PRIu64 "\n", current);
                goto done;
            }
            cs = mygames_consumer_projection_apply(
                consumer, argv[4], argv[5], current, hash, &new_count);
            memset(hash, 0, sizeof(hash));
            if (cs == MYGAMES_CONSUMER_APPLIED) ++applied;
            else if (cs == MYGAMES_CONSUMER_REPLAY) ++replayed;
            else {
                fprintf(stderr, "Reconcile: consumer refused seq=%" PRIu64
                                " status=%d\n", current, cs);
                goto done;
            }
            count = new_count;
            ++processed;
            ++current;
        }
    }
    if (current <= source_seq) {
        fprintf(stderr, "Reconcile: bounded batch incomplete, next=%" PRIu64
                        " end=%" PRIu64 "\n", current, source_seq);
        rc = 3; /* supervisor should explicitly continue, not report success */
        goto done;
    }
    {
        uint64_t committed_last = 0u, committed_count = 0u;
        cs = mygames_consumer_projection_snapshot(
            consumer, argv[4], argv[5], &committed_last, &committed_count);
        if (cs != MYGAMES_CONSUMER_APPLIED ||
            committed_last != source_seq ||
            committed_count != source_seq) {
            fprintf(stderr, "Reconcile: final projection cursor mismatch\n");
            goto done;
        }
    }
    printf("mygames.consumer_reconcile: PASS applied=%" PRIu64
           " replay=%" PRIu64 " last_seq=%" PRIu64 "\n",
           applied, replayed, source_seq);
    rc = 0;
done:
    memset(hash, 0, sizeof(hash));
    mygames_consumer_projection_close(consumer);
    mygames_room_store_close(source);
    return rc;
}
int main(int argc, char **argv) { return reconcile_main(argc, argv); }
