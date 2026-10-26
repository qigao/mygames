#ifndef MYGAMES_ROOM_STORE_H
#define MYGAMES_ROOM_STORE_H
#include "mygames/room_checkpoint.h"
#include "mygames/ddz_match_checkpoint.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_STORE_COMMAND_DIGEST_BYTES 32u
#define MYGAMES_STORE_REQUEST_ID_MAX 48u
#define MYGAMES_STORE_RESULT_MAX 1024u
#define MYGAMES_STORE_EVENT_MAX 1024u
#define MYGAMES_STORE_ARCHIVE_PAGE_MAX 16u

/* Fixed-capacity immutable snapshot of exact committed Outbox events.
 * A single readonly SQLite transaction pins checkpoint and page bytes.
 * Receiver MUST still validate canonical public schema before responding.
 * All history is currently retained: pruning/retention watermark != supported. */
typedef struct mygames_store_archive_item {
    uint64_t sequence;
    size_t size;
    uint8_t payload[MYGAMES_STORE_EVENT_MAX];
} mygames_store_archive_item;

typedef struct mygames_store_archive_page {
    uint64_t epoch;
    uint64_t checkpoint_sequence;
    uint64_t retained_from_sequence;
    uint64_t resume_after_sequence;
    size_t count;
    int has_more;
    mygames_store_archive_item items[MYGAMES_STORE_ARCHIVE_PAGE_MAX];
} mygames_store_archive_page;


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
    MYGAMES_STORE_QUARANTINED,
    MYGAMES_STORE_READ_ONLY,
    MYGAMES_STORE_STALE_GAME
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

/* SOURCE ARCHIVE READER ONLY. The installed TurboDB SQLite driver is opened
 * with open_mode=read_only (SQLITE_OPEN_READONLY); no schema creation, WAL
 * configuration, checkpoint publication, session mutation or Outbox ACK.
 * An existing source must pass the full checkpoint/journal verification.
 * Any mutating call on this handle rejects with MYGAMES_STORE_READ_ONLY.
 *
 * This is NOT consumer authentication or a remotely accessible recovery
 * service. Call from an isolated trusted backend only; a production service
 * must separately enforce principal, namespace, ACL, rate and retention.
 */
mygames_store_status mygames_room_store_open_readonly(
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

/* Controlled cleanup of a just-issued credential for which the HELLO was
 * never admitted. Caller MUST assert this player has no room membership.
 * Only removes attached, generation-matching provisional leases.
 */
mygames_store_status mygames_room_store_discard_unsent_session(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id player, uint64_t generation);

/* Enumerate and delete expired detached leases that own NO room seat. The
 * caller must check room membership on the unique CFlow Room Owner before
 * deleting. Neither function modifies RoomDirectory or command journal.
 * after_player enables bounded, non-spinning sweeps past seated expirations.
 */
mygames_store_status mygames_room_store_next_expired_session(
    mygames_room_store *store, uint64_t now_realtime_ms,
    mygames_player_id after_player,
    mygames_player_id *out_player, uint64_t *out_generation);

mygames_store_status mygames_room_store_delete_expired_unseated(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id player, uint64_t generation,
    uint64_t now_realtime_ms);

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

/* M5c -- PRIVATE game/room/receipt/Outbox single TurboDB transaction.
 * A non-NULL expected_previous_match must match the *exact canonical bytes*
 * stored for this room. NULL is accepted only for its first ever match.
 * A later fresh match requires the previous private game to have finished
 * or require an all-pass redeal and a larger game_id.
 *
 * post_command_rooms must contain the STARTING room with exactly the three
 * authoritative match players in their original seats. An in-memory CFlow
 * Owner computes the candidate using mygames_ddz_match_bid/play first;
 * a network callback/model proposal MUST NOT call this API.
 *
 * All DB work, including private game bytes, room checkpoint, exact command
 * outcome and PUBLIC-only sanitized Outbox event, commits atomically.
 * Only STORE_OK means the caller can replace its live match and send Game
 * ACK. STORE_REPLAY returns an earlier committed sequence; load the exact
 * original response with get_outcome, never send the candidate result.
 * STORE_COMMIT_UNKNOWN quarantines admission without guessing/retrying.
 */
mygames_store_status mygames_room_store_commit_game(
    mygames_room_store *store, mygames_player_id authenticated_player,
    const mygames_room_directory *post_command_rooms,
    const char *room_id,
    const mygames_ddz_match *expected_previous_match,
    const mygames_ddz_match *candidate_match,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    const void *sanitized_event, size_t event_bytes,
    uint64_t *out_committed_sequence);

/* Loads one verified private match from the SAME TurboDB store and checks
 * its epoch/game/sequence against the checkpoint. The caller must ensure
 * secret bytes stay inside its unique, authenticated Game Owner; never
 * serialize this private POD into MQTT/public WebSocket or logs.
 * No durable game for the room returns STORE_NOT_FOUND.
 * Invalid outputs remain unchanged on errors.
 */
mygames_store_status mygames_room_store_load_game(
    mygames_room_store *store, const char *room_id,
    mygames_ddz_match *out_match, uint64_t *out_sequence);

/* One authoritative, journalled timeout command for an expired WAITING
 * room participant. Atomically removes the detached hashed lease, applies
 * the already-validated post-removal room snapshot and stores an exact system
 * outcome + unpublished Outbox event. Never delete a seated lease separately.
 *
 * The DB verifies the lease's player/generation/expiry and the previous
 * persisted snapshot actually contains this member. The resulting snapshot
 * must no longer contain the member. COMMIT_UNKNOWN quarantines the owner.
 */
mygames_store_status mygames_room_store_expire_seated(
    mygames_room_store *store,
    const mygames_room_directory *post_expiry_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    mygames_player_id expired_player, uint64_t lease_generation,
    uint64_t now_realtime_ms,
    const char *system_request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *outcome, size_t outcome_bytes,
    const void *sanitized_event, size_t event_bytes,
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
/* Privileged local recovery/read API: retrieve an immutable, already
 * COMMITTED event by exact global sequence, INCLUDING events whose Outbox
 * `delivered=1`. This reads the original canonical bytes without changing
 * delivery state or ACKing a client.
 *
 * The source database must be made available only to a trusted backend.
 * This API does not authorize an MQTT subscriber or validate its principal.
 * An absent event at seq<=durable checkpoint seq is corruption, not an
 * empty result. A future authenticated backfill service should expose
 * this capability without sharing direct source DB write credentials.
 */
mygames_store_status mygames_room_store_get_committed_event(
    mygames_room_store *store, uint64_t event_sequence,
    void *payload, size_t capacity, size_t *out_size);

/* Must be a read-only store handle. One SQLite read transaction sees both
 * the verified checkpoint (including complete contiguous archive checks)
 * and the returned 1..16 exact committed event bytes, even if the Room
 * Owner concurrently publishes new transactions. Does not mutate a cursor,
 * Outbox delivery status, Room Directory or consumer projection.
 *
 * after_sequence is a hint from the **consumer's durable DB cursor**.
 * An after_sequence beyond the source snapshot returns STALE_SEQUENCE.
 * No history pruning or retention gaps are presently supported.
 */
mygames_store_status mygames_room_store_read_archive_page(
    mygames_room_store *store, uint64_t after_sequence,
    uint32_t max_events, mygames_store_archive_page *out_page);

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
