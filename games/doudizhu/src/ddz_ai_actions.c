#include "mygames/ddz_ai_actions.h"

#include <stdbool.h>

#define DDZ_AI_RANKS 15u

typedef struct ddz_ai_walk {
    const mygames_ddz_public_view *view;
    uint64_t prefixes[DDZ_AI_RANKS][5];
    uint8_t available[DDZ_AI_RANKS];
    uint64_t *out;
    size_t count;
} ddz_ai_walk;

static unsigned ddz_ai_popcount(uint64_t value) {
    unsigned n = 0u;
    while (value != 0u) {
        value &= value - UINT64_C(1);
        ++n;
    }
    return n;
}

static bool ddz_ai_valid_view(uint64_t hand, const mygames_ddz_public_view *v) {
    mygames_ddz_move actual = {0};
    unsigned seat;
    if (!v || (hand & ~MYGAMES_DDZ_CARD_MASK) != 0u || hand == 0u ||
        v->finished || v->turn_seat >= MYGAMES_DDZ_SEATS ||
        v->landlord_seat >= MYGAMES_DDZ_SEATS ||
        v->last_seat > MYGAMES_DDZ_NO_SEAT ||
        v->winner_seat != MYGAMES_DDZ_NO_SEAT)
        return false;
    for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat) {
        if (v->remaining[seat] > 20u) return false;
    }
    if (ddz_ai_popcount(hand) != v->remaining[v->turn_seat]) return false;
    if (v->last_seat == MYGAMES_DDZ_NO_SEAT) {
        return v->last_cards == 0u && v->last_move.kind == MYGAMES_DDZ_MOVE_NONE;
    }
    if (v->last_seat == v->turn_seat || v->last_cards == 0u ||
        mygames_ddz_classify(v->last_cards, &actual) != MYGAMES_DDZ_OK)
        return false;
    return actual.kind == v->last_move.kind &&
           actual.high_rank == v->last_move.high_rank &&
           actual.chain_length == v->last_move.chain_length &&
           actual.card_count == v->last_move.card_count;
}

static void ddz_ai_emit(ddz_ai_walk *walk, uint64_t candidate) {
    mygames_ddz_move move = {0};
    if (candidate == 0u ||
        mygames_ddz_classify(candidate, &move) != MYGAMES_DDZ_OK)
        return;
    if (walk->view->last_seat != MYGAMES_DDZ_NO_SEAT &&
        !mygames_ddz_beats(&move, &walk->view->last_move))
        return;
    if (walk->out) walk->out[walk->count] = candidate;
    ++walk->count;
}

/* Rank-count vectors are bounded by 2^20 (the number of subsets of a
 * maximum 20-card hand). Unlike enumerating physical subsets, this visits
 * every rules-distinct rank multiset exactly once. The authoritative
 * classifier decides which combinations are v1-valid, including wings. */
static void ddz_ai_walk_ranks(ddz_ai_walk *walk, unsigned rank,
                              uint64_t candidate) {
    unsigned copies;
    if (rank == DDZ_AI_RANKS) {
        ddz_ai_emit(walk, candidate);
        return;
    }
    for (copies = 0u; copies <= walk->available[rank]; ++copies)
        ddz_ai_walk_ranks(walk, rank + 1u,
                          candidate | walk->prefixes[rank][copies]);
}

mygames_ddz_ai_status mygames_ddz_ai_legal_actions(
    uint64_t own_hand, const mygames_ddz_public_view *public_view,
    uint64_t *out_masks, size_t capacity, size_t *out_count) {
    ddz_ai_walk walk = {0};
    unsigned id, rank;
    size_t needed;
    if (!public_view || !out_count || (!out_masks && capacity != 0u))
        return MYGAMES_DDZ_AI_INVALID_ARGUMENT;
    if (!ddz_ai_valid_view(own_hand, public_view))
        return MYGAMES_DDZ_AI_INVALID_OBSERVATION;
    walk.view = public_view;
    for (id = 0u; id < MYGAMES_DDZ_CARDS; ++id) {
        if ((own_hand & (UINT64_C(1) << id)) == 0u) continue;
        rank = id < 52u ? id / 4u : id - 39u;
        walk.available[rank]++;
        walk.prefixes[rank][walk.available[rank]] =
            walk.prefixes[rank][walk.available[rank] - 1u] |
            (UINT64_C(1) << id);
    }
    /* A live non-leading trick permits PASS, which is not classifiable. */
    if (public_view->last_seat != MYGAMES_DDZ_NO_SEAT) ++walk.count;
    ddz_ai_walk_ranks(&walk, 0u, UINT64_C(0));
    needed = walk.count;
    if (out_masks && capacity < needed) {
        *out_count = needed;
        return MYGAMES_DDZ_AI_CAPACITY;
    }
    if (!out_masks) {
        *out_count = needed;
        return MYGAMES_DDZ_AI_OK;
    }
    walk.count = 0u;
    walk.out = out_masks;
    if (public_view->last_seat != MYGAMES_DDZ_NO_SEAT) {
        out_masks[0] = UINT64_C(0);
        walk.count = 1u;
    }
    ddz_ai_walk_ranks(&walk, 0u, UINT64_C(0));
    *out_count = walk.count;
    return MYGAMES_DDZ_AI_OK;
}
