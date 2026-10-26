#include "mygames/ddz_round.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unlike assert(), checks remain active in Release/NDEBUG builds. */
#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static uint64_t ranks(unsigned rank, unsigned copies) {
    uint64_t result = 0u;
    unsigned i;
    REQUIRE(rank < 13u && copies > 0u && copies <= 4u);
    for (i = 0u; i < copies; ++i)
        result |= UINT64_C(1) << (rank * 4u + i);
    return result;
}

static void kind(uint64_t cards, mygames_ddz_move_kind expected,
                 unsigned high, unsigned chain_length) {
    mygames_ddz_move move = {0};
    REQUIRE(mygames_ddz_classify(cards, &move) == MYGAMES_DDZ_OK);
    REQUIRE(move.kind == expected);
    REQUIRE(move.high_rank == high && move.chain_length == chain_length);
}

static void test_move_catalogue(void) {
    uint64_t straight = 0u, pairs = 0u, triples = 0u;
    mygames_ddz_move invalid_guard = {MYGAMES_DDZ_MOVE_BOMB, 12u, 1u, 4u};
    unsigned i;
    kind(ranks(0u, 1u), MYGAMES_DDZ_MOVE_SINGLE, 0u, 1u);
    kind(UINT64_C(1) << 53u, MYGAMES_DDZ_MOVE_SINGLE, 14u, 1u);
    kind(ranks(3u, 2u), MYGAMES_DDZ_MOVE_PAIR, 3u, 1u);
    kind(ranks(4u, 3u), MYGAMES_DDZ_MOVE_TRIPLE, 4u, 1u);
    kind(ranks(5u, 3u) | ranks(2u, 1u), MYGAMES_DDZ_MOVE_TRIPLE_SINGLE, 5u, 1u);
    kind(ranks(5u, 3u) | ranks(2u, 2u), MYGAMES_DDZ_MOVE_TRIPLE_PAIR, 5u, 1u);
    kind(ranks(8u, 4u), MYGAMES_DDZ_MOVE_BOMB, 8u, 1u);
    kind((UINT64_C(1) << 52u) | (UINT64_C(1) << 53u),
         MYGAMES_DDZ_MOVE_ROCKET, 14u, 1u);
    kind(ranks(8u, 4u) | ranks(6u, 1u) | ranks(7u, 1u),
         MYGAMES_DDZ_MOVE_FOUR_TWO_SINGLES, 8u, 1u);
    kind(ranks(8u, 4u) | ranks(6u, 2u) | ranks(7u, 2u),
         MYGAMES_DDZ_MOVE_FOUR_TWO_PAIRS, 8u, 1u);
    for (i = 0u; i < 5u; ++i) straight |= ranks(i, 1u);
    for (i = 3u; i < 6u; ++i) pairs |= ranks(i, 2u);
    for (i = 0u; i < 2u; ++i) triples |= ranks(i, 3u);
    kind(straight, MYGAMES_DDZ_MOVE_STRAIGHT, 4u, 5u);
    kind(pairs, MYGAMES_DDZ_MOVE_PAIR_CHAIN, 5u, 3u);
    kind(triples, MYGAMES_DDZ_MOVE_TRIPLE_CHAIN, 1u, 2u);
    kind(triples | ranks(6u, 1u) | ranks(7u, 1u),
         MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES, 1u, 2u);
    kind(triples | ranks(6u, 2u),
         MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES, 1u, 2u);
    kind(triples | ranks(6u, 2u) | ranks(7u, 2u),
         MYGAMES_DDZ_MOVE_AIRPLANE_PAIRS, 1u, 2u);
    REQUIRE(mygames_ddz_classify(straight | ranks(12u, 1u), &invalid_guard) ==
            MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_ddz_classify(UINT64_C(1) << 54u, &invalid_guard) ==
            MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_ddz_classify(0u, &invalid_guard) == MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(invalid_guard.kind == MYGAMES_DDZ_MOVE_BOMB &&
            invalid_guard.high_rank == 12u);
    REQUIRE(mygames_ddz_classify(ranks(0u, 1u) | ranks(2u, 1u) |
                                 ranks(4u, 1u) | ranks(6u, 1u) |
                                 ranks(8u, 1u), &invalid_guard) ==
            MYGAMES_DDZ_INVALID_MOVE);
}

static void test_masks_and_beating(void) {
    uint8_t cards[3] = {0u, 3u, 52u};
    uint64_t mask = UINT64_C(99);
    mygames_ddz_move low, high, bomb, rocket, straight5, straight6;
    REQUIRE(mygames_ddz_cards_to_mask(cards, 3u, &mask) == MYGAMES_DDZ_OK);
    REQUIRE(mask == ((UINT64_C(1) << 0u) | (UINT64_C(1) << 3u) |
                     (UINT64_C(1) << 52u)));
    cards[1] = 0u;
    REQUIRE(mygames_ddz_cards_to_mask(cards, 3u, &mask) == MYGAMES_DDZ_INVALID_ARGUMENT);
    REQUIRE(mask == ((UINT64_C(1) << 0u) | (UINT64_C(1) << 3u) |
                     (UINT64_C(1) << 52u)));
    cards[1] = 54u;
    REQUIRE(mygames_ddz_cards_to_mask(cards, 3u, &mask) == MYGAMES_DDZ_INVALID_ARGUMENT);
    REQUIRE(mygames_ddz_classify(ranks(1u, 1u), &low) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_classify(ranks(2u, 1u), &high) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_classify(ranks(1u, 4u), &bomb) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_classify((UINT64_C(1) << 52u) | (UINT64_C(1) << 53u),
                                 &rocket) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_beats(&high, &low));
    REQUIRE(!mygames_ddz_beats(&low, &high));
    REQUIRE(mygames_ddz_beats(&bomb, &high));
    REQUIRE(mygames_ddz_beats(&rocket, &bomb));
    REQUIRE(!mygames_ddz_beats(&bomb, &rocket));
    REQUIRE(!mygames_ddz_beats(&rocket, &rocket));
    REQUIRE(mygames_ddz_classify(ranks(0u,1u) | ranks(1u,1u) |
                                 ranks(2u,1u) | ranks(3u,1u) |
                                 ranks(4u,1u), &straight5) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_classify(ranks(0u,1u) | ranks(1u,1u) |
                                 ranks(2u,1u) | ranks(3u,1u) |
                                 ranks(4u,1u) | ranks(5u,1u),
                                 &straight6) == MYGAMES_DDZ_OK);
    REQUIRE(!mygames_ddz_beats(&straight6, &straight5));
}

static void ordered_deck(uint8_t deck[MYGAMES_DDZ_CARDS]) {
    unsigned i;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) deck[i] = (uint8_t)i;
}

