#include "mygames/ddz_ai_features.h"

#include <stdbool.h>
#include <string.h>

static unsigned ddz_feature_count(uint64_t bits) {
    unsigned n = 0u;
    while (bits != 0u) {
        bits &= bits - UINT64_C(1);
        ++n;
    }
    return n;
}

static bool ddz_feature_masks_valid(uint64_t mask) {
    return (mask & ~MYGAMES_DDZ_CARD_MASK) == 0u;
}

static bool ddz_feature_move_equal(const mygames_ddz_move *a,
                                   const mygames_ddz_move *b) {
    return a->kind == b->kind && a->high_rank == b->high_rank &&
           a->chain_length == b->chain_length &&
           a->card_count == b->card_count;
}

static bool ddz_feature_history_valid(const mygames_ddz_ai_history *h) {
    uint64_t used = UINT64_C(0);
    unsigned p, i;
    if (!h || h->current.turn_id == 0u ||
        h->current.landlord_seat >= MYGAMES_DDZ_SEATS ||
        h->current.last_seat > MYGAMES_DDZ_NO_SEAT ||
        h->current.winner_seat > MYGAMES_DDZ_NO_SEAT ||
        h->recent_count > MYGAMES_DDZ_AI_HISTORY_MOVES ||
        h->bomb_count > 14u ||
        h->current.finished ||
        h->current.turn_seat >= MYGAMES_DDZ_SEATS) return false;
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p) {
        const unsigned initial = p == h->current.landlord_seat ? 20u : 17u;
        if (!ddz_feature_masks_valid(h->played_by_seat[p]) ||
            (used & h->played_by_seat[p]) != 0u ||
            h->current.remaining[p] == 0u ||
            h->current.remaining[p] > initial ||
            ddz_feature_count(h->played_by_seat[p]) !=
              initial - h->current.remaining[p] ||
            (h->last_action_by_seat[p] & ~h->played_by_seat[p]) != 0u)
            return false;
        used |= h->played_by_seat[p];
    }
    for (i = 0u; i < h->recent_count; ++i) {
        if (h->recent_seats[i] >= MYGAMES_DDZ_SEATS ||
            (h->recent_actions[i] &
             ~h->played_by_seat[h->recent_seats[i]]) != 0u) return false;
    }
    if (h->current.last_seat == MYGAMES_DDZ_NO_SEAT)
        return h->current.last_cards == 0u &&
            h->current.last_move.kind == MYGAMES_DDZ_MOVE_NONE;
    if (h->current.last_cards == 0u ||
        (h->current.last_cards &
          ~h->played_by_seat[h->current.last_seat]) != 0u)
        return false;
    {
        mygames_ddz_move kind = {0};
        return mygames_ddz_classify(h->current.last_cards, &kind) == MYGAMES_DDZ_OK &&
               ddz_feature_move_equal(&kind, &h->current.last_move);
    }
}

mygames_ddz_ai_status mygames_ddz_ai_history_init(
    const mygames_ddz_public_view *initial,
    mygames_ddz_ai_history *out) {
    mygames_ddz_ai_history h = {0};
    unsigned p;
    if (!initial || !out) return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    if (initial->turn_id != UINT64_C(1) ||
        initial->landlord_seat >= MYGAMES_DDZ_SEATS ||
        initial->turn_seat != initial->landlord_seat ||
        initial->last_seat != MYGAMES_DDZ_NO_SEAT ||
        initial->winner_seat != MYGAMES_DDZ_NO_SEAT ||
        initial->last_cards != 0u || initial->finished ||
        initial->last_move.kind != MYGAMES_DDZ_MOVE_NONE)
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p)
        if (initial->remaining[p] !=
            (p == initial->landlord_seat ? 20u : 17u))
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    h.current = *initial;
    if (!ddz_feature_history_valid(&h))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    *out = h;
    return MYGAMES_DDZ_AI_OK;
}

