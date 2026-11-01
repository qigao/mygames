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

/* R9e: dispatch per PHYSICAL seat into the original authoritative C11
 * SelfPlay. This is neither an alternate game engine nor online Room owner. */
typedef struct mixed_context {
    onpolicy_context frozen_q;
    uint64_t episode_id;
    uint8_t seat_behavior[MYGAMES_DDZ_SEATS];
    uint32_t seat_decisions[MYGAMES_DDZ_SEATS];
    uint32_t total_decisions;
} mixed_context;

static unsigned mixed_card_count(uint64_t mask) {
    unsigned n = 0u;
    while (mask != UINT64_C(0)) {
        mask &= mask - UINT64_C(1);
        ++n;
    }
    return n;
}

static uint32_t mixed_hash32(uint32_t n) {
    n ^= n >> 16u;
    n *= UINT32_C(0x7feb352d);
    n ^= n >> 15u;
    n *= UINT32_C(0x846ca68b);
    n ^= n >> 16u;
    return n;
}

static bool mixed_choose(void *opaque,
    const mygames_ddz_ai_observation *actor,
    const uint64_t *legal, size_t legal_count, uint64_t *selected) {
    mixed_context *ctx = (mixed_context *)opaque;
    unsigned seat;
    bool ok = false;
    size_t i;
    if (!ctx || !actor || !legal || !selected || legal_count == 0u ||
        legal_count > MYGAMES_DDZ_DMC_Q_MAX_ACTIONS ||
        (unsigned)actor->role >= MYGAMES_DDZ_SEATS)
        return false;
    seat = actor->public_history.current.turn_seat;
    if (seat >= MYGAMES_DDZ_SEATS ||
        actor->public_history.current.remaining[seat] == 0u)
        return false;
    switch (ctx->seat_behavior[seat]) {
    case MYGAMES_DDZ_SEAT_FROZEN_Q:
        /* Canonical complete legal set and model/rules/turn fences are checked
         * by existing DMCQSelector, never Top-K or heuristic fallback. */
        ok = onpolicy_choose(&ctx->frozen_q, actor,
                             legal, legal_count, selected);
        break;
    case MYGAMES_DDZ_SEAT_SHED_MOST: {
        size_t best = 0u;
        unsigned count = 0u;
        for (i = 0u; i < legal_count; ++i) {
            unsigned cards = mixed_card_count(legal[i]);
            if (cards > count || (cards == count && legal[i] < legal[best])) {
                best = i;
                count = cards;
            }
        }
        *selected = legal[best];
        ok = true;
        break;
    }
    case MYGAMES_DDZ_SEAT_LEGAL_INDEX: {
        const uint64_t hand = actor->own_hand;
        const uint64_t turn = actor->public_history.current.turn_id;
        const uint32_t key = (uint32_t)hand ^ (uint32_t)(hand >> 32u) ^
            (uint32_t)turn ^ (uint32_t)ctx->episode_id ^
            (uint32_t)(ctx->episode_id >> 32u) ^
            ((uint32_t)seat * UINT32_C(0x9e3779b9));
        *selected = legal[(size_t)mixed_hash32(key) % legal_count];
        ok = true;
        break;
    }
    default:
        return false;
    }
    if (ok) {
        ++ctx->seat_decisions[seat];
        ++ctx->total_decisions;
    }
    return ok;
}

