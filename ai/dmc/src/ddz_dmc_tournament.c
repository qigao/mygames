#include "mygames/ddz_dmc_tournament.h"
#include <stdint.h>
#include <stdlib.h>

#ifndef MYGAMES_DDZ_MATCH_TESTING
#error "Tournament requires TEST-only M5b shuffle, never production"
#endif

typedef struct tournament_context {
    const mygames_ddz_tournament_config *cfg;
    mygames_ddz_tournament_score_fn score;
    void *score_context;
    uint32_t candidate_decisions;
    uint32_t baseline_decisions;
    uint32_t score_batches;
    bool scoring_failed;
} tournament_context;

typedef struct tournament_entropy { uint32_t state; } tournament_entropy;

static bool tournament_test_fill(void *opaque, uint8_t *out, size_t bytes) {
    tournament_entropy *rng = (tournament_entropy *)opaque;
    uint32_t n;
    unsigned j;
    if (!rng || !out || bytes != 4u || rng->state == 0u) return false;
    n = rng->state;
    n ^= n << 13u;
    n ^= n >> 17u;
    n ^= n << 5u;
    if (n == 0u) n = UINT32_C(0x5a17c35d);
    rng->state = n;
    for (j = 0u; j < 4u; ++j) out[j] = (uint8_t)(n >> (8u * j));
    return true;
}
static uint32_t tournament_mix(uint32_t n) {
    n ^= n >> 16u;
    n *= UINT32_C(0x7feb352d);
    n ^= n >> 15u;
    n *= UINT32_C(0x846ca68b);
    n ^= n >> 16u;
    return n;
}
static unsigned tournament_card_count(uint64_t mask) {
    unsigned count = 0u;
    while (mask) { mask &= mask - UINT64_C(1); ++count; }
    return count;
}

/* The original native C11 DMCQSelector remains the sole canonical
 * COMPLETE-legal-list and model/rules/turn-fenced score-and-argmax owner.
 * The callback receives only actor x/z and canonical action feature bytes. */
static bool tournament_score_batch(
    void *opaque, const mygames_ddz_ai_features *features,
    const int8_t *actions, size_t count, float *out) {
    tournament_context *ctx = (tournament_context *)opaque;
    if (!ctx || !features || !ctx->score || !out || !actions ||
        (unsigned)features->role > 2u || count == 0u ||
        count > MYGAMES_DDZ_DMC_Q_BATCH ||
        features->x_count !=
          (features->role == MYGAMES_DDZ_AI_LANDLORD ? 319u : 430u))
        return false;
    ++ctx->score_batches;
    if (!ctx->score(ctx->score_context, (uint32_t)features->role,
            features->x_no_action, features->x_count,
            features->z, MYGAMES_DDZ_AI_HISTORY_FEATURES,
            actions, count, out)) {
        ctx->scoring_failed = true;
        return false;
    }
    return true;
}

static bool tournament_policy(
    void *opaque, const mygames_ddz_ai_observation *obs,
    const uint64_t *legal, size_t count, uint64_t *selected) {
    tournament_context *ctx = (tournament_context *)opaque;
    unsigned seat;
    bool candidate;
    if (!ctx || !obs || !legal || count == 0u || !selected) return false;
    seat = obs->public_history.current.turn_seat;
    if (seat >= MYGAMES_DDZ_SEATS || (unsigned)obs->role > 2u) return false;
    candidate = ctx->cfg->identity_null != 0u ||
        ((seat == ctx->cfg->landlord_seat) ==
         (ctx->cfg->candidate_landlord != 0u));
    if (candidate) {
        mygames_ddz_dmc_q_provider provider = {0};
        provider.model_version = ctx->cfg->candidate_model_version;
        provider.rules_version = ctx->cfg->rules_version;
        provider.role = obs->role;
        provider.score_batch = tournament_score_batch;
        provider.context = ctx;
        if (mygames_ddz_dmc_q_pick(
                obs, legal, count, obs->public_history.current.turn_id,
                ctx->cfg->candidate_model_version, ctx->cfg->rules_version,
                &provider, selected) != MYGAMES_DDZ_DMC_Q_OK) {
            ctx->scoring_failed = true;
            return false;
        }
        ++ctx->candidate_decisions;
    } else {
        size_t best = 0u;
        if (ctx->cfg->baseline_type == 0u) {
            unsigned largest = 0u;
            size_t i;
            for (i = 0u; i < count; ++i) {
                const unsigned n = tournament_card_count(legal[i]);
                if (n > largest || (n == largest && legal[i] < legal[best])) {
                    largest = n;
                    best = i;
                }
            }
        } else {
            const uint64_t hand = obs->own_hand;
            const uint32_t key = (uint32_t)hand ^
                (uint32_t)(hand >> 32u) ^
                (uint32_t)obs->public_history.current.turn_id ^
                seat * UINT32_C(0x9e3779b9) ^ ctx->cfg->shuffled_seed;
            best = (size_t)tournament_mix(key) % count;
        }
        *selected = legal[best];
        ++ctx->baseline_decisions;
    }
    return true;
}