mygames_ddz_ai_status mygames_ddz_ai_history_append(
    mygames_ddz_ai_history *history,
    const mygames_ddz_action_result *accepted,
    const mygames_ddz_public_view *next_view) {
    mygames_ddz_ai_history next;
    unsigned p;
    unsigned played;
    mygames_ddz_move actual = {0};
    if (!history || !accepted || !next_view)
        return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    if (!ddz_feature_history_valid(history) ||
        history->current.turn_id == UINT64_MAX ||
        accepted->acting_seat != history->current.turn_seat ||
        accepted->next_turn_id != history->current.turn_id + UINT64_C(1) ||
        next_view->turn_id != accepted->next_turn_id ||
        next_view->landlord_seat != history->current.landlord_seat ||
        next_view->turn_seat != accepted->next_seat ||
        next_view->winner_seat != accepted->winner_seat ||
        next_view->finished != accepted->finished ||
        !ddz_feature_masks_valid(accepted->played_cards) ||
        (accepted->played_cards == 0u) != accepted->passed)
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    played = ddz_feature_count(accepted->played_cards);
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p) {
        if (next_view->remaining[p] != accepted->remaining[p] ||
            (p == accepted->acting_seat ?
              (played > history->current.remaining[p] ||
               next_view->remaining[p] != history->current.remaining[p] - played) :
              next_view->remaining[p] != history->current.remaining[p]))
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    }
    if (accepted->passed) {
        if (history->current.last_seat == MYGAMES_DDZ_NO_SEAT ||
            accepted->move.kind != MYGAMES_DDZ_MOVE_NONE ||
            (accepted->trick_cleared ?
              (next_view->last_seat != MYGAMES_DDZ_NO_SEAT ||
               next_view->last_cards != 0u) :
              (next_view->last_seat != history->current.last_seat ||
               next_view->last_cards != history->current.last_cards ||
               !ddz_feature_move_equal(&next_view->last_move,
                                       &history->current.last_move))))
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    } else {
        if ((accepted->played_cards &
             history->played_by_seat[0]) != 0u ||
            (accepted->played_cards &
             history->played_by_seat[1]) != 0u ||
            (accepted->played_cards &
             history->played_by_seat[2]) != 0u ||
            mygames_ddz_classify(accepted->played_cards, &actual) != MYGAMES_DDZ_OK ||
            !ddz_feature_move_equal(&actual, &accepted->move) ||
            (history->current.last_seat != MYGAMES_DDZ_NO_SEAT &&
             !mygames_ddz_beats(&actual, &history->current.last_move)) ||
            next_view->last_cards != accepted->played_cards ||
            next_view->last_seat != accepted->acting_seat ||
            !ddz_feature_move_equal(&next_view->last_move, &actual) ||
            accepted->trick_cleared)
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    }
    next = *history;
    next.current = *next_view;
    next.last_action_by_seat[accepted->acting_seat] = accepted->played_cards;
    next.played_by_seat[accepted->acting_seat] |= accepted->played_cards;
    if (!accepted->passed && (actual.kind == MYGAMES_DDZ_MOVE_BOMB ||
                              actual.kind == MYGAMES_DDZ_MOVE_ROCKET)) {
        if (next.bomb_count == 14u) return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
        ++next.bomb_count;
    }
    if (next.recent_count == MYGAMES_DDZ_AI_HISTORY_MOVES) {
        memmove(next.recent_actions, next.recent_actions + 1u,
                (MYGAMES_DDZ_AI_HISTORY_MOVES - 1u) *
                sizeof(next.recent_actions[0]));
        memmove(next.recent_seats, next.recent_seats + 1u,
                (MYGAMES_DDZ_AI_HISTORY_MOVES - 1u) *
                sizeof(next.recent_seats[0]));
        --next.recent_count;
    }
    next.recent_actions[next.recent_count] = accepted->played_cards;
    next.recent_seats[next.recent_count] = accepted->acting_seat;
    ++next.recent_count;
    if (!next_view->finished && !ddz_feature_history_valid(&next))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    /* Terminal states retain the authoritative public result for the
     * final transition, but are not eligible for another model decision. */
    if (next_view->finished) {
        if (next_view->winner_seat != accepted->acting_seat ||
            next_view->turn_seat != MYGAMES_DDZ_NO_SEAT ||
            next_view->remaining[accepted->acting_seat] != 0u)
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    }
    *history = next;
    return MYGAMES_DDZ_AI_OK;
}


static bool ddz_feature_public_view_equal(
    const mygames_ddz_public_view *a,
    const mygames_ddz_public_view *b) {
    unsigned p;
    if (a->turn_id != b->turn_id || a->last_cards != b->last_cards ||
        a->last_move.kind != b->last_move.kind ||
        a->last_move.high_rank != b->last_move.high_rank ||
        a->last_move.chain_length != b->last_move.chain_length ||
        a->last_move.card_count != b->last_move.card_count ||
        a->landlord_seat != b->landlord_seat ||
        a->turn_seat != b->turn_seat ||
        a->last_seat != b->last_seat ||
        a->winner_seat != b->winner_seat ||
        a->finished != b->finished)
        return false;
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p)
        if (a->remaining[p] != b->remaining[p]) return false;
    return true;
}

