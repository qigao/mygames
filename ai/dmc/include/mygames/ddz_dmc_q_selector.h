#ifndef MYGAMES_DDZ_DMC_Q_SELECTOR_H
#define MYGAMES_DDZ_DMC_Q_SELECTOR_H

#include "mygames/ddz_dmc_selfplay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_DDZ_DMC_Q_BATCH 64u
#define MYGAMES_DDZ_DMC_Q_MAX_ACTIONS 65536u

typedef enum mygames_ddz_dmc_q_status {
    MYGAMES_DDZ_DMC_Q_OK = 0,
    MYGAMES_DDZ_DMC_Q_INVALID,
    MYGAMES_DDZ_DMC_Q_CAPACITY,
    MYGAMES_DDZ_DMC_Q_STALE,
    MYGAMES_DDZ_DMC_Q_INVALID_ACTION,
    MYGAMES_DDZ_DMC_Q_MODEL_FAILED,
    MYGAMES_DDZ_DMC_Q_NONFINITE
} mygames_ddz_dmc_q_status;

/* A SELECTED, immutable model snapshot supplied by trusted offline/native
 * inference infrastructure. No fake built-in score engine or rule fallback.
 * The scorer sees ONLY role-specific x/z state features and canonical
 * rank-count features of up to 64 previously-authorized legal actions.
 * No full match pointer, hidden hand partition, shuffled deck, HTTP/DB or
 * opponent's private state is accessible via this interface.
 *
 * The callback writes exactly count floats. Any callback failure or NaN/Inf
 * rejects the ENTIRE decision without changing the output mask.
 * Every required model/rules version and acting turn is fenced by caller. */
typedef bool (*mygames_ddz_dmc_q_score_batch_fn)(
    void *context,
    const mygames_ddz_ai_features *actor_features,
    const int8_t *action_features, /* row-major, 54 bytes per action */
    size_t action_count,
    float *out_scores);

typedef struct mygames_ddz_dmc_q_provider {
    uint64_t model_version;
    uint32_t rules_version;
    mygames_ddz_ai_role role;
    mygames_ddz_dmc_q_score_batch_fn score_batch;
    void *context;
} mygames_ddz_dmc_q_provider;

/* argmax across the WHOLE legal-action list with exact bounded 64-action
 * inference batches. Score ties choose the lower physical mask deterministically.
 * Never truncate to Top-K and never choose a rule/random fallback. The
 * responsible existing CFlow Owner must independently recheck epoch/game/
 * turn/seat and call the sole authoritative round_submit/Game COMMIT.
 *
 * legal_actions must be the full list from mygames_ddz_ai_legal_actions()
 * for this observation, not a client-provided subset. The chooser checks
 * every candidate against acting-hand ownership and the original v1 rules. */
mygames_ddz_dmc_q_status mygames_ddz_dmc_q_pick(
    const mygames_ddz_ai_observation *observation,
    const uint64_t *legal_actions, size_t legal_count,
    uint64_t required_turn_id, uint64_t required_model_version,
    uint32_t required_rules_version,
    const mygames_ddz_dmc_q_provider *provider,
    uint64_t *out_selected);

/* Adapter into existing native OFFLINE Self-Play only. No model creates a
 * gameplay ACK, owns a Room, or writes data. All three immutable roles
 * must be supplied; the callback returns false on ANY score/fence failure.
 * No automatic best-legal/fallback engine is ever selected. */
typedef struct mygames_ddz_dmc_q_policy {
    mygames_ddz_dmc_q_provider providers[MYGAMES_DDZ_SEATS];
    uint64_t required_model_version;
    uint32_t required_rules_version;
    mygames_ddz_dmc_q_status last_status;
    size_t admitted_decisions;
} mygames_ddz_dmc_q_policy;

bool mygames_ddz_dmc_q_policy_select(
    void *opaque, const mygames_ddz_ai_observation *observation,
    const uint64_t *legal_actions, size_t legal_count,
    uint64_t *out_selected_action);

#ifdef __cplusplus
}
#endif
#endif
