#ifndef MYGAMES_DOUZERO_REMOTE_TICKET_H
#define MYGAMES_DOUZERO_REMOTE_TICKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure C11, independent of CHttp, KFCore and all online game headers.
 * It is a bounded OFFLINE/OWNER-INTEGRATION-READY *response correlator*,
 * NOT an online Game Owner, service, move classifier or Game COMMIT.
 *
 * The single serialized Game Owner must create every identity from its
 * authenticated current turn, reserve unique ticket + wire IDs, compute
 * legal_list_sha256 over the FULL ordered native legal-action list, and
 * snapshot ONLY actor-private/public feature vectors. Never accept client-
 * chosen identities or copy another player's private hand to a remote host.
 * SHA-256 digests in this API are caller-computed; this module does not
 * pretend to authenticate a hash, model package, or the inference service.
 */
#define MYGAMES_DOUZERO_REMOTE_PROTOCOL_V1 UINT32_C(1)
#define MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS 64u
#define MYGAMES_DOUZERO_REMOTE_MAX_ACTIONS 16384u
#define MYGAMES_DOUZERO_REMOTE_SHA256_BYTES 32u
#define MYGAMES_DOUZERO_REMOTE_PROFILE_ORIGINAL_WP_V1 UINT32_C(1)

typedef enum mygames_douzero_remote_status {
    MYGAMES_DOUZERO_REMOTE_OK = 0,
    MYGAMES_DOUZERO_REMOTE_INVALID,
    MYGAMES_DOUZERO_REMOTE_BUSY,
    MYGAMES_DOUZERO_REMOTE_STALE,
    MYGAMES_DOUZERO_REMOTE_DEADLINE,
    MYGAMES_DOUZERO_REMOTE_CAPACITY,
    MYGAMES_DOUZERO_REMOTE_INCOMPLETE,
    MYGAMES_DOUZERO_REMOTE_NONFINITE,
    MYGAMES_DOUZERO_REMOTE_REPLAY,
    MYGAMES_DOUZERO_REMOTE_CANCELLED
} mygames_douzero_remote_status;

/* Strict canonical fields reflected by BOTH H2/JSON-RPC and WS responses.
 * 'role': 0=landlord, 1=farmer_up (counterclockwise from landlord),
 *         2=farmer_down (clockwise from landlord).
 * 'seat' and 'landlord_seat' are physical native seat indices.
 * The fixed original upstream WP rule profile is not automatically
 * qualified against the current M5j online Round/Match rules. */
typedef struct mygames_douzero_remote_identity {
    uint32_t protocol_version;
    uint32_t rules_profile;
    uint64_t ticket_id;
    uint64_t wire_request_id_base; /* chunk i uses base+i; no reuse */
    uint64_t room_generation;
    uint64_t owner_epoch;
    uint64_t transport_generation;
    uint64_t game_id;
    uint64_t turn_id;
    uint64_t model_version;
    uint32_t legal_action_count;
    uint8_t seat;
    uint8_t landlord_seat;
    uint8_t role;
    uint8_t model_sha256[MYGAMES_DOUZERO_REMOTE_SHA256_BYTES];
    uint8_t legal_list_sha256[MYGAMES_DOUZERO_REMOTE_SHA256_BYTES];
} mygames_douzero_remote_identity;

typedef struct mygames_douzero_remote_reply_chunk {
    mygames_douzero_remote_identity identity;
    uint64_t wire_request_id;
    uint32_t chunk_index;
    uint32_t chunk_count;
    size_t score_count;
    const float *scores; /* borrowed only while callback executes */
} mygames_douzero_remote_reply_chunk;

/* The caller owns 'scratch' and its lifetime through reset(). The ledger
 * owns NO networking, allocations, threads, or game state. A single Owner
 * serial executor must call ALL functions (not thread safe). */
typedef struct mygames_douzero_remote_ticket {
    mygames_douzero_remote_identity identity;
    float *score_scratch;
    size_t score_capacity;
    uint64_t absolute_deadline_ms; /* owner monotonic local clock */
    uint32_t next_chunk;
    uint8_t phase; /* private contract: always initialize to zero */
} mygames_douzero_remote_ticket;

/* Only a ZERO ticket slot may begin. Invalid input leaves slot, scratch and
 * external output wholly unchanged. A caller must provide a bounded,
 * owner-monotonic absolute deadline strictly beyond now. */
mygames_douzero_remote_status mygames_douzero_remote_begin(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_identity *identity,
    float *score_scratch, size_t score_capacity,
    uint64_t absolute_deadline_ms, uint64_t now_ms);

/* The CHttp Owner sends ONLY this next chunk and can keep at most one
 * native wire request active per turn. A callback MUST NOT publish from
 * another thread; copy its response to the serialized Game Owner. */
mygames_douzero_remote_status mygames_douzero_remote_expected_chunk(
    const mygames_douzero_remote_ticket *ticket, uint64_t now_ms,
    uint64_t *out_wire_request_id, uint32_t *out_chunk_index,
    uint32_t *out_chunk_count, size_t *out_score_count);

/* Validate EVERY echo field, exact chunk order/count/wire request ID, all
 * finite scores, digest & transport connection generation. Reject output
 * without modifying scratch or advancing state; no retry or model fallback.
 * After the last chunk, the ticket is ready for ONE best-index read. */
mygames_douzero_remote_status mygames_douzero_remote_accept(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_reply_chunk *reply, uint64_t now_ms);

/* Returns ONLY the original legal-list index after the complete scores and
 * current authoritative Owner identity are confirmed. Deterministic ties:
 * smallest canonical legal-action index. The Owner must STILL revalidate
 * action legality and use its existing Game Owner play/COMMIT/ACK API.
 * Failure never writes out_index, never commits, and never sends PASS. */
mygames_douzero_remote_status mygames_douzero_remote_best_index(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_identity *current_owner,
    uint64_t now_ms, size_t *out_index);

/* Caller first cancels any in-flight CHttp request using its ordinary
 * request handle. Invalidate is immediate and permanent even if a late
 * callback arrives. Reset is allowed only after cancel or success. */
mygames_douzero_remote_status mygames_douzero_remote_cancel(
    mygames_douzero_remote_ticket *ticket);
mygames_douzero_remote_status mygames_douzero_remote_reset(
    mygames_douzero_remote_ticket *ticket);

#ifdef __cplusplus
}
#endif
#endif
