#ifndef MYGAMES_DDZ_AI_FEATURES_H
#define MYGAMES_DDZ_AI_FEATURES_H

#include "mygames/ddz_ai_actions.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_DDZ_AI_HISTORY_MOVES 15u
#define MYGAMES_DDZ_AI_CARD_FEATURES 54u
#define MYGAMES_DDZ_AI_HISTORY_FEATURES (MYGAMES_DDZ_AI_HISTORY_MOVES * 54u)
#define MYGAMES_DDZ_AI_LANDLORD_FEATURES 319u
#define MYGAMES_DDZ_AI_FARMER_FEATURES 430u
#define MYGAMES_DDZ_AI_MAX_FEATURES 430u

typedef enum mygames_ddz_ai_role {
    MYGAMES_DDZ_AI_LANDLORD = 0,
    MYGAMES_DDZ_AI_LANDLORD_UP = 1,
    MYGAMES_DDZ_AI_LANDLORD_DOWN = 2
} mygames_ddz_ai_role;

/* PUBLIC-only, monotonically extended after real accepted/COMMITTED moves.
 * This is a small memory projection, NOT a durable journal. If an owner
 * restarts, reconstruct from trusted committed move history: never invent
 * preceding moves from the latest private checkpoint or M5h score ledger. */
typedef struct mygames_ddz_ai_history {
    mygames_ddz_public_view current;
    uint64_t played_by_seat[MYGAMES_DDZ_SEATS];
    uint64_t last_action_by_seat[MYGAMES_DDZ_SEATS]; /* zero on PASS */
    uint64_t recent_actions[MYGAMES_DDZ_AI_HISTORY_MOVES]; /* oldest-first */
    uint8_t recent_seats[MYGAMES_DDZ_AI_HISTORY_MOVES];
    uint8_t recent_count;
    uint8_t bomb_count;
} mygames_ddz_ai_history;

/* Caller must already have authenticated the seat before supplying its hand.
 * No match pointer, secret deck, third-party private hand, RNG state, or
 * persisted credentials can be transported in this type. */
typedef struct mygames_ddz_ai_observation {
    mygames_ddz_ai_history public_history;
    uint64_t own_hand;
    mygames_ddz_ai_role role;
} mygames_ddz_ai_observation;

/* x_no_action and z are shape-compatible with original DouZero's
 * 319 / 430 (role-dependent) and 5x162 representations, respectively.
 * Inputs are OWN hand, combined unknown opponent cards derived by subtraction
 * from the public played-card union, and public-only ordered history.
 * This is NOT yet a claim of checkpoint-weight or house-rule compatibility. */
typedef struct mygames_ddz_ai_features {
    int8_t x_no_action[MYGAMES_DDZ_AI_MAX_FEATURES];
    int8_t z[MYGAMES_DDZ_AI_HISTORY_FEATURES];
    size_t x_count;
    mygames_ddz_ai_role role;
} mygames_ddz_ai_features;

mygames_ddz_ai_status mygames_ddz_ai_history_init(
    const mygames_ddz_public_view *initial,
    mygames_ddz_ai_history *out);

/* Supply an accepted rule result and public view of the very next turn.
 * The caller may publish this projection only AFTER confirmed Game COMMIT.
 * Invalid/stale/noncontiguous events leave the history byte-identical. */
mygames_ddz_ai_status mygames_ddz_ai_history_append(
    mygames_ddz_ai_history *history,
    const mygames_ddz_action_result *accepted,
    const mygames_ddz_public_view *next_view);

/* Rebuild a canonical PUBLIC action projection after restart from trusted
 * committed play/pass rows. This is NOT game authority: it checks the
 * chronological rows using existing pattern/beat logic and validates
 * the reconstructed public view against the caller's authenticated,
 * authoritative current-round snapshot.
 *
 * Each row is a public accepted action; there are at most 162 turns.
 * No match pointer, secret hands/deck, Owner, or ORM handle crosses here.
 * A stale/missing/malformed event stream fails CLOSED, and output is
 * untouched on failure. */
#define MYGAMES_DDZ_AI_MAX_PUBLIC_MOVES 162u
typedef struct mygames_ddz_ai_public_move {
    uint64_t turn_id;
    uint64_t cards_mask;  /* 0 is PASS */
    uint8_t actor_seat;
} mygames_ddz_ai_public_move;

mygames_ddz_ai_status mygames_ddz_ai_history_reconstruct(
    uint8_t landlord_seat,
    const mygames_ddz_ai_public_move *accepted_moves,
    size_t move_count,
    const mygames_ddz_public_view *expected_authoritative_view,
    mygames_ddz_ai_history *out);

mygames_ddz_ai_status mygames_ddz_ai_observe(
    uint64_t authenticated_own_hand,
    const mygames_ddz_ai_history *public_history,
    mygames_ddz_ai_observation *out);

mygames_ddz_ai_status mygames_ddz_ai_encode_douzero(
    const mygames_ddz_ai_observation *observation,
    mygames_ddz_ai_features *out);

/* 54-dimensional rank-count action encoding; PASS -> all zeros.
 * Validates ownership/classification/response before writing output.
 * Physical suits are never features in DouZero's rank-only action head. */
mygames_ddz_ai_status mygames_ddz_ai_encode_action(
    const mygames_ddz_ai_observation *observation,
    uint64_t proposed_action,
    int8_t out[MYGAMES_DDZ_AI_CARD_FEATURES]);

#ifdef __cplusplus
}
#endif
#endif
