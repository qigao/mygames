#ifndef MYGAMES_CONSUMER_PROJECTION_H
#define MYGAMES_CONSUMER_PROJECTION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Separate consumer-owned TurboDB SQLite database, not the authoritative
 * Room Owner's database. The namespace identifies ONE stable upstream
 * game-event sequence across publisher/broker restarts. */
#define MYGAMES_CONSUMER_IDENTIFIER_MAX 64u
#define MYGAMES_CONSUMER_EVENT_DIGEST_BYTES 32u

typedef struct mygames_consumer_projection mygames_consumer_projection;

typedef enum mygames_consumer_status {
    MYGAMES_CONSUMER_APPLIED = 0, /* new unique event + projection committed */
    MYGAMES_CONSUMER_REPLAY,      /* same namespace/seq and digest */
    MYGAMES_CONSUMER_INVALID,
    MYGAMES_CONSUMER_SEQUENCE_GAP,
    MYGAMES_CONSUMER_CONFLICT,
    MYGAMES_CONSUMER_CORRUPT,
    MYGAMES_CONSUMER_BACKEND_ERROR,
    MYGAMES_CONSUMER_COMMIT_UNKNOWN,
    MYGAMES_CONSUMER_QUARANTINED
} mygames_consumer_status;

mygames_consumer_status mygames_consumer_projection_open(
    const char *sqlite_driver_absolute_path, const char *database_path,
    mygames_consumer_projection **out);
void mygames_consumer_projection_close(mygames_consumer_projection *store);

/* Demonstration projection = count of unique applied events. In one
 * SERIALIZABLE TurboDB transaction:
 * - consume seq=(last_seq+1), insert its SHA256 digest and increment count
 * - an identical prior seq/digest returns REPLAY without side effects
 * - same seq with different digest is CONFLICT (not benign duplicate)
 * - missing/unordered seq returns SEQUENCE_GAP; no silent skip
 *
 * Future real projections MUST be updated in this same transaction,
 * never directly in a Flowie MQTT callback.
 */
mygames_consumer_status mygames_consumer_projection_apply(
    mygames_consumer_projection *store,
    const char *source_namespace, const char *consumer_id,
    uint64_t committed_sequence,
    const uint8_t event_sha256[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES],
    uint64_t *out_applied_count);

/* Strictly read-only and no implied network/room authority. */
mygames_consumer_status mygames_consumer_projection_snapshot(
    mygames_consumer_projection *store,
    const char *source_namespace, const char *consumer_id,
    uint64_t *out_last_sequence, uint64_t *out_applied_count);

#ifdef MYGAMES_CONSUMER_FAULT_TESTING
/* One-shot failure after receipt INSERT, before durable count/cursor UPDATE. */
void mygames_consumer_projection_fail_after_receipt_once(
    mygames_consumer_projection *store);
#endif

#ifdef __cplusplus
}
#endif
#endif
