#ifndef MYGAMES_RECOVERY_CONSUMER_H
#define MYGAMES_RECOVERY_CONSUMER_H
#include "mygames/consumer_projection.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Test-qualified *application-side* admission of a previously received and
 * TLS-authenticated canonical HTTP 200 recovery page. No network operations,
 * direct source DB access, MQTT ACK or source Outbox mutation.
 *
 * Caller guarantees exclusive per-(namespace, consumer) ledger ownership:
 * do not run concurrently with a live Flowie MQTT callback or another
 * recovery worker. The caller must verify TLS CA/SAN, client identity and
 * HTTP 200 before invoking this function. A subsequent versioned CHttp client
 * is tracked separately by MyGames #6.
 */
typedef enum mygames_recovery_consumer_status {
    MYGAMES_RECOVERY_CONSUMER_OK = 0,
    MYGAMES_RECOVERY_CONSUMER_MORE,
    MYGAMES_RECOVERY_CONSUMER_INVALID,
    MYGAMES_RECOVERY_CONSUMER_BAD_PAGE,
    MYGAMES_RECOVERY_CONSUMER_LEDGER_ERROR,
    MYGAMES_RECOVERY_CONSUMER_COMMIT_UNKNOWN
} mygames_recovery_consumer_status;

typedef struct mygames_recovery_consumer_report {
    uint64_t committed_before;
    uint64_t committed_after;
    uint64_t source_checkpoint_sequence;
    uint64_t newly_committed;
    int has_more; /* not permission to advance/skip an HTTP page */
} mygames_recovery_consumer_report;

/* Snapshot() obtains the actual durable consumer cursor, which becomes the
 * page decoder's exact expected after_seq. A response's resume_after hint
 * is NOT used as durable progress. Each event is individually committed as
 * receipt SHA256 + demonstration effect + last_seq in one TurboDB transaction.
 *
 * max_events_per_call in [1,16] bounds partial progress. On a crash after k
 * commits, next process MUST reopen the ledger and issue GET after the newly
 * persisted cursor, never reuse an in-memory report. On ANY error,
 * out_report remains untouched; the supervisor must resnapshot its ledger.
 * COMMIT_UNKNOWN is terminal/quarantined, not safe to auto-retry.
 */
mygames_recovery_consumer_status
mygames_recovery_consumer_apply_response(
    mygames_consumer_projection *ledger,
    const char *expected_source_namespace, const char *expected_consumer_id,
    const void *http_response_bytes, size_t response_size,
    uint32_t max_events_per_call,
    mygames_recovery_consumer_report *out_report);

#ifdef __cplusplus
}
#endif
#endif
