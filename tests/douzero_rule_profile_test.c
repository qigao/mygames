#include "mygames/douzero_rule_profile.h"

#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static uint64_t rank_cards(unsigned rank, unsigned count) {
    uint64_t mask = 0u;
    unsigned i;
    REQUIRE(rank < 13u && count >= 1u && count <= 4u);
    for (i = 0u; i < count; ++i)
        mask |= UINT64_C(1) << (rank * 4u + i);
    return mask;
}

int main(void) {
    const uint64_t plane = rank_cards(4u,3u) | rank_cards(5u,3u) |
                           rank_cards(6u,3u) | rank_cards(12u,3u);
    const uint64_t double_quad = rank_cards(0u,4u) | rank_cards(1u,4u);
    const uint64_t ordinary = rank_cards(0u,3u) | rank_cards(1u,3u) |
                              rank_cards(8u,1u) | rank_cards(9u,1u);
    const uint64_t disconnected = rank_cards(0u,3u) | rank_cards(2u,3u) |
                                  rank_cards(4u,3u) | rank_cards(12u,3u);
    const uint64_t rocket = (UINT64_C(1) << 52u) | (UINT64_C(1) << 53u);
    mygames_ddz_move move = {0};
    mygames_ddz_move old = {MYGAMES_DDZ_MOVE_BOMB, 11u, 1u, 4u};

    /* Production/online classifier stays strict, unchanged and atomic. */
    REQUIRE(mygames_ddz_classify(plane, &old) == MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_ddz_classify(double_quad, &old) == MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(old.kind == MYGAMES_DDZ_MOVE_BOMB && old.high_rank == 11u);

    REQUIRE(mygames_douzero_classify_move(plane, &move) == MYGAMES_DDZ_OK);
    REQUIRE(move.kind == MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES &&
            move.high_rank == 6u && move.chain_length == 3u &&
            move.card_count == 12u);
    REQUIRE(mygames_douzero_classify_move(double_quad, &move) == MYGAMES_DDZ_OK);
    REQUIRE(move.kind == MYGAMES_DDZ_MOVE_FOUR_TWO_PAIRS &&
            move.high_rank == 1u && move.chain_length == 1u &&
            move.card_count == 8u);

    /* No unintended broadening of other original game variants. */
    REQUIRE(mygames_douzero_classify_move(disconnected, &old) ==
            MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_douzero_classify_move(0u, &old) == MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_douzero_classify_move(UINT64_C(1)<<54u, &old) ==
            MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(mygames_douzero_classify_move(ordinary, &move) == MYGAMES_DDZ_OK);
    REQUIRE(move.kind == MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES &&
            move.high_rank == 1u && move.chain_length == 2u);
    REQUIRE(mygames_douzero_classify_move(rocket, &move) == MYGAMES_DDZ_OK);
    REQUIRE(move.kind == MYGAMES_DDZ_MOVE_ROCKET && move.card_count == 2u);
    REQUIRE(mygames_douzero_classify_move(plane, NULL) == MYGAMES_DDZ_INVALID_MOVE);
    REQUIRE(old.kind == MYGAMES_DDZ_MOVE_BOMB && old.high_rank == 11u);
    REQUIRE(mygames_ddz_classify(plane, &old) == MYGAMES_DDZ_INVALID_MOVE);
    puts("mygames.douzero_offline_native_profile: PASS");
    return 0;
}
