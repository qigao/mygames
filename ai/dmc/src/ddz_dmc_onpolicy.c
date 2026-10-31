#include "mygames/ddz_dmc_onpolicy.h"

#include <stdlib.h>
#include <string.h>

typedef struct onpolicy_context {
    mygames_ddz_dmc_onpolicy_score_fn score;
    void *score_context;
    uint64_t model_version;
    uint32_t rules_version;
    mygames_ddz_dmc_q_status last_q_status;
    bool scorer_failed;
    uint32_t decisions;
} onpolicy_context;

static bool onpolicy_score_batch(
    void *opaque, const mygames_ddz_ai_features *features,
    const int8_t *actions, size_t action_count, float *out_scores) {
    onpolicy_context *context = (onpolicy_context *)opaque;
    if (!context || !context->score || !features || !actions ||
        !out_scores || (unsigned)features->role >= MYGAMES_DDZ_SEATS ||
        features->x_count != (features->role == MYGAMES_DDZ_AI_LANDLORD ?
                              319u : 430u) || action_count == 0u ||
        action_count > MYGAMES_DDZ_DMC_Q_BATCH)
        return false;
    if (!context->score(context->score_context, (uint32_t)features->role,
            features->x_no_action, features->x_count,
            features->z, MYGAMES_DDZ_AI_HISTORY_FEATURES,
            actions, action_count, out_scores)) {
        context->scorer_failed = true;
        return false;
    }
    return true;
}

static bool onpolicy_choose(
    void *opaque, const mygames_ddz_ai_observation *actor,
    const uint64_t *legal, size_t legal_count, uint64_t *selected) {
    onpolicy_context *context = (onpolicy_context *)opaque;
    mygames_ddz_dmc_q_provider provider = {0};
    if (!context || !actor || !legal || !selected ||
        (unsigned)actor->role >= MYGAMES_DDZ_SEATS)
        return false;
    provider.model_version = context->model_version;
    provider.rules_version = context->rules_version;
    provider.role = actor->role;
    provider.score_batch = onpolicy_score_batch;
    provider.context = context;
    context->last_q_status = mygames_ddz_dmc_q_pick(
        actor, legal, legal_count, actor->public_history.current.turn_id,
        context->model_version, context->rules_version,
        &provider, selected);
    if (context->last_q_status != MYGAMES_DDZ_DMC_Q_OK) return false;
    ++context->decisions;
    return true;
}

static bool onpolicy_ready_room(mygames_room *room) {
    unsigned seat = 99u;
    return mygames_room_init(room, "offline-frozen-q", 101u) ==
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

mygames_ddz_dmc_onpolicy_status mygames_ddz_dmc_onpolicy_collect(
    const mygames_ddz_dmc_onpolicy_config *config,
    mygames_ddz_dmc_onpolicy_score_fn score, void *score_context,
    void *replay_bytes, size_t replay_capacity,
    size_t *out_replay_size, uint32_t *out_accepted_moves) {
    mygames_room room = {0};
    mygames_ddz_dmc_selfplay_config trial = {0};
    mygames_ddz_dmc_sample *samples;
    mygames_ddz_dmc_episode episode = {0};
    mygames_ddz_settlement settlement = {0};
    mygames_ddz_dmc_selfplay_status status;
    onpolicy_context ctx = {0};
    size_t required_bytes = 0u, actual_bytes = 0u;
    uint8_t *staged_bytes = NULL;
    mygames_ddz_dmc_onpolicy_status result = MYGAMES_DDZ_ONPOLICY_GAME_FAILED;
    if (!config || !score || !replay_bytes || !out_replay_size ||
        !out_accepted_moves || config->episode_id == 0u ||
        config->frozen_model_version <= UINT64_C(1) ||
        config->rules_version != 1u ||
        config->offline_landlord_seat >= MYGAMES_DDZ_SEATS ||
        (config->objective != MYGAMES_DDZ_DMC_WIN_SIGN &&
         config->objective != MYGAMES_DDZ_DMC_GAME_POINTS))
        return MYGAMES_DDZ_ONPOLICY_INVALID;
    if (replay_capacity < MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES +
                          MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES +
                          MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES)
        return MYGAMES_DDZ_ONPOLICY_CAPACITY;
    if (!onpolicy_ready_room(&room)) return MYGAMES_DDZ_ONPOLICY_GAME_FAILED;
    samples = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    if (!samples) return MYGAMES_DDZ_ONPOLICY_CAPACITY;
    ctx.score = score;
    ctx.score_context = score_context;
    ctx.model_version = config->frozen_model_version;
    ctx.rules_version = config->rules_version;
    ctx.last_q_status = MYGAMES_DDZ_DMC_Q_OK;
    trial.room_epoch = 1u;
    trial.game_id = config->episode_id;
    trial.episode_id = config->episode_id;
    trial.model_version = config->frozen_model_version;
    trial.rules_version = config->rules_version;
    trial.objective = config->objective;
    trial.max_legal_actions = MYGAMES_DDZ_DMC_Q_MAX_ACTIONS;
    trial.offline_landlord_seat = config->offline_landlord_seat;
    status = mygames_ddz_dmc_selfplay_run(
        &room, &trial, onpolicy_choose, &ctx,
        samples, MYGAMES_DDZ_DMC_MAX_STEPS, &episode, &settlement);
    if (status == MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED ||
        ctx.scorer_failed || ctx.last_q_status != MYGAMES_DDZ_DMC_Q_OK) {
        result = MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED;
        goto done;
    }
    if (status != MYGAMES_DDZ_SELFPLAY_OK || !episode.sealed ||
        !episode.terminated || episode.count != (size_t)ctx.decisions ||
        episode.count == 0u || settlement.landlord_seat !=
            config->offline_landlord_seat)
        goto done;
    if (mygames_ddz_dmc_replay_encode(
            &episode, NULL, 0u, &required_bytes) != MYGAMES_DDZ_REPLAY_OK) {
        result = MYGAMES_DDZ_ONPOLICY_REPLAY_FAILED;
        goto done;
    }
    if (replay_capacity < required_bytes) {
        result = MYGAMES_DDZ_ONPOLICY_CAPACITY;
        goto done;
    }
    /* Transactional in-memory publication: even an unexpected encoder
     * failure never mutates caller-owned output Replay bytes. */
    staged_bytes = (uint8_t *)malloc(required_bytes);
    if (!staged_bytes) {
        result = MYGAMES_DDZ_ONPOLICY_CAPACITY;
        goto done;
    }
    if (mygames_ddz_dmc_replay_encode(
            &episode, staged_bytes, required_bytes,
            &actual_bytes) != MYGAMES_DDZ_REPLAY_OK ||
        actual_bytes != required_bytes) {
        result = MYGAMES_DDZ_ONPOLICY_REPLAY_FAILED;
        goto done;
    }
    memcpy(replay_bytes, staged_bytes, actual_bytes);
    *out_replay_size = actual_bytes;
    *out_accepted_moves = ctx.decisions;
    result = MYGAMES_DDZ_ONPOLICY_OK;
done:
    free(staged_bytes);
    free(samples);
    return result;
}