static void test_round_and_rejections(void) {
    mygames_ddz_round round = {0}, prior;
    mygames_ddz_action_result result = {0}, original_result;
    uint8_t deck[MYGAMES_DDZ_CARDS];
    const uint64_t seat0 = UINT64_C(1) << 0u;
    const uint64_t seat1 = UINT64_C(1) << 4u;
    ordered_deck(deck);
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_validate(&round) == MYGAMES_DDZ_OK);
    REQUIRE(round.remaining[0] == 20u && round.remaining[1] == 17u &&
            round.remaining[2] == 17u && round.turn_seat == 0u && round.turn_id == 1u);
    prior = round;
    original_result = result;
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 1u, seat1, &result) ==
            MYGAMES_DDZ_NOT_YOUR_TURN);
    REQUIRE(memcmp(&round, &prior, sizeof(round)) == 0);
    REQUIRE(memcmp(&result, &original_result, sizeof(result)) == 0);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, 1u, 0u, &result) ==
            MYGAMES_DDZ_CANNOT_PASS);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, 1u, UINT64_C(1) << 1u, &result) ==
            MYGAMES_DDZ_CARDS_NOT_OWNED);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, 1u, seat0, &result) == MYGAMES_DDZ_OK);
    REQUIRE(result.next_turn_id == 2u && result.remaining[0] == 19u);
    prior = round;
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 1u, seat1, &result) ==
            MYGAMES_DDZ_STALE_TURN);
    REQUIRE(memcmp(&round, &prior, sizeof(round)) == 0);
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 2u, UINT64_C(1) << 1u, &result) ==
            MYGAMES_DDZ_NOT_HIGHER);
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 2u, seat1, &result) == MYGAMES_DDZ_OK);
    REQUIRE(round.last_move.high_rank == 1u && round.turn_seat == 2u);
    REQUIRE(mygames_ddz_round_submit(&round, 2u, 3u, 0u, &result) == MYGAMES_DDZ_OK);
    REQUIRE(result.passed && !result.trick_cleared);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, 4u, 0u, &result) == MYGAMES_DDZ_OK);
    REQUIRE(result.trick_cleared && round.turn_seat == 1u &&
            round.last_move.kind == MYGAMES_DDZ_MOVE_NONE);
    REQUIRE(mygames_ddz_round_validate(&round) == MYGAMES_DDZ_OK);
    prior = round;
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 5u, 0u, &result) ==
            MYGAMES_DDZ_CANNOT_PASS);
    REQUIRE(memcmp(&round, &prior, sizeof(round)) == 0);
    deck[0] = 53u; /* Original 53 remains in the last slot. */
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_INVALID_DECK);
    REQUIRE(memcmp(&round, &prior, sizeof(round)) == 0);
    REQUIRE(mygames_ddz_round_deal(&round, deck, 3u) == MYGAMES_DDZ_INVALID_ARGUMENT);
}

