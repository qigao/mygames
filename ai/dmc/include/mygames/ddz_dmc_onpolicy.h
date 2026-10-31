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

#ifdef __cplusplus
}
#endif
#endif
