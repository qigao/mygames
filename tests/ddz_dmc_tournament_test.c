#include "mygames/ddz_dmc_tournament.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #x); \
    exit(1); \
} } while (0)

typedef struct test_scorer {
    unsigned calls;
    bool reject;
    bool nonfinite;
    bool omit_last;
} test_scorer;

/* TEST-only card-count Q. Same choices as 'shed-most' baseline, so
 * swapping the same starting deal's team labels MUST cancel exactly.
 * This test does NOT claim trained-policy strength or import Torch. */
static bool card_count_q(
    void *opaque, uint32_t role, const int8_t *x, size_t x_count,
    const int8_t *z, size_t z_count, const int8_t *actions,
    size_t count, float *out) {
    test_scorer *scorer = (test_scorer *)opaque;
    size_t i, j;
    if (!scorer || !x || !z || !actions || !out ||
        count == 0u || count > 64u || role > 2u ||
        x_count != (role == 0u ? 319u : 430u) || z_count != 810u)
        return false;
    ++scorer->calls;
    if (scorer->reject) return false;
    for (i = 0u; i < count; ++i) {
        float sum = 0.0f;
        if (scorer->omit_last && i + 1u == count) continue;
        for (j = 0u; j < 54u; ++j) {
            const int8_t value = actions[i * 54u + j];
            if (value != 0 && value != 1) return false;
            sum += (float)value;
        }
        out[i] = sum;
    }
    if (scorer->nonfinite) out[count - 1u] = NAN;
    return true;
}

static mygames_ddz_tournament_result play(
    uint32_t seed, unsigned seat, unsigned candidate_landlord,
    unsigned baseline_type, unsigned identity_null, test_scorer *ctx) {
    mygames_ddz_tournament_config cfg = {0};
    mygames_ddz_tournament_result result = {0};
    cfg.shuffled_seed = seed;
    cfg.landlord_seat = (uint8_t)seat;
    cfg.candidate_landlord = (uint8_t)candidate_landlord;
    cfg.baseline_type = (uint8_t)baseline_type;
    cfg.identity_null = (uint8_t)identity_null;
    cfg.candidate_model_version = 2u;
    cfg.rules_version = 1u;
    REQUIRE(mygames_ddz_tournament_play(&cfg, card_count_q, ctx,
                &result) == MYGAMES_DDZ_TOURNAMENT_OK);
    REQUIRE(result.landlord_seat == seat &&
            result.winner_seat < MYGAMES_DDZ_SEATS &&
            (result.candidate_win_sign == 1 ||
             result.candidate_win_sign == -1) &&
            result.candidate_score_batches > 0u &&
            result.candidate_decisions > 0u && result.moves > 0u);
    return result;
}

int main(void) {
    test_scorer scorer = {0};
    unsigned seat;
    for (seat = 0u; seat < 3u; ++seat) {
        mygames_ddz_tournament_result a = play(
            UINT32_C(0x1278a9bc), seat, 1u, 0u, 0u, &scorer);
        mygames_ddz_tournament_result b = play(
            UINT32_C(0x1278a9bc), seat, 0u, 0u, 0u, &scorer);
        REQUIRE(a.winner_seat == b.winner_seat &&
                a.candidate_win_sign + b.candidate_win_sign == 0 &&
                a.candidate_game_points + b.candidate_game_points == 0);
        /* A vs A identity-null: model controls every seat, swapped
         * labels cannot affect terminal outcome on an identical deck. */
        a = play(UINT32_C(0x1278a9bc), seat, 1u, 1u, 1u, &scorer);
        b = play(UINT32_C(0x1278a9bc), seat, 0u, 1u, 1u, &scorer);
        REQUIRE(a.winner_seat == b.winner_seat &&
                a.candidate_win_sign + b.candidate_win_sign == 0 &&
                a.candidate_game_points + b.candidate_game_points == 0);
    }
    {
        mygames_ddz_tournament_config cfg = {0};
        mygames_ddz_tournament_result result = {0}, sentinel;
        cfg.shuffled_seed = UINT32_C(0x1278a9bc);
        cfg.candidate_model_version = 2u;
        cfg.rules_version = 1u;
        result.candidate_game_points = INT32_C(0x1234567);
        result.candidate_win_sign = 13;
        sentinel = result;
        scorer.reject = true;
        REQUIRE(mygames_ddz_tournament_play(&cfg, card_count_q, &scorer,
                    &result) == MYGAMES_DDZ_TOURNAMENT_POLICY_FAILED);
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
        scorer.reject = false;
        scorer.nonfinite = true;
        REQUIRE(mygames_ddz_tournament_play(&cfg, card_count_q, &scorer,
                    &result) == MYGAMES_DDZ_TOURNAMENT_POLICY_FAILED);
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
        scorer.nonfinite = false;
        scorer.omit_last = true;
        REQUIRE(mygames_ddz_tournament_play(&cfg, card_count_q, &scorer,
                    &result) == MYGAMES_DDZ_TOURNAMENT_POLICY_FAILED);
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
        scorer.omit_last = false;
        cfg.rules_version = 2u;
        REQUIRE(mygames_ddz_tournament_play(&cfg, card_count_q, &scorer,
                    &result) == MYGAMES_DDZ_TOURNAMENT_INVALID);
        REQUIRE(memcmp(&result, &sentinel, sizeof(result)) == 0);
    }
    REQUIRE(scorer.calls > 0u);
    puts("mygames.ddz_dmc_tournament: PASS");
    return 0;
}
