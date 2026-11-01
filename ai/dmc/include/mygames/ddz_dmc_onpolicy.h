#ifndef MYGAMES_DDZ_DMC_ONPOLICY_H
#define MYGAMES_DDZ_DMC_ONPOLICY_H

#include "mygames/ddz_dmc_q_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mygames_ddz_dmc_onpolicy_status {
    MYGAMES_DDZ_ONPOLICY_OK = 0,
    MYGAMES_DDZ_ONPOLICY_INVALID,
    MYGAMES_DDZ_ONPOLICY_CAPACITY,
    MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED,
    MYGAMES_DDZ_ONPOLICY_GAME_FAILED,
    MYGAMES_DDZ_ONPOLICY_REPLAY_FAILED
} mygames_ddz_dmc_onpolicy_status;

/* A model gets ONLY the actor's authorized features and the complete, native
 * rules-checked list of legal action features. No match, hidden deck or
 * opponents' hands enter this interface. No heuristic/CPU model fallback. */
typedef bool (*mygames_ddz_dmc_onpolicy_score_fn)(
    void *context, uint32_t role,
    const int8_t *x, size_t x_count,
    const int8_t *z, size_t z_count,
    const int8_t *actions, size_t action_count, float *out_q);

typedef struct mygames_ddz_dmc_onpolicy_config {
    uint64_t episode_id;
    uint64_t frozen_model_version;  /* behavior/source model, not future target */
    uint32_t rules_version;
    mygames_ddz_dmc_objective objective;
    uint8_t offline_landlord_seat;
} mygames_ddz_dmc_onpolicy_config;

/* FULL genuine OS-CSPRNG-shuffled C11 match: no test entropy injection.
 * Three seats use the SAME exact immutable model version. Every accepted
 * move is selected via MyGames::DMCQSelector and sealed ONLY after the
 * authoritative C11 final settlement. The canonical Replay is returned
 * in caller-owned memory, never persisted/ACKed by the game engine.
 *
 * Fail closed: output Replay bytes, size and decision count are untouched
 * on every error. Querying an insufficient capacity is an explicit error;
 * no partial/truncated Replay is ever published. Private file ACL,
 * checkpoint digest and provenance metadata belong to the trusted caller. */
mygames_ddz_dmc_onpolicy_status mygames_ddz_dmc_onpolicy_collect(
    const mygames_ddz_dmc_onpolicy_config *config,
    mygames_ddz_dmc_onpolicy_score_fn score, void *score_context,
    void *replay_bytes, size_t replay_capacity,
    size_t *out_replay_size, uint32_t *out_accepted_moves);

/* Mixed R9e C11 Replay v1 is a dedicated heterogeneous source:
 * model_version UINT64_MAX is an intentionally untrainable sentinel for the
 * legacy v3 learner, NEVER a claim that all seats used the frozen Q model.
 * A future dedicated trainer MUST verify its private per-seat source record. */
#define MYGAMES_DDZ_DMC_MIXED_REPLAY_SOURCE UINT64_MAX

typedef enum mygames_ddz_dmc_seat_behavior {
    MYGAMES_DDZ_SEAT_FROZEN_Q = 1,
    MYGAMES_DDZ_SEAT_SHED_MOST = 2,
    MYGAMES_DDZ_SEAT_LEGAL_INDEX = 3
} mygames_ddz_dmc_seat_behavior;

typedef struct mygames_ddz_dmc_mixed_config {
    uint64_t episode_id;
    uint64_t frozen_model_version;
    uint32_t rules_version;
    mygames_ddz_dmc_objective objective;
    uint8_t offline_landlord_seat;
    uint8_t seat_behavior[MYGAMES_DDZ_SEATS]; /* physical seat, not role */
} mygames_ddz_dmc_mixed_config;

/* Outcome/public source-only metadata, no private cards or deck. Actual
 * parent Q checkpoint SHA256 must be bound separately by a trusted owner. */
typedef struct mygames_ddz_dmc_mixed_provenance {
    uint64_t episode_id;
    uint64_t frozen_model_version; /* parent Q, not v1 Replay source */
    uint64_t replay_source_marker;
    uint32_t seat_decisions[MYGAMES_DDZ_SEATS];
    uint8_t seat_behavior[MYGAMES_DDZ_SEATS];
    uint8_t landlord_seat;
    uint8_t winner_seat;
} mygames_ddz_dmc_mixed_provenance;

/* Genuine offline OS-CSPRNG match via existing canonical SelfPlay/Match.
 * Requires at least one frozen Q and one shed-most physical seat; optional
 * legal-index third seat. Only actor-authorized state/legal Q features are
 * passed to scorer. No test shuffle, model/heuristic fallback or network.
 * Replay-v1 UINT64_MAX marker is intentionally rejected by legacy trainer,
 * whose candidate model version must be strictly newer than every Replay
 * source version. No mixed-source learner admission until private Replay SHA
 * is bound to all per-seat policies and exact parent model checkpoint SHA.
 * Replay bytes, count and provenance remain UNTOUCHED on every failure. */
mygames_ddz_dmc_onpolicy_status mygames_ddz_dmc_mixed_collect(
    const mygames_ddz_dmc_mixed_config *config,
    mygames_ddz_dmc_onpolicy_score_fn score, void *score_context,
    void *replay_bytes, size_t replay_capacity,
    size_t *out_replay_size, uint32_t *out_accepted_moves,
    mygames_ddz_dmc_mixed_provenance *out_provenance);

#ifdef __cplusplus
}
#endif
#endif