mygames_ddz_ai_status mygames_ddz_ai_history_reconstruct(
    uint8_t landlord_seat,
    const mygames_ddz_ai_public_move *accepted_moves,
    size_t move_count,
    const mygames_ddz_public_view *expected_authoritative_view,
    mygames_ddz_ai_history *out) {
    mygames_ddz_public_view initial = {0};
    mygames_ddz_ai_history history = {0};
    unsigned passes = 0u;
    size_t i;
    unsigned p;
    if (!out || !expected_authoritative_view ||
        landlord_seat >= MYGAMES_DDZ_SEATS ||
        move_count > MYGAMES_DDZ_AI_MAX_PUBLIC_MOVES ||
        (move_count != 0u && !accepted_moves))
        return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    initial.turn_id = UINT64_C(1);
    initial.landlord_seat = landlord_seat;
    initial.turn_seat = landlord_seat;
    initial.last_seat = MYGAMES_DDZ_NO_SEAT;
    initial.winner_seat = MYGAMES_DDZ_NO_SEAT;
    for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p)
        initial.remaining[p] = (uint8_t)(p == landlord_seat ? 20u : 17u);
    if (mygames_ddz_ai_history_init(&initial, &history) != MYGAMES_DDZ_AI_OK)
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    for (i = 0u; i < move_count; ++i) {
        const mygames_ddz_ai_public_move *row = &accepted_moves[i];
        mygames_ddz_public_view next = history.current;
        mygames_ddz_action_result result = {0};
        mygames_ddz_move classified = {0};
        unsigned count;
        uint64_t remaining;
        if (history.current.finished || history.current.turn_id == UINT64_MAX ||
            row->turn_id != history.current.turn_id ||
            row->actor_seat != history.current.turn_seat ||
            row->actor_seat >= MYGAMES_DDZ_SEATS ||
            (row->cards_mask & ~MYGAMES_DDZ_CARD_MASK) != 0u)
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
        result.acting_seat = row->actor_seat;
        result.played_cards = row->cards_mask;
        next.turn_id++;
        next.turn_seat = (uint8_t)((row->actor_seat + 1u) % MYGAMES_DDZ_SEATS);
        if (row->cards_mask == 0u) {
            if (next.last_seat == MYGAMES_DDZ_NO_SEAT)
                return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
            result.passed = true;
            ++passes;
            if (passes == MYGAMES_DDZ_SEATS - 1u) {
                next.last_seat = MYGAMES_DDZ_NO_SEAT;
                next.last_cards = UINT64_C(0);
                next.last_move = (mygames_ddz_move){0};
                result.trick_cleared = true;
                passes = 0u;
            }
        } else {
            if (mygames_ddz_classify(row->cards_mask, &classified) != MYGAMES_DDZ_OK ||
                (next.last_seat != MYGAMES_DDZ_NO_SEAT &&
                 !mygames_ddz_beats(&classified, &next.last_move)))
                return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
            remaining = row->cards_mask;
            count = 0u;
            while (remaining != 0u) {
                ++count;
                remaining &= remaining - UINT64_C(1);
            }
            if (count > next.remaining[row->actor_seat])
                return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
            next.remaining[row->actor_seat] =
                (uint8_t)(next.remaining[row->actor_seat] - count);
            next.last_seat = row->actor_seat;
            next.last_cards = row->cards_mask;
            next.last_move = classified;
            result.move = classified;
            passes = 0u;
            if (next.remaining[row->actor_seat] == 0u) {
                next.finished = true;
                next.winner_seat = row->actor_seat;
                next.turn_seat = MYGAMES_DDZ_NO_SEAT;
                result.finished = true;
            }
        }
        result.next_turn_id = next.turn_id;
        result.next_seat = next.turn_seat;
        result.winner_seat = next.winner_seat;
        for (p = 0u; p < MYGAMES_DDZ_SEATS; ++p)
            result.remaining[p] = next.remaining[p];
        if (mygames_ddz_ai_history_append(&history, &result, &next) !=
            MYGAMES_DDZ_AI_OK)
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    }
    if (!ddz_feature_public_view_equal(&history.current,
                                       expected_authoritative_view))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    *out = history;
    return MYGAMES_DDZ_AI_OK;
}

