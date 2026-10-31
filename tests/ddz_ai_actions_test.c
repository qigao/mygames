#include "mygames/ddz_ai_actions.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)

static unsigned popcount64(uint64_t x) {
    unsigned n = 0u;
    while (x) { x &= x - UINT64_C(1); ++n; }
    return n;
}

static unsigned rank_of(unsigned id) { return id < 52u ? id / 4u : id - 39u; }

static uint64_t canonical(uint64_t selection, uint64_t own) {
    uint64_t result = 0u;
    unsigned rank, id;
    for (rank = 0u; rank < 15u; ++rank) {
        unsigned need = 0u;
        for (id = 0u; id < 54u; ++id)
            if (rank_of(id) == rank && (selection & (UINT64_C(1) << id))) ++need;
        for (id = 0u; id < 54u && need != 0u; ++id)
            if (rank_of(id) == rank && (own & (UINT64_C(1) << id))) {
                result |= UINT64_C(1) << id;
                --need;
            }
        REQUIRE(need == 0u);
    }
    return result;
}

static bool contains(const uint64_t *v, size_t n, uint64_t m) {
    size_t i;
    for (i = 0u; i < n; ++i) if (v[i] == m) return true;
    return false;
}

static void compare_exhaustive_small(uint64_t hand, bool response) {
    mygames_ddz_public_view view = {0};
    uint64_t moves[1024];
    uint64_t oracle[1024];
    uint64_t subset;
    size_t needed = 0u, count = 0u, i;
    view.turn_seat = 0u;
    view.landlord_seat = 0u;
    view.last_seat = MYGAMES_DDZ_NO_SEAT;
    view.winner_seat = MYGAMES_DDZ_NO_SEAT;
    view.remaining[0] = (uint8_t)popcount64(hand);
    view.remaining[1] = 17u;
    view.remaining[2] = 17u;
    if (response) {
        view.last_seat = 1u;
        view.last_cards = UINT64_C(1);
        REQUIRE(mygames_ddz_classify(view.last_cards, &view.last_move) == MYGAMES_DDZ_OK);
    }
    REQUIRE(mygames_ddz_ai_legal_actions(hand, &view, NULL, 0u, &needed) == MYGAMES_DDZ_AI_OK);
    REQUIRE(needed < 1024u);
    REQUIRE(mygames_ddz_ai_legal_actions(hand, &view, moves, needed, &count) == MYGAMES_DDZ_AI_OK);
    REQUIRE(count == needed);
    count = 0u;
    if (response) oracle[count++] = UINT64_C(0);
    subset = hand;
    while (subset != 0u) {
        mygames_ddz_move candidate = {0};
        if (mygames_ddz_classify(subset, &candidate) == MYGAMES_DDZ_OK &&
            (!response || mygames_ddz_beats(&candidate, &view.last_move))) {
            uint64_t equiv = canonical(subset, hand);
            if (!contains(oracle, count, equiv)) {
                REQUIRE(count < 1024u);
                oracle[count++] = equiv;
            }
        }
        subset = (subset - UINT64_C(1)) & hand;
    }
    REQUIRE(count == needed);
    for (i = 0u; i < needed; ++i) {
        size_t j;
        REQUIRE(contains(oracle, count, moves[i]));
        for (j = i + 1u; j < needed; ++j) REQUIRE(moves[i] != moves[j]);
    }
    {
        uint64_t guard[2] = {UINT64_C(0x1234), UINT64_C(0x5678)};
        size_t reported = 999u;
        REQUIRE(needed > 1u);
        REQUIRE(mygames_ddz_ai_legal_actions(hand, &view, guard, 1u, &reported) == MYGAMES_DDZ_AI_CAPACITY);
        REQUIRE(reported == needed);
        REQUIRE(guard[0] == UINT64_C(0x1234) && guard[1] == UINT64_C(0x5678));
        view.finished = true;
        reported = 999u;
        REQUIRE(mygames_ddz_ai_legal_actions(hand, &view, guard, 2u, &reported) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
        REQUIRE(reported == 999u);
    }
}

static void test_real_round(void) {
    mygames_ddz_round round = {0};
    mygames_ddz_public_view view = {0};
    uint8_t deck[MYGAMES_DDZ_CARDS];
    uint64_t *actions;
    size_t needed = 0u, count = 0u, i;
    unsigned seat;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) deck[i] = (uint8_t)i;
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&round, &view) == MYGAMES_DDZ_OK);
    seat = view.turn_seat;
    REQUIRE(mygames_ddz_ai_legal_actions(round.hands[seat], &view, NULL, 0u, &needed) == MYGAMES_DDZ_AI_OK);
    REQUIRE(needed > 0u && needed < 1000000u);
    actions = (uint64_t *)malloc(needed * sizeof(*actions));
    REQUIRE(actions != NULL);
    REQUIRE(mygames_ddz_ai_legal_actions(round.hands[seat], &view, actions, needed, &count) == MYGAMES_DDZ_AI_OK);
    REQUIRE(count == needed);
    for (i = 0u; i < needed; ++i) {
        mygames_ddz_round trial = round;
        mygames_ddz_action_result result = {0};
        REQUIRE((actions[i] & ~round.hands[seat]) == 0u);
        REQUIRE(mygames_ddz_round_submit(&trial, seat, round.turn_id,
                                         actions[i], &result) == MYGAMES_DDZ_OK);
    }
    free(actions);
}

int main(void) {
    const uint64_t hand =
        (UINT64_C(1) << 4u) | (UINT64_C(1) << 5u) |
        (UINT64_C(1) << 8u) | (UINT64_C(1) << 12u) |
        (UINT64_C(1) << 16u) | (UINT64_C(1) << 52u) |
        (UINT64_C(1) << 53u);
    compare_exhaustive_small(hand, false);
    compare_exhaustive_small(hand, true);
    test_real_round();
    return 0;
}