static bool tournament_ready_room(mygames_room *room) {
    unsigned seat = 99u;
    return mygames_room_init(room, "offline-tournament", 101u) ==
            MYGAMES_ROOM_OK &&
        mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK &&
        seat == 1u &&
        mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK &&
        seat == 2u &&
        mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK &&
        mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK &&
        mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK &&
        mygames_room_start(room, 101u) == MYGAMES_ROOM_OK;
}

mygames_ddz_tournament_status mygames_ddz_tournament_play(
    const mygames_ddz_tournament_config *cfg,
    mygames_ddz_tournament_score_fn score, void *score_context,
    mygames_ddz_tournament_result *out_result) {
    mygames_room room = {0};
    mygames_ddz_dmc_selfplay_config trial = {0};
    mygames_ddz_dmc_sample *samples;
    mygames_ddz_dmc_episode episode = {0};
    mygames_ddz_settlement settlement = {0};
    mygames_ddz_dmc_selfplay_status rc;
    tournament_entropy entropy = {0};
    tournament_context ctx = {0};
    mygames_ddz_tournament_result answer = {0};
    bool candidate_landlord;
    if (!cfg || !score || !out_result || cfg->shuffled_seed == 0u ||
        cfg->landlord_seat >= MYGAMES_DDZ_SEATS ||
        cfg->candidate_landlord > 1u || cfg->baseline_type > 1u ||
        cfg->identity_null > 1u || cfg->candidate_model_version == 0u ||
        cfg->rules_version != 1u || !tournament_ready_room(&room))
        return MYGAMES_DDZ_TOURNAMENT_INVALID;
    candidate_landlord = cfg->candidate_landlord != 0u;
    samples = (mygames_ddz_dmc_sample *)calloc(MYGAMES_DDZ_DMC_MAX_STEPS,
                                                sizeof(*samples));
    if (!samples) return MYGAMES_DDZ_TOURNAMENT_ENGINE_FAILED;
    entropy.state = cfg->shuffled_seed;
    ctx.cfg = cfg;
    ctx.score = score;
    ctx.score_context = score_context;
    trial.room_epoch = UINT64_C(1);
    trial.game_id = UINT64_C(1); /* seed/deck independent of game id */
    trial.episode_id = UINT64_C(1);
    trial.model_version = cfg->candidate_model_version;
    trial.rules_version = cfg->rules_version;
    trial.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    trial.max_legal_actions = MYGAMES_DDZ_DMC_Q_MAX_ACTIONS;
    trial.offline_landlord_seat = cfg->landlord_seat;
    rc = mygames_ddz_dmc_selfplay_test_entropy(
        &room, &trial, tournament_test_fill, &entropy,
        tournament_policy, &ctx, samples, MYGAMES_DDZ_DMC_MAX_STEPS,
        &episode, &settlement);
    if (rc == MYGAMES_DDZ_SELFPLAY_OK && episode.sealed &&
        episode.terminated && episode.count ==
            (size_t)ctx.candidate_decisions + ctx.baseline_decisions &&
        settlement.landlord_seat == cfg->landlord_seat) {
        const bool landlord_won =
            settlement.winner_seat == cfg->landlord_seat;
        answer.candidate_win_sign =
            (landlord_won == candidate_landlord) ? 1 : -1;
        answer.candidate_game_points = (candidate_landlord ? 1 : -1) *
            settlement.delta[cfg->landlord_seat];
        answer.moves = (uint32_t)episode.count;
        answer.candidate_decisions = ctx.candidate_decisions;
        answer.baseline_decisions = ctx.baseline_decisions;
        answer.candidate_score_batches = ctx.score_batches;
        answer.winner_seat = settlement.winner_seat;
        answer.landlord_seat = settlement.landlord_seat;
        *out_result = answer;
        free(samples);
        return MYGAMES_DDZ_TOURNAMENT_OK;
    }
    free(samples);
    return ctx.scoring_failed ?
        MYGAMES_DDZ_TOURNAMENT_POLICY_FAILED :
        MYGAMES_DDZ_TOURNAMENT_ENGINE_FAILED;
}
