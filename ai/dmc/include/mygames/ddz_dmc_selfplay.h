#ifndef MYGAMES_DDZ_DMC_SELFPLAY_H
#define MYGAMES_DDZ_DMC_SELFPLAY_H

#include "mygames/ddz_dmc_replay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mygames_ddz_dmc_selfplay_status {
    MYGAMES_DDZ_SELFPLAY_OK = 0,
    MYGAMES_DDZ_SELFPLAY_INVALID,
    MYGAMES_DDZ_SELFPLAY_CAPACITY,
    MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED,
    MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED,
    MYGAMES_DDZ_SELFPLAY_TRUNCATED
} mygames_ddz_dmc_selfplay_status;

/* The callback sees only ONE authorized acting player's observation and
 * the complete, rules-valid physical-card action list. It never sees
 * mygames_ddz_match*, hidden opponents' hands or a shuffled deck.
 * Return false on failure, or true with exactly one offered legal mask.
 * The policy may be a test heuristic or a versioned batched inference
 * adapter. No implicit random/legal-first fallback is permitted. */
typedef bool (*mygames_ddz_dmc_policy_fn)(
    void *context, const mygames_ddz_ai_observation *observation,
    const uint64_t *legal_actions, size_t legal_count,
    uint64_t *out_selected_action);

typedef struct mygames_ddz_dmc_selfplay_config {
    uint64_t room_epoch;
    uint64_t game_id;
    uint64_t episode_id;
    uint64_t model_version;
    uint32_t rules_version;
    mygames_ddz_dmc_objective objective;
    size_t max_legal_actions; /* hard admission limit, no silent Top-K */
} mygames_ddz_dmc_selfplay_config;

/* Offline, in-process game simulation: genuine authoritative C11
 * secure deal, bid (current v1 single first-player bid3 baseline),
 * all accepted rule transitions, public action history and terminal
 * nonmonetary settlement. NEVER call on a real CFlow/CHttp room owner.
 * It neither opens SQLite nor publishes gameplay ACK/Outbox, and
 * uses no client-supplied shuffle or production entropy hook.
 *
 * Caller owns sample storage and must discard it on any non-OK result;
 * out_episode and out_settlement remain unchanged on failure.
 * A completed sealed episode is usable by the private Replay v1 codec.
 * Bidding policy/early bidding model are an independent future phase. */
mygames_ddz_dmc_selfplay_status mygames_ddz_dmc_selfplay_run(
    const mygames_room *started_room,
    const mygames_ddz_dmc_selfplay_config *config,
    mygames_ddz_dmc_policy_fn policy, void *policy_context,
    mygames_ddz_dmc_sample *owned_samples, size_t sample_capacity,
    mygames_ddz_dmc_episode *out_episode,
    mygames_ddz_settlement *out_settlement);

#ifdef __cplusplus
}
#endif
#endif
