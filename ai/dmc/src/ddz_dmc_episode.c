#include "mygames/ddz_dmc_episode.h"

#include <limits.h>
#include <string.h>

mygames_ddz_dmc_status mygames_ddz_dmc_episode_init(
    mygames_ddz_dmc_episode *out,
    mygames_ddz_dmc_sample *owned_samples,
    size_t capacity, uint64_t episode_id,
    uint64_t model_version, uint32_t rules_version,
    uint8_t landlord_seat) {
    mygames_ddz_dmc_episode episode = {0};
    if (!out || !owned_samples || capacity == 0u ||
        capacity > MYGAMES_DDZ_DMC_MAX_STEPS ||
        episode_id == 0u || model_version == 0u ||
        rules_version == 0u || landlord_seat >= MYGAMES_DDZ_SEATS)
        return MYGAMES_DDZ_DMC_INVALID;
    episode.samples = owned_samples;
    episode.capacity = capacity;
    episode.episode_id = episode_id;
    episode.model_version = model_version;
    episode.next_turn_id = UINT64_C(1);
    episode.rules_version = rules_version;
    episode.landlord_seat = landlord_seat;
    episode.terminal_winner = MYGAMES_DDZ_NO_SEAT;
    *out = episode;
    return MYGAMES_DDZ_DMC_OK;
}

mygames_ddz_dmc_status mygames_ddz_dmc_episode_append(
    mygames_ddz_dmc_episode *episode,
    const mygames_ddz_ai_observation *before,
    const mygames_ddz_action_result *accepted,
    uint64_t chosen_card_mask) {
    mygames_ddz_dmc_sample sample = {0};
    const mygames_ddz_public_view *view;
    if (!episode || !before || !accepted || !episode->samples)
        return MYGAMES_DDZ_DMC_INVALID;
    if (episode->sealed || episode->terminated) return MYGAMES_DDZ_DMC_SEALED;
    if (episode->count >= episode->capacity)
        return MYGAMES_DDZ_DMC_CAPACITY;
    view = &before->public_history.current;
    if (episode->count >= MYGAMES_DDZ_DMC_MAX_STEPS ||
        episode->next_turn_id == UINT64_MAX ||
        view->turn_id != episode->next_turn_id ||
        view->landlord_seat != episode->landlord_seat ||
        view->finished || view->turn_seat >= MYGAMES_DDZ_SEATS ||
        accepted->acting_seat != view->turn_seat ||
        chosen_card_mask != accepted->played_cards ||
        accepted->next_turn_id != episode->next_turn_id + UINT64_C(1) ||
        accepted->passed != (chosen_card_mask == 0u))
        return MYGAMES_DDZ_DMC_INVALID;
    if (accepted->finished &&
        (accepted->winner_seat != accepted->acting_seat ||
         accepted->next_seat != MYGAMES_DDZ_NO_SEAT))
        return MYGAMES_DDZ_DMC_INVALID;
    sample.episode_id = episode->episode_id;
    sample.source_turn_id = view->turn_id;
    sample.model_version = episode->model_version;
    sample.rules_version = episode->rules_version;
    sample.chosen_cards = chosen_card_mask;
    sample.actor_seat = accepted->acting_seat;
    sample.role = before->role;
    if (mygames_ddz_ai_encode_douzero(before, &sample.features) !=
            MYGAMES_DDZ_AI_OK ||
        mygames_ddz_ai_encode_action(before, chosen_card_mask,
                                     sample.action_features) != MYGAMES_DDZ_AI_OK)
        return MYGAMES_DDZ_DMC_INVALID;
    episode->samples[episode->count++] = sample;
    episode->next_turn_id = accepted->next_turn_id;
    if (accepted->finished) {
        episode->terminated = true;
        episode->terminal_winner = accepted->winner_seat;
    }
    return MYGAMES_DDZ_DMC_OK;
}

static bool dmc_settlement_valid(const mygames_ddz_settlement *s,
                                 uint8_t landlord, uint8_t winner) {
    uint64_t multiplier;
    int32_t expected;
    unsigned p;
    if (!s || s->landlord_seat != landlord || s->winner_seat != winner ||
        landlord >= MYGAMES_DDZ_SEATS || winner >= MYGAMES_DDZ_SEATS ||
        s->bid_base < 1u || s->bid_base > 3u ||
        s->bombs_rockets > 14u || s->spring > MYGAMES_DDZ_SPRING_FARMERS)
        return false;
    multiplier = UINT64_C(1) << ((unsigned)s->bombs_rockets +
          (s->spring == MYGAMES_DDZ_SPRING_NONE ? 0u : 1u));
    if (s->multiplier != multiplier ||
        s->unit != s->bid_base * multiplier ||
        s->unit == 0u || s->unit > (uint32_t)INT32_MAX / 2u)
        return false;
    if ((s->spring == MYGAMES_DDZ_SPRING_LANDLORD && winner != landlord) ||
        (s->spring == MYGAMES_DDZ_SPRING_FARMERS && winner == landlord))
        return false;
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p) {
        if (p == landlord)
            expected = winner == landlord ?
               2 * (int32_t)s->unit : -2 * (int32_t)s->unit;
        else
            expected = winner == landlord ?
               -(int32_t)s->unit : (int32_t)s->unit;
        if (s->delta[p] != expected) return false;
    }
    return true;
}

mygames_ddz_dmc_status mygames_ddz_dmc_episode_seal(
    mygames_ddz_dmc_episode *episode,
    const mygames_ddz_settlement *authoritative_settlement,
    mygames_ddz_dmc_objective objective) {
    size_t i;
    unsigned landlord;
    bool landlord_won;
    if (!episode || !episode->samples ||
        (objective != MYGAMES_DDZ_DMC_WIN_SIGN &&
         objective != MYGAMES_DDZ_DMC_GAME_POINTS))
        return MYGAMES_DDZ_DMC_INVALID;
    if (episode->sealed) return MYGAMES_DDZ_DMC_SEALED;
    if (!episode->terminated || episode->count == 0u)
        return MYGAMES_DDZ_DMC_NOT_TERMINAL;
    landlord = episode->landlord_seat;
    if (!dmc_settlement_valid(authoritative_settlement, episode->landlord_seat,
                              episode->terminal_winner))
        return MYGAMES_DDZ_DMC_INVALID;
    landlord_won = authoritative_settlement->winner_seat == landlord;
    for (i = 0u; i < episode->count; ++i) {
        const mygames_ddz_dmc_sample *sample = &episode->samples[i];
        if (sample->episode_id != episode->episode_id ||
            sample->rules_version != episode->rules_version ||
            sample->model_version != episode->model_version ||
            sample->actor_seat >= MYGAMES_DDZ_SEATS ||
            (sample->actor_seat == landlord ?
             sample->role != MYGAMES_DDZ_AI_LANDLORD :
             sample->role == MYGAMES_DDZ_AI_LANDLORD))
            return MYGAMES_DDZ_DMC_INVALID;
    }
    for (i = 0u; i < episode->count; ++i) {
        mygames_ddz_dmc_sample *s = &episode->samples[i];
        const bool same_team = s->actor_seat == landlord ?
                               landlord_won : !landlord_won;
        s->target = objective == MYGAMES_DDZ_DMC_WIN_SIGN ?
                     (same_team ? 1.0 : -1.0) :
                     (double)authoritative_settlement->delta[s->actor_seat];
    }
    episode->objective = objective;
    episode->sealed = true;
    return MYGAMES_DDZ_DMC_OK;
}