mygames_ddz_ai_status mygames_ddz_ai_observe(
    uint64_t authenticated_own_hand,
    const mygames_ddz_ai_history *public_history,
    mygames_ddz_ai_observation *out) {
    mygames_ddz_ai_observation obs = {0};
    unsigned seat, landlord;
    uint64_t played = UINT64_C(0);
    if (!public_history || !out) return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    if (!ddz_feature_history_valid(public_history))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    seat = public_history->current.turn_seat;
    landlord = public_history->current.landlord_seat;
    if (!ddz_feature_masks_valid(authenticated_own_hand) ||
        ddz_feature_count(authenticated_own_hand) !=
          public_history->current.remaining[seat])
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    for (unsigned p = 0u; p < MYGAMES_DDZ_SEATS; ++p)
        played |= public_history->played_by_seat[p];
    if ((authenticated_own_hand & played) != 0u)
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    obs.own_hand = authenticated_own_hand;
    obs.public_history = *public_history;
    obs.role = seat == landlord ? MYGAMES_DDZ_AI_LANDLORD :
               seat == (landlord + 1u) % MYGAMES_DDZ_SEATS ?
                 MYGAMES_DDZ_AI_LANDLORD_DOWN : MYGAMES_DDZ_AI_LANDLORD_UP;
    *out = obs;
    return MYGAMES_DDZ_AI_OK;
}

static bool ddz_feature_cards(uint64_t mask,
                              int8_t out[MYGAMES_DDZ_AI_CARD_FEATURES]) {
    unsigned count[15] = {0};
    unsigned id, rank, j;
    if (!ddz_feature_masks_valid(mask)) return false;
    memset(out, 0, MYGAMES_DDZ_AI_CARD_FEATURES * sizeof(out[0]));
    for (id = 0u; id < MYGAMES_DDZ_CARDS; ++id) {
        if ((mask & (UINT64_C(1) << id)) == 0u) continue;
        rank = id < 52u ? id / 4u : id - 39u;
        ++count[rank];
    }
    for (rank = 0u; rank < 13u; ++rank)
        for (j = 0u; j < count[rank]; ++j)
            out[rank * 4u + j] = 1;
    out[52] = (int8_t)count[13];
    out[53] = (int8_t)count[14];
    return true;
}

static bool ddz_feature_append_cards(int8_t *dst, size_t limit, size_t *pos,
                                     uint64_t mask) {
    int8_t one[MYGAMES_DDZ_AI_CARD_FEATURES];
    if (*pos > limit || MYGAMES_DDZ_AI_CARD_FEATURES > limit - *pos ||
        !ddz_feature_cards(mask, one))
        return false;
    memcpy(dst + *pos, one, sizeof(one));
    *pos += sizeof(one);
    return true;
}

static bool ddz_feature_append_remaining(int8_t *dst, size_t limit, size_t *pos,
                                         unsigned remaining, unsigned initial) {
    if (remaining == 0u || remaining > initial ||
        *pos > limit || initial > limit - *pos)
        return false;
    memset(dst + *pos, 0, initial);
    dst[*pos + remaining - 1u] = 1;
    *pos += initial;
    return true;
}

static bool ddz_feature_append_bombs(int8_t *dst, size_t limit, size_t *pos,
                                     unsigned bomb_count) {
    if (bomb_count >= 15u || *pos > limit || 15u > limit - *pos)
        return false;
    memset(dst + *pos, 0, 15u);
    dst[*pos + bomb_count] = 1;
    *pos += 15u;
    return true;
}