mygames_ddz_dmc_onpolicy_status mygames_ddz_dmc_mixed_collect(
    const mygames_ddz_dmc_mixed_config *config,
    mygames_ddz_dmc_onpolicy_score_fn score, void *score_context,
    void *replay_bytes, size_t replay_capacity,
    size_t *out_replay_size, uint32_t *out_accepted_moves,
    mygames_ddz_dmc_mixed_provenance *out_provenance) {
    mygames_room room = {0};
    mygames_ddz_dmc_selfplay_config trial = {0};
    mygames_ddz_dmc_sample *samples = NULL;
    mygames_ddz_dmc_episode episode = {0};
    mygames_ddz_settlement settlement = {0};
    mygames_ddz_dmc_selfplay_status state;
    mixed_context ctx = {0};
    mygames_ddz_dmc_mixed_provenance provenance = {0};
    size_t required_bytes = 0u, written_bytes = 0u;
    uint8_t *staged = NULL;
    unsigned seat, q_seats = 0u, strong_seats = 0u;
    mygames_ddz_dmc_onpolicy_status result = MYGAMES_DDZ_ONPOLICY_GAME_FAILED;
    if (!config || !score || !replay_bytes || !out_replay_size ||
        !out_accepted_moves || !out_provenance || !config->episode_id ||
        config->frozen_model_version <= UINT64_C(1) ||
        config->frozen_model_version == UINT64_MAX ||
        config->rules_version != 1u ||
        config->offline_landlord_seat >= MYGAMES_DDZ_SEATS ||
        (config->objective != MYGAMES_DDZ_DMC_WIN_SIGN &&
         config->objective != MYGAMES_DDZ_DMC_GAME_POINTS))
        return MYGAMES_DDZ_ONPOLICY_INVALID;
    for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat) {
        switch (config->seat_behavior[seat]) {
        case MYGAMES_DDZ_SEAT_FROZEN_Q: ++q_seats; break;
        case MYGAMES_DDZ_SEAT_SHED_MOST: ++strong_seats; break;
        case MYGAMES_DDZ_SEAT_LEGAL_INDEX: break;
        default: return MYGAMES_DDZ_ONPOLICY_INVALID;
        }
    }
    if (!q_seats || !strong_seats) return MYGAMES_DDZ_ONPOLICY_INVALID;
    if (replay_capacity < MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES +
                          MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES +
                          MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES)
        return MYGAMES_DDZ_ONPOLICY_CAPACITY;
    if (!onpolicy_ready_room(&room)) return MYGAMES_DDZ_ONPOLICY_GAME_FAILED;
    samples = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    if (!samples) return MYGAMES_DDZ_ONPOLICY_CAPACITY;
    ctx.frozen_q.score = score;
    ctx.frozen_q.score_context = score_context;
    ctx.frozen_q.model_version = config->frozen_model_version;
    ctx.frozen_q.rules_version = config->rules_version;
    ctx.frozen_q.last_q_status = MYGAMES_DDZ_DMC_Q_OK;
    ctx.episode_id = config->episode_id;
    memcpy(ctx.seat_behavior, config->seat_behavior, sizeof(ctx.seat_behavior));
    trial.room_epoch = UINT64_C(1);
    trial.game_id = config->episode_id;
    trial.episode_id = config->episode_id;
    trial.model_version = MYGAMES_DDZ_DMC_MIXED_REPLAY_SOURCE;
    trial.rules_version = config->rules_version;
    trial.objective = config->objective;
    trial.max_legal_actions = MYGAMES_DDZ_DMC_Q_MAX_ACTIONS;
    trial.offline_landlord_seat = config->offline_landlord_seat;
    state = mygames_ddz_dmc_selfplay_run(
        &room, &trial, mixed_choose, &ctx, samples,
        MYGAMES_DDZ_DMC_MAX_STEPS, &episode, &settlement);
    if (state == MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED ||
        ctx.frozen_q.scorer_failed || ctx.frozen_q.last_q_status !=
                MYGAMES_DDZ_DMC_Q_OK) {
        result = MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED;
        goto done;
    }
    if (state != MYGAMES_DDZ_SELFPLAY_OK || !episode.sealed ||
        !episode.terminated || episode.count != ctx.total_decisions ||
        episode.count == 0u || !ctx.frozen_q.decisions ||
        settlement.landlord_seat != config->offline_landlord_seat)
        goto done;
    if (mygames_ddz_dmc_replay_encode(&episode, NULL, 0u,
                                     &required_bytes) != MYGAMES_DDZ_REPLAY_OK) {
        result = MYGAMES_DDZ_ONPOLICY_REPLAY_FAILED;
        goto done;
    }
    if (replay_capacity < required_bytes) {
        result = MYGAMES_DDZ_ONPOLICY_CAPACITY;
        goto done;
    }
    staged = (uint8_t *)malloc(required_bytes);
    if (!staged) {
        result = MYGAMES_DDZ_ONPOLICY_CAPACITY;
        goto done;
    }
    if (mygames_ddz_dmc_replay_encode(&episode, staged, required_bytes,
                                     &written_bytes) != MYGAMES_DDZ_REPLAY_OK ||
        written_bytes != required_bytes) {
        result = MYGAMES_DDZ_ONPOLICY_REPLAY_FAILED;
        goto done;
    }
    provenance.episode_id = config->episode_id;
    provenance.frozen_model_version = config->frozen_model_version;
    provenance.replay_source_marker = MYGAMES_DDZ_DMC_MIXED_REPLAY_SOURCE;
    provenance.landlord_seat = settlement.landlord_seat;
    provenance.winner_seat = settlement.winner_seat;
    memcpy(provenance.seat_behavior, ctx.seat_behavior,
           sizeof(provenance.seat_behavior));
    memcpy(provenance.seat_decisions, ctx.seat_decisions,
           sizeof(provenance.seat_decisions));
    memcpy(replay_bytes, staged, written_bytes);
    *out_replay_size = written_bytes;
    *out_accepted_moves = ctx.total_decisions;
    *out_provenance = provenance;
    result = MYGAMES_DDZ_ONPOLICY_OK;
done:
    free(staged);
    free(samples);
    return result;
}
