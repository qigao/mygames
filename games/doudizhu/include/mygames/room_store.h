#ifndef MYGAMES_ROOM_STORE_H
#define MYGAMES_ROOM_STORE_H
#include "mygames/room_checkpoint.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_STORE_COMMAND_DIGEST_BYTES 32u
#define MYGAMES_STORE_REQUEST_ID_MAX 48u
#define MYGAMES_STORE_RESULT_MAX 1024u
#define MYGAMES_STORE_EVENT_MAX 1024u

typedef struct mygames_room_store mygames_room_store;

typedef enum mygames_store_status {
    MYGAMES_STORE_OK = 0,
    MYGAMES_STORE_INVALID,
    MYGAMES_STORE_BACKEND_ERROR,
    MYGAMES_STORE_UNINITIALIZED,
    MYGAMES_STORE_CORRUPT,
    MYGAMES_STORE_STALE_EPOCH,
    MYGAMES_STORE_STALE_SEQUENCE,
    MYGAMES_STORE_REPLAY,
    MYGAMES_STORE_REQUEST_CONFLICT,
    MYGAMES_STORE_NOT_FOUND,
    MYGAMES_STORE_CAPACITY,
    /* A dispatched commit can have landed. The connection is quarantined:
     * do not replay the operation without independent reconciliation. */
    MYGAMES_STORE_COMMIT_UNKNOWN,
    MYGAMES_STORE_QUARANTINED
} mygames_store_status;

/* One writer: call only from the unique Room Owner, not a CHttp callback.
 *
 * Opens the exact supplied SQLite plugin through TurboDB ORM Runtime (no
 * driver scanning/fallback) and an explicitly supplied datastore path. Schema
 * bootstrap and initial empty checkpoint occur transactionally on new stores.
 * Existing stores are validated rather than silently repaired or reset.
 */
mygames_store_status mygames_room_store_open(
    const char *sqlite_plugin_absolute_path, const char *database_path,
    mygames_room_store **out_store);

/* Caller releases only after every transaction and query is complete. */
void mygames_room_store_close(mygames_room_store *store);
const char *mygames_room_store_last_error(const mygames_room_store *store);

/* Full validation, then atomic publication into caller-owned output only. */
mygames_store_status mygames_room_store_load(
    mygames_room_store *store, mygames_room_directory *out_rooms,
    uint64_t *out_epoch, uint64_t *out_sequence);

/* Explicit durable Owner Epoch claim via compare-and-swap, including a
 * re-encoded matching epoch inside the checkpoint in the same transaction.
 * A stale owner cannot commit after another owner advances the epoch.
 * Does not elect an owner; external authority must decide who may claim.
 */
/* Highest committed player ID in the durable command journal. A cold
 * development gateway uses this to avoid reissuing old numeric identities.
 * No session credentials are restored by this function.
 */
mygames_store_status mygames_room_store_player_high_watermark(
    mygames_room_store *store, mygames_player_id *out_player);

mygames_store_status mygames_room_store_claim_epoch(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t *out_claimed_epoch);

/* Development-only durable bearer lease contract. The SQLite DB stores
 * ONLY SHA-256 of 256-bit random tickets (never plaintext). All calls run
 * on the unique Room Owner. No external principal authentication is implied.
 *
 * The identity high-watermark is allocated transactionally and never reused
 * even after a session row is deleted. Epoch CAS fences superseded owners.
 * A server restart atomically detaches formerly connected sessions and
 * grants only a bounded, absolute wall-clock grace to present their tickets.
 */
#define MYGAMES_STORE_TICKET_HASH_BYTES 32u

mygames_store_status mygames_room_store_issue_session(
    mygames_room_store *store, uint64_t epoch,
    const uint8_t token_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    mygames_player_id *out_allocated_player);

mygames_store_status mygames_room_store_detach_session(
    mygames_room_store *store, uint64_t epoch, mygames_player_id player,
    uint64_t generation, uint64_t expires_at_realtime_ms);

mygames_store_status mygames_room_store_resume_session(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id temporary_player,
    const uint8_t old_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    const uint8_t rotated_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    uint64_t now_realtime_ms,
    mygames_player_id *out_player, uint64_t *out_generation);

mygames_store_status mygames_room_store_has_session(
    mygames_room_store *store, mygames_player_id player);

/* Atomically updates the owner epoch in Checkpoint and transitions all old
 * attached leases into detached, time-limited recovery capabilities. The
 * deadline is computed by caller from a strictly bounded realtime grace.
 */
mygames_store_status mygames_room_store_claim_epoch_for_recovery(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t recovery_deadline_realtime_ms, uint64_t *out_claimed_epoch);

/* Atomic transaction covering:
 *   1. optimistic epoch+sequence CAS on durable checkpoint
 *   2. durable command ID + 32-byte semantic digest + exact response
 *   3. durable, unpublished outbox event under the SAME sequence
 * Only OK confirms the whole transaction committed.
 *
 * The caller must supply a *post-command*, already-validated room state,
 * normalized request digest, exact result and sanitized public event. Nothing
 * mutates the live RoomDirectory here. No MQTT publish occurs here.
 * A duplicate normalized command (SHA-256 digest match) returns REPLAY + prior
 * durable seq without changing facts; recover the ORIGINAL reply using
 * mygames_room_store_get_outcome(). A conflicting digest returns CONFLICT.
 * Command IDs are unique per player, not across unrelated player sessions.
 */
mygames_store_status mygames_room_store_commit(
    mygames_room_store *store, mygames_player_id player_id,
    const mygames_room_directory *post_command_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    const void *event_payload, size_t event_bytes,
    uint64_t *out_committed_sequence);

/* Read one previously committed outcome without executing game logic. */
mygames_store_status mygames_room_store_get_outcome(
    mygames_room_store *store, mygames_player_id player_id,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    void *result, size_t result_capacity, size_t *out_result_size,
    uint64_t *out_sequence);

/* Read the first unpublished event with seq > after_sequence. Its bytes are
 * borrowed only for the call and copied into caller memory.
 * ACK is a separate transaction and must happen only after the Flowie
 * publisher has observed the selected delivery settlement. */
mygames_store_status mygames_room_store_next_event(
    mygames_room_store *store, uint64_t after_sequence,
    void *payload, size_t capacity, size_t *out_size, uint64_t *out_sequence);
mygames_store_status mygames_room_store_ack_event(
    mygames_room_store *store, uint64_t epoch, uint64_t sequence);

/* Test only. Production binary must not enable MYGAMES_STORE_FAULT_TESTING.
 * One-shot injection after checkpoint UPDATE but before command/outbox INSERT.
 */
#ifdef MYGAMES_STORE_FAULT_TESTING
void mygames_room_store_fail_after_checkpoint_once(mygames_room_store *store);
void mygames_room_store_fail_after_command_once(mygames_room_store *store);
#endif
#ifdef __cplusplus
}
#endif
#endif
