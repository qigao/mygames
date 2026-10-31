#ifndef MYGAMES_DDZ_DMC_TOURNAMENT_H
#define MYGAMES_DDZ_DMC_TOURNAMENT_H
#include "mygames/ddz_dmc_q_selector.h"
#ifdef __cplusplus
extern "C" {
#endif

/* TEST-ONLY source: the existing M5a/M5b/M5g rules are the sole
 * authoritative game engine. A deterministic shuffle is admitted only
 * through MYGAMES_DDZ_MATCH_TESTING, never a live Owner. No secret deck,
 * full Match or hidden opponents' hands can reach the scorer callback.
 * The scorer sees only the acting player's role + x/z/action features. */
typedef bool (*mygames_ddz_tournament_score_fn)(
    void *context, uint32_t role, const int8_t *x, size_t x_count,
    const int8_t *z, size_t z_count, const int8_t *action_rows,
    size_t action_count, float *out_q);

typedef enum mygames_ddz_tournament_status {
    MYGAMES_DDZ_TOURNAMENT_OK = 0,
    MYGAMES_DDZ_TOURNAMENT_INVALID,
    MYGAMES_DDZ_TOURNAMENT_POLICY_FAILED,
    MYGAMES_DDZ_TOURNAMENT_ENGINE_FAILED
} mygames_ddz_tournament_status;

typedef struct mygames_ddz_tournament_config {
    uint32_t shuffled_seed; /* nonzero TEST-only shuffle identity */
    uint8_t landlord_seat; /* exactly 0..2 */
    uint8_t candidate_landlord; /* 1 candidate landlord / 0 farmers */
    uint8_t baseline_type; /* 0 shed-most, 1 deterministic legal index */
    uint8_t identity_null; /* 1: frozen model controls all three seats */
    uint64_t candidate_model_version;
    uint32_t rules_version;
} mygames_ddz_tournament_config;

typedef struct mygames_ddz_tournament_result {
    int32_t candidate_game_points; /* candidate TEAM sum of M5g delta */
    int32_t candidate_win_sign; /* +1 or -1 */
    uint32_t moves;
    uint32_t candidate_decisions;
    uint32_t baseline_decisions;
    uint32_t candidate_score_batches;
    uint8_t winner_seat;
    uint8_t landlord_seat;
} mygames_ddz_tournament_result;

/* Complete, actual native C11 match. Fail closed on any missing/nonfinite Q
 * or engine error and do not mutate out_result on failure. The same nonzero
 * shuffled_seed with independent invocation yields the SAME private deal;
 * the provider cannot inspect it. All three role transitions are native.
 * Results contain ONLY public terminal/aggregate metrics, no private data.
 * This API must NEVER be linked into public CFlow/CHttp/ORM services. */
mygames_ddz_tournament_status mygames_ddz_tournament_play(
    const mygames_ddz_tournament_config *config,
    mygames_ddz_tournament_score_fn score, void *score_context,
    mygames_ddz_tournament_result *out_result);

#ifdef __cplusplus
}
#endif
#endif