static void test_landlord_wins(void) {
    mygames_ddz_round round = {0}, prior;
    mygames_ddz_action_result result = {0};
    uint8_t deck[MYGAMES_DDZ_CARDS], landlord[20], outsiders[34];
    uint64_t winning = 0u;
    unsigned i, nlandlord = 0u, nothers = 0u;
    for (i = 0u; i < 5u; ++i) winning |= ranks(i, 3u);
    for (i = 5u; i < 10u; ++i) winning |= ranks(i, 1u);
    kind(winning, MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES, 4u, 5u);
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        if ((winning & (UINT64_C(1) << i)) != 0u)
            landlord[nlandlord++] = (uint8_t)i;
        else
            outsiders[nothers++] = (uint8_t)i;
    }
    REQUIRE(nlandlord == 20u && nothers == 34u);
    nlandlord = 0u;
    nothers = 0u;
    for (i = 0u; i < 51u; ++i) {
        if (i % MYGAMES_DDZ_SEATS == 0u) deck[i] = landlord[nlandlord++];
        else deck[i] = outsiders[nothers++];
    }
    for (i = 51u; i < MYGAMES_DDZ_CARDS; ++i) deck[i] = landlord[nlandlord++];
    REQUIRE(nlandlord == 20u && nothers == 34u);
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(round.hands[0] == winning);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, 1u, winning, &result) == MYGAMES_DDZ_OK);
    REQUIRE(round.finished && round.winner_seat == 0u &&
            round.turn_seat == MYGAMES_DDZ_NO_SEAT);
    REQUIRE(result.finished && result.winner_seat == 0u &&
            result.remaining[0] == 0u && result.next_turn_id == 2u);
    prior = round;
    REQUIRE(mygames_ddz_round_submit(&round, 1u, 2u,
                                     UINT64_C(1) << deck[1], &result) ==
            MYGAMES_DDZ_FINISHED);
    REQUIRE(memcmp(&round, &prior, sizeof(round)) == 0);
}

int main(void) {
    test_move_catalogue();
    test_masks_and_beating();
    test_round_and_rejections();
    test_landlord_wins();
    puts("mygames.ddz_round: PASS");
    return 0;
}
