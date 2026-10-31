#include "mygames/ddz_dmc_selfplay.h"

#include <stdlib.h>

mygames_ddz_dmc_selfplay_status mygames_ddz_dmc_selfplay_run(
    const mygames_room *started_room,
    const mygames_ddz_dmc_selfplay_config *cfg,
    mygames_ddz_dmc_policy_fn policy, void *policy_context,
    mygames_ddz_dmc_sample *owned_samples, size_t sample_capacity,
    mygames_ddz_dmc_episode *out_episode,
    mygames_ddz_settlement *out_settlement) {
    mygames_ddz_match match = {0}, next = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_dmc_episode episode = {0};
    mygames_ddz_settlement settlement = {0};
    mygames_ddz_ai_history history = {0};
    mygames_ddz_public_view view = {0};
    mygames_ddz_dmc_selfplay_status result = MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
    uint64_t *legal = NULL;
    unsigned landlord;
    if (!started_room || !cfg || !policy || !owned_samples ||
        !out_episode || !out_settlement ||
        !cfg->room_epoch || !cfg->game_id || !cfg->episode_id ||
        !cfg->model_version || !cfg->rules_version ||
        cfg->max_legal_actions == 0u ||
        cfg->max_legal_actions > (size_t)(UINT32_C(1) << 20u) ||
        cfg->offline_landlord_seat >= MYGAMES_DDZ_SEATS ||
        sample_capacity == 0u ||
        sample_capacity > MYGAMES_DDZ_DMC_MAX_STEPS ||
        (cfg->objective != MYGAMES_DDZ_DMC_WIN_SIGN &&
         cfg->objective != MYGAMES_DDZ_DMC_GAME_POINTS))
        return MYGAMES_DDZ_SELFPLAY_INVALID;
    if (mygames_ddz_match_start(
            &match, started_room, cfg->room_epoch,
            cfg->game_id) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
    landlord = cfg->offline_landlord_seat;
    /* Rotate game roles without inventing a second bidding rule: each
     * preceding bidder makes a valid PASS(0), then the specified seat
     * legally bids 3. This is trusted OFFLINE only; production owner and
     * network command schemas do NOT accept an AI-selected landlord. */
    {
        unsigned bid_count = 0u;
        while (match.bid_turn_seat != landlord &&
               bid_count < MYGAMES_DDZ_SEATS - 1u) {
            const unsigned seat = match.bid_turn_seat;
            if (seat >= MYGAMES_DDZ_SEATS ||
                mygames_ddz_match_bid(&match, cfg->room_epoch, cfg->game_id,
                    match.players[seat], match.bid_turn_id, 0u,
                    &next, &bid) != MYGAMES_DDZ_MATCH_OK ||
                next.phase != MYGAMES_DDZ_MATCH_BIDDING)
                return MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
            match = next;
            ++bid_count;
        }
        if (match.bid_turn_seat != landlord ||
            mygames_ddz_match_bid(&match, cfg->room_epoch, cfg->game_id,
                match.players[landlord], match.bid_turn_id, 3u,
                &next, &bid) != MYGAMES_DDZ_MATCH_OK ||
            next.phase != MYGAMES_DDZ_MATCH_PLAYING ||
            next.round.landlord_seat != landlord)
            return MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
        match = next;
    }
    if (mygames_ddz_round_public_view(&match.round, &view) != MYGAMES_DDZ_OK ||
        mygames_ddz_ai_history_init(&view, &history) != MYGAMES_DDZ_AI_OK ||
        mygames_ddz_dmc_episode_init(&episode, owned_samples,
            sample_capacity, cfg->episode_id, cfg->model_version,
            cfg->rules_version, (uint8_t)landlord) != MYGAMES_DDZ_DMC_OK)
        return MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;

    legal = (uint64_t *)malloc(cfg->max_legal_actions * sizeof(*legal));
    if (!legal) return MYGAMES_DDZ_SELFPLAY_CAPACITY;
    while (match.phase == MYGAMES_DDZ_MATCH_PLAYING) {
        mygames_ddz_ai_observation observation = {0};
        mygames_ddz_action_result accepted = {0};
        mygames_ddz_public_view after = {0};
        uint64_t own_hand = UINT64_C(0);
        uint64_t selected = UINT64_C(0);
        size_t required = 0u, count = 0u, i;
        bool found = false;
        const unsigned seat = match.round.turn_seat;
        if (episode.count >= sample_capacity) {
            result = MYGAMES_DDZ_SELFPLAY_TRUNCATED;
            goto done;
        }
        if (seat >= MYGAMES_DDZ_SEATS ||
            mygames_ddz_match_private_hand(&match, cfg->room_epoch,
                 cfg->game_id, match.players[seat], &own_hand) !=
                    MYGAMES_DDZ_MATCH_OK ||
            mygames_ddz_round_public_view(&match.round, &view) !=
                    MYGAMES_DDZ_OK ||
            mygames_ddz_ai_observe(own_hand, &history, &observation) !=
                    MYGAMES_DDZ_AI_OK ||
            mygames_ddz_ai_legal_actions(own_hand, &view,
                  NULL, 0u, &required) != MYGAMES_DDZ_AI_OK ||
            required == 0u) {
            result = MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
            goto done;
        }
        if (required > cfg->max_legal_actions) {
            result = MYGAMES_DDZ_SELFPLAY_CAPACITY;
            goto done;
        }
        if (mygames_ddz_ai_legal_actions(own_hand, &view,
                legal, cfg->max_legal_actions, &count) != MYGAMES_DDZ_AI_OK ||
            count != required) {
            result = MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
            goto done;
        }
        if (!policy(policy_context, &observation, legal, count, &selected)) {
            result = MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED;
            goto done;
        }
        for (i = 0u; i < count; ++i)
            if (legal[i] == selected) { found = true; break; }
        if (!found) {
            result = MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED;
            goto done;
        }
        if (mygames_ddz_match_play(&match, cfg->room_epoch, cfg->game_id,
                match.players[seat], match.round.turn_id, selected,
                &next, &accepted) != MYGAMES_DDZ_MATCH_OK ||
            mygames_ddz_round_public_view(&next.round, &after) !=
                MYGAMES_DDZ_OK ||
            mygames_ddz_dmc_episode_append(&episode, &observation,
                &accepted, selected) != MYGAMES_DDZ_DMC_OK ||
            mygames_ddz_ai_history_append(&history, &accepted, &after) !=
                MYGAMES_DDZ_AI_OK) {
            result = MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
            goto done;
        }
        match = next;
    }
    if (match.phase != MYGAMES_DDZ_MATCH_FINISHED ||
        mygames_ddz_match_settlement(&match, &settlement) != MYGAMES_DDZ_MATCH_OK ||
        mygames_ddz_dmc_episode_seal(&episode,
            &settlement, cfg->objective) != MYGAMES_DDZ_DMC_OK) {
        result = MYGAMES_DDZ_SELFPLAY_ENGINE_FAILED;
        goto done;
    }
    *out_episode = episode;
    *out_settlement = settlement;
    result = MYGAMES_DDZ_SELFPLAY_OK;
done:
    free(legal);
    return result;
}