mygames_ddz_ai_status mygames_ddz_ai_encode_douzero(
    const mygames_ddz_ai_observation *observation,
    mygames_ddz_ai_features *out) {
    mygames_ddz_ai_features features = {0};
    const mygames_ddz_ai_history *h;
    uint64_t played = UINT64_C(0), other;
    unsigned landlord, up, down, teammate, p, i;
    size_t position = 0u;
    bool ok;
    if (!observation || !out) return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    h = &observation->public_history;
    if (!ddz_feature_history_valid(h) ||
        (observation->role != MYGAMES_DDZ_AI_LANDLORD &&
         observation->role != MYGAMES_DDZ_AI_LANDLORD_UP &&
         observation->role != MYGAMES_DDZ_AI_LANDLORD_DOWN))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    landlord = h->current.landlord_seat;
    up = (landlord + 2u) % MYGAMES_DDZ_SEATS;
    down = (landlord + 1u) % MYGAMES_DDZ_SEATS;
    p = h->current.turn_seat;
    if ((p == landlord && observation->role != MYGAMES_DDZ_AI_LANDLORD) ||
        (p == up && observation->role != MYGAMES_DDZ_AI_LANDLORD_UP) ||
        (p == down && observation->role != MYGAMES_DDZ_AI_LANDLORD_DOWN) ||
        !ddz_feature_masks_valid(observation->own_hand) ||
        ddz_feature_count(observation->own_hand) != h->current.remaining[p])
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        played |= h->played_by_seat[i];
    if ((observation->own_hand & played) != 0u)
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    other = MYGAMES_DDZ_CARD_MASK & ~(observation->own_hand | played);
    if (ddz_feature_count(other) !=
        (unsigned)h->current.remaining[(p + 1u) % 3u] +
        (unsigned)h->current.remaining[(p + 2u) % 3u])
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    features.role = observation->role;
    ok = ddz_feature_append_cards(features.x_no_action,
              MYGAMES_DDZ_AI_MAX_FEATURES, &position, observation->own_hand) &&
         ddz_feature_append_cards(features.x_no_action,
              MYGAMES_DDZ_AI_MAX_FEATURES, &position, other);
    if (observation->role == MYGAMES_DDZ_AI_LANDLORD) {
        ok = ok &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->current.last_cards) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->played_by_seat[up]) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->played_by_seat[down]) &&
            ddz_feature_append_remaining(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position,
                h->current.remaining[up], 17u) &&
            ddz_feature_append_remaining(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position,
                h->current.remaining[down], 17u);
        features.x_count = MYGAMES_DDZ_AI_LANDLORD_FEATURES;
    } else {
        teammate = p == up ? down : up;
        ok = ok &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->played_by_seat[landlord]) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->played_by_seat[teammate]) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->current.last_cards) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->last_action_by_seat[landlord]) &&
            ddz_feature_append_cards(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->last_action_by_seat[teammate]) &&
            ddz_feature_append_remaining(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position,
                h->current.remaining[landlord], 20u) &&
            ddz_feature_append_remaining(features.x_no_action,
                MYGAMES_DDZ_AI_MAX_FEATURES, &position,
                h->current.remaining[teammate], 17u);
        features.x_count = MYGAMES_DDZ_AI_FARMER_FEATURES;
    }
    ok = ok && ddz_feature_append_bombs(features.x_no_action,
        MYGAMES_DDZ_AI_MAX_FEATURES, &position, h->bomb_count) &&
        position == features.x_count;
    if (!ok) return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    /* DouZero last 15 public moves, zero-padded on the left; each rank
     * card is one of the 54 entries in 5 groups of 3 turns (5x162). */
    for (i = 0u; i < h->recent_count; ++i) {
        const size_t index = (MYGAMES_DDZ_AI_HISTORY_MOVES -
                              h->recent_count + i) *
                              MYGAMES_DDZ_AI_CARD_FEATURES;
        if (!ddz_feature_cards(h->recent_actions[i], features.z + index))
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    }
    *out = features;
    return MYGAMES_DDZ_AI_OK;
}

mygames_ddz_ai_status mygames_ddz_ai_encode_action(
    const mygames_ddz_ai_observation *observation,
    uint64_t proposed_action,
    int8_t out[MYGAMES_DDZ_AI_CARD_FEATURES]) {
    int8_t encoded[MYGAMES_DDZ_AI_CARD_FEATURES] = {0};
    mygames_ddz_move move = {0};
    const mygames_ddz_public_view *view;
    if (!observation || !out) return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    view = &observation->public_history.current;
    if (!ddz_feature_history_valid(&observation->public_history) ||
        !ddz_feature_masks_valid(observation->own_hand) ||
        !ddz_feature_masks_valid(proposed_action) ||
        (proposed_action & ~observation->own_hand) != 0u ||
        ddz_feature_count(observation->own_hand) !=
          view->remaining[view->turn_seat])
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    if (proposed_action == 0u) {
        if (view->last_seat == MYGAMES_DDZ_NO_SEAT)
            return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    } else if (mygames_ddz_classify(proposed_action, &move) != MYGAMES_DDZ_OK ||
        (view->last_seat != MYGAMES_DDZ_NO_SEAT &&
         !mygames_ddz_beats(&move, &view->last_move)))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    if (!ddz_feature_cards(proposed_action, encoded))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    memcpy(out, encoded, sizeof(encoded));
    return MYGAMES_DDZ_AI_OK;
}
