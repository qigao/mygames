#ifndef MYGAMES_DOUZERO_REMOTE_WIRE_H
#define MYGAMES_DOUZERO_REMOTE_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same binary body over mTLS HTTP/2 POST /v1/douzero/q and authenticated
 * WSS binary /v1/douzero/q. This pure-C11 codec does not own the network,
 * model execution, Game Owner, retries or the legal action enumeration.
 * The model may only score acting-seat features, never a full hidden deck.
 */
#define MYGAMES_DOUZERO_REMOTE_ABI 1u
#define MYGAMES_DOUZERO_REMOTE_HEADER_BYTES 136u
#define MYGAMES_DOUZERO_REMOTE_STATE_LANDLORD 319u
#define MYGAMES_DOUZERO_REMOTE_STATE_FARMER 430u
#define MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES 810u
#define MYGAMES_DOUZERO_REMOTE_ACTION_BYTES 54u
#define MYGAMES_DOUZERO_REMOTE_BATCH_MAX 64u
#define MYGAMES_DOUZERO_REMOTE_TOTAL_ACTIONS_MAX 65536u
#define MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX \
    (MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + \
     MYGAMES_DOUZERO_REMOTE_STATE_FARMER + \
     MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES + \
     MYGAMES_DOUZERO_REMOTE_ACTION_BYTES * MYGAMES_DOUZERO_REMOTE_BATCH_MAX)
#define MYGAMES_DOUZERO_REMOTE_REPLY_BYTES_MAX \
    (MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + \
     4u * MYGAMES_DOUZERO_REMOTE_BATCH_MAX)

typedef enum mygames_douzero_wire_status {
    MYGAMES_DOUZERO_WIRE_OK = 0,
    MYGAMES_DOUZERO_WIRE_INVALID = 1,
    MYGAMES_DOUZERO_WIRE_CAPACITY = 2,
    MYGAMES_DOUZERO_WIRE_STALE = 3,
    MYGAMES_DOUZERO_WIRE_NONFINITE = 4
} mygames_douzero_wire_status;

/* Distinct request ID per full-legal-list chunk; immutable Game Owner
 * identity, generation, model and canonical ordered FULL legal-list SHA256.
 * Hash values must originate from the owning application and be verified by
 * the client; this codec does not calculate or authenticate SHA256.
 */
typedef struct mygames_douzero_ticket {
    uint64_t request_id;
    uint64_t room_epoch;
    uint64_t game_id;
    uint64_t turn_id;
    uint64_t owner_generation;
    uint64_t model_version;
    uint32_t first_action;
    uint32_t total_actions;
    uint16_t action_count;
    uint8_t role;  /* 0 landlord, 1 up, 2 down */
    uint8_t legal_actions_sha256[32];
    uint8_t model_artifact_sha256[32]; /* original DouZero WP ONNX artifact */
} mygames_douzero_ticket;

/* Borrowed request view is only valid as long as frame memory lives. */
typedef struct mygames_douzero_request_view {
    mygames_douzero_ticket ticket;
    const int8_t *x;      /* 319/430 actor-private+public binary features */
    const int8_t *z;      /* 810 public action-history binary features */
    const int8_t *actions;/* count * 54 binary legal action encoding */
} mygames_douzero_request_view;

/* Canonical little-endian scalars and IEEE-754 float32 little-endian Q.
 * All failure paths leave the caller output bytes, size or view unchanged.
 * Response echoes FULL ticket, not just request ID; duplicate/stale chunks
 * cannot be silently accepted by matching a reused network connection.
 */
mygames_douzero_wire_status mygames_douzero_wire_encode_request(
    const mygames_douzero_ticket *ticket,
    const int8_t *x, size_t x_count,
    const int8_t *z, size_t z_count,
    const int8_t *actions, size_t action_bytes,
    void *out, size_t capacity, size_t *out_size);

mygames_douzero_wire_status mygames_douzero_wire_decode_request(
    const void *frame, size_t size, mygames_douzero_request_view *out);

mygames_douzero_wire_status mygames_douzero_wire_encode_reply(
    const mygames_douzero_ticket *ticket, const float *scores,
    size_t score_count, void *out, size_t capacity, size_t *out_size);

/* The authoritative response-admission gate against an owned pending ticket.
 * The Game Owner must STILL collect all batches and re-check its own
 * live room/game/turn/generation, then choose from the entire legal list
 * and revalidate the physical action through original Round/Owner commit.
 */
mygames_douzero_wire_status mygames_douzero_wire_decode_reply(
    const void *frame, size_t size, const mygames_douzero_ticket *expected,
    float *out_scores, size_t score_capacity);

/* Complete original legal-action list collection. A Game Owner may receive
 * HTTP/2 streams or WS responses OUT OF ORDER. This owner-local ledger tracks
 * an exact fixed partition into 64-row batches; each batch has a distinct
 * request ID and is accepted once only. Partial scores MUST NOT be consumed.
 * The owner must compare live turn/generation/model/legal list again AFTER
 * collector_complete() before it selects/commits a physical-card action.
 */
#define MYGAMES_DOUZERO_REMOTE_CHUNKS_MAX 1024u
typedef struct mygames_douzero_score_collector {
    mygames_douzero_ticket initial; /* first_action=0, first batch exact */
    float *scores;                    /* caller-owned total_actions slots */
    size_t score_capacity;
    uint8_t received[MYGAMES_DOUZERO_REMOTE_CHUNKS_MAX / 8u];
    uint16_t received_count;
    uint16_t expected_count;
} mygames_douzero_score_collector;

/* Begin has no effect on caller output if initial identity/buffer invalid.
 * Must use a fresh, non-recycled ID range for every proposal. */
mygames_douzero_wire_status mygames_douzero_score_collector_begin(
    const mygames_douzero_ticket *first_batch, float *scores,
    size_t score_capacity, mygames_douzero_score_collector *out);

/* Exact expected batch ticket (unique ID base+i, first=i*64), no fallback. */
mygames_douzero_wire_status mygames_douzero_score_collector_ticket(
    const mygames_douzero_score_collector *collector,
    size_t batch_index, mygames_douzero_ticket *out);

/* Accept arbitrary order, but never missing/overlapping/duplicate frames.
 * Any bad frame leaves all collected Q slots and the received bitmap alone. */
mygames_douzero_wire_status mygames_douzero_score_collector_accept(
    mygames_douzero_score_collector *collector, const void *reply,
    size_t reply_size);

int mygames_douzero_score_collector_complete(
    const mygames_douzero_score_collector *collector);

#ifdef __cplusplus
}
#endif
#endif
