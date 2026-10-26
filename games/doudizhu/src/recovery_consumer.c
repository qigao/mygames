#include "mygames/recovery_consumer.h"
#include "mygames/recovery_wire.h"
#include "mygames/outbox_dispatch.h"

#include <cmeta_crypto.h>
#include <salts/error_codes.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* The sole consumer Owner owns the ledger for this entire operation.
 * This is a bounded synchronous staging layer, not a CHttp client or a
 * worker pool. It does not acknowledge MQTT or source Outbox messages. */
static mygames_recovery_consumer_status ledger_error(
    mygames_consumer_status result) {
    if (result == MYGAMES_CONSUMER_COMMIT_UNKNOWN ||
        result == MYGAMES_CONSUMER_QUARANTINED)
        return MYGAMES_RECOVERY_CONSUMER_COMMIT_UNKNOWN;
    return MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR;
}

mygames_recovery_consumer_status
mygames_recovery_consumer_apply_response(
    mygames_consumer_projection *ledger,
    const char *expected_source_namespace, const char *expected_consumer_id,
    const void *http_response_bytes, size_t response_size,
    uint32_t max_events_per_call,
    mygames_recovery_consumer_report *out_report) {
    mygames_store_archive_page page = {0};
    mygames_recovery_consumer_report report = {0};
    mygames_consumer_status st;
    uint64_t cursor = 0u, count = 0u;
    size_t limit, i;

    if (!ledger || !expected_source_namespace || !expected_consumer_id ||
        !http_response_bytes || !response_size || !out_report ||
        max_events_per_call == 0u ||
        max_events_per_call > MYGAMES_STORE_ARCHIVE_PAGE_MAX)
        return MYGAMES_RECOVERY_CONSUMER_INVALID;

    /* Strictly read from durable receipt/effect/cursor state. Never use
     * response.resume_after or a remembered prior HTTP callback. */
    st = mygames_consumer_projection_snapshot(
        ledger, expected_source_namespace, expected_consumer_id,
        &cursor, &count);
    if (st != MYGAMES_CONSUMER_APPLIED)
        return ledger_error(st);
    if (cursor > (uint64_t)INT64_MAX || cursor != count)
        return MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR;

    if (mygames_recovery_decode_page(
            http_response_bytes, response_size,
            expected_source_namespace, expected_consumer_id,
            cursor, mygames_outbox_public_event_validate, &page) !=
        MYGAMES_RECOVERY_WIRE_OK)
        return MYGAMES_RECOVERY_CONSUMER_BAD_PAGE;

    report.committed_before = cursor;
    report.source_checkpoint_sequence = page.checkpoint_sequence;
    limit = page.count;
    if (limit > max_events_per_call) limit = max_events_per_call;
    for (i = 0u; i < limit; ++i) {
        const mygames_store_archive_item *event = &page.items[i];
        uint8_t digest[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES] = {0};
        uint64_t committed_effects = 0u;

        if (event->sequence != cursor + 1u ||
            cmeta_sha256(event->payload, event->size, digest) != SALTS_OK) {
            memset(digest, 0, sizeof(digest));
            return MYGAMES_RECOVERY_CONSUMER_BAD_PAGE;
        }
        st = mygames_consumer_projection_apply(
            ledger, expected_source_namespace, expected_consumer_id,
            event->sequence, digest, &committed_effects);
        memset(digest, 0, sizeof(digest));

        /* A replay/CONFLICT here means some other owner advanced the ledger
         * since our earlier snapshot. Never treat it as success or let two
         * worker processes concurrently own the same projection handle. */
        if (st != MYGAMES_CONSUMER_APPLIED)
            return ledger_error(st);
        if (committed_effects != event->sequence)
            return MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR;
        cursor = event->sequence;
        ++report.newly_committed;
    }
    {
        uint64_t last = 0u, applied = 0u;
        st = mygames_consumer_projection_snapshot(
            ledger, expected_source_namespace, expected_consumer_id,
            &last, &applied);
        if (st != MYGAMES_CONSUMER_APPLIED)
            return ledger_error(st);
        if (last != cursor || applied != cursor)
            return MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR;
    }

    report.committed_after = cursor;
    report.has_more = cursor < page.checkpoint_sequence ? 1 : 0;
    *out_report = report;
    return report.has_more ? MYGAMES_RECOVERY_CONSUMER_MORE
                           : MYGAMES_RECOVERY_CONSUMER_OK;
}
