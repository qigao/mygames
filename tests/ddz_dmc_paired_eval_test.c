/* Test-only paired-deal evaluator for native MyGames C11 authoritative rules.
 * Same deterministic TEST shuffle across swapped policy/team assignments.
 * No source secrets or hands printed; no model/skill win claim. */
#include "mygames/ddz_dmc_selfplay.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MYGAMES_DDZ_MATCH_TESTING
#error "Paired evaluation requires the compile-time TEST-only entropy API"
#endif

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d FAIL: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

typedef struct test_entropy {
    uint32_t state;
} test_entropy;

typedef struct policy_context {
    uint8_t landlord;
    bool candidate_landlord;
    bool identity_null;
    uint32_t policy_seed;
    uint32_t callback_count;
} policy_context;

/* TEST-ONLY deterministic shuffle entropy, never a production CSPRNG
 * replacement. Each paired run starts from the identical independent seed. */
static bool seeded_fill(void *opaque, uint8_t *out, size_t bytes) {
    test_entropy *rng = (test_entropy *)opaque;
    uint32_t n;
    unsigned j;
    if (!rng || !out || bytes != 4u || rng->state == 0u) return false;
    n = rng->state;
    n ^= n << 13u;
    n ^= n >> 17u;
    n ^= n << 5u;
    if (n == 0u) n = UINT32_C(0x5a17c35d);
    rng->state = n;
    for (j = 0u; j < 4u; ++j) out[j] = (uint8_t)(n >> (8u * j));
    return true;
}

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16u;
    x *= UINT32_C(0x7feb352d);
    x ^= x >> 15u;
    x *= UINT32_C(0x846ca68b);
    x ^= x >> 16u;
    return x;
}

static unsigned popcount64(uint64_t cards) {
    unsigned n = 0u;
    while (cards != 0u) {
        cards &= cards - UINT64_C(1);
        ++n;
    }
    return n;
}

static bool candidate_vs_baseline(
    void *opaque, const mygames_ddz_ai_observation *observation,
    const uint64_t *legal, size_t count, uint64_t *out) {
    policy_context *ctx = (policy_context *)opaque;
    unsigned actor, i;
    bool use_candidate;
    if (!ctx || !observation || !legal || count == 0u || !out)
        return false;
    actor = observation->public_history.current.turn_seat;
    if (actor >= MYGAMES_DDZ_SEATS ||
        (observation->own_hand & ~MYGAMES_DDZ_CARD_MASK) != 0u)
        return false;
    use_candidate = ctx->identity_null ||
        ((actor == ctx->landlord) == ctx->candidate_landlord);
    if (use_candidate) {
        /* Explicit untrained "shed the most cards" heuristic. */
        size_t best = 0u;
        unsigned maximum = 0u;
        for (i = 0u; i < count; ++i) {
            const unsigned cards = popcount64(legal[i]);
            if (cards > maximum || (cards == maximum && legal[i] < legal[best])) {
                maximum = cards;
                best = i;
            }
        }
        *out = legal[best];
    } else {
        /* Uniform index over offered legal masks using a stable policy
         * hash of permitted observation/actor/turn; no opponent secrets.
         * This is a baseline heuristic, not genuine RL or crypto RNG. */
        const uint64_t hand = observation->own_hand;
        uint32_t key = (uint32_t)hand ^ (uint32_t)(hand >> 32u) ^
                       (uint32_t)observation->public_history.current.turn_id ^
                       (uint32_t)actor * UINT32_C(0x9e3779b9) ^
                       ctx->policy_seed;
        const size_t index = (size_t)mix32(key) % count;
        *out = legal[index];
    }
    ++ctx->callback_count;
    return true;
}

static void started_room(mygames_room *room) {
    unsigned seat = 99u;
    REQUIRE(mygames_room_init(room, "dmc-paired-only", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    REQUIRE(mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    REQUIRE(mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(room, 101u) == MYGAMES_ROOM_OK);
}

static void verify_identical_deck(const mygames_room *room, uint32_t seed) {
    mygames_ddz_match x = {0}, y = {0};
    test_entropy a = {seed}, b = {seed};
    REQUIRE(mygames_ddz_match_start_test_entropy(&x, room, 1u, 1u,
               seeded_fill, &a) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_match_start_test_entropy(&y, room, 1u, 2u,
               seeded_fill, &b) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(memcmp(x.secret_deck, y.secret_deck,
                   sizeof(x.secret_deck)) == 0);
    REQUIRE(memcmp(x.initial_hands, y.initial_hands,
                   sizeof(x.initial_hands)) == 0);
    REQUIRE(x.secret_kitty == y.secret_kitty);
    /* These private checks occur ONLY in the trusted test runner.
     * The policy callback above has no access to either match. */
}

static mygames_ddz_settlement play(
    const mygames_room *room, uint32_t shuffle_seed,
    uint32_t policy_seed, uint8_t landlord, bool candidate_landlord,
    bool identity_null, uint64_t unique_episode,
    mygames_ddz_dmc_sample *storage) {
    mygames_ddz_dmc_selfplay_config cfg = {0};
    mygames_ddz_dmc_episode episode = {0};
    mygames_ddz_settlement score = {0};
    test_entropy entropy = {shuffle_seed};
    policy_context policy = {0};
    cfg.room_epoch = 1u;
    cfg.game_id = unique_episode;
    cfg.episode_id = unique_episode;
    cfg.model_version = 1u; /* Baselines: NO trained checkpoint. */
    cfg.rules_version = 1u;
    cfg.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    cfg.max_legal_actions = 131072u;
    cfg.offline_landlord_seat = landlord;
    policy.landlord = landlord;
    policy.candidate_landlord = candidate_landlord;
    policy.identity_null = identity_null;
    policy.policy_seed = policy_seed;
    REQUIRE(mygames_ddz_dmc_selfplay_test_entropy(room, &cfg,
             seeded_fill, &entropy, candidate_vs_baseline, &policy,
             storage, MYGAMES_DDZ_DMC_MAX_STEPS, &episode, &score)
             == MYGAMES_DDZ_SELFPLAY_OK);
    REQUIRE(episode.sealed && episode.count == policy.callback_count &&
            episode.landlord_seat == landlord &&
            episode.terminal_winner == score.winner_seat &&
            score.delta[0] + score.delta[1] + score.delta[2] == 0);
    return score;
}

static int signed_result(const mygames_ddz_settlement *score,
                         uint8_t landlord, bool candidate_landlord) {
    const bool win = score->winner_seat == landlord;
    return (win == candidate_landlord) ? 1 : -1;
}

int main(void) {
    mygames_room room = {0};
    mygames_ddz_dmc_sample *samples;
    uint32_t seed_idx, null_idx;
    int total_signed = 0;
    int64_t total_points = INT64_C(0);
    unsigned games = 0u;
    started_room(&room);
    samples = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    REQUIRE(samples != NULL);
    /* Null check: swapped labels with IDENTICAL playing policy must cancel
     * exactly for the SAME physical shuffled deck & landlord seat. */
    for (null_idx = 0u; null_idx < 3u; ++null_idx) {
        const uint32_t seed = UINT32_C(0x51b53f2d) + null_idx * UINT32_C(777);
        mygames_ddz_settlement x, y;
        verify_identical_deck(&room, seed);
        x = play(&room, seed, 13u, (uint8_t)null_idx, true, true,
                 100u + null_idx * 2u, samples);
        y = play(&room, seed, 13u, (uint8_t)null_idx, false, true,
                 101u + null_idx * 2u, samples);
        REQUIRE(x.winner_seat == y.winner_seat);
        REQUIRE(x.unit == y.unit && x.multiplier == y.multiplier);
        REQUIRE(signed_result(&x, (uint8_t)null_idx, true) +
                signed_result(&y, (uint8_t)null_idx, false) == 0);
        REQUIRE(x.delta[null_idx] - y.delta[null_idx] == 0);
    }

    /* Two independent test-only decks, each covering all three landlord
     * seats with two SAME-deck policy/team swaps per seat (12 matches).
     * This is a structural paired-evaluation gate, NOT skill significance. */
    for (seed_idx = 0u; seed_idx < 2u; ++seed_idx) {
        const uint32_t seed = UINT32_C(0x1f3d6a59) +
                              seed_idx * UINT32_C(0x07f4a7c5);
        int sum_win = 0;
        int64_t sum_points = INT64_C(0);
        unsigned landlord;
        verify_identical_deck(&room, seed);
        for (landlord = 0u; landlord < 3u; ++landlord) {
            const uint64_t e = UINT64_C(1000) + seed_idx * 6u +
                               landlord * 2u;
            const mygames_ddz_settlement a = play(
                &room, seed, 0x27u, (uint8_t)landlord,
                true, false, e, samples);
            const mygames_ddz_settlement b = play(
                &room, seed, 0x27u, (uint8_t)landlord,
                false, false, e + UINT64_C(1), samples);
            sum_win += signed_result(&a, (uint8_t)landlord, true) +
                       signed_result(&b, (uint8_t)landlord, false);
            sum_points += (int64_t)a.delta[landlord] -
                          (int64_t)b.delta[landlord];
            games += 2u;
        }
        REQUIRE(sum_win >= -6 && sum_win <= 6 &&
                (sum_win % 2) == 0);
        total_signed += sum_win;
        total_points += sum_points;
        /* Aggregated, no private cards, deck seeds, turn-by-turn moves,
         * player identity or credentials in the evaluation output. */
        printf("MYGAMES_PAIRED_V1 seed_index=%" PRIu32
               " landlord_rotations=3 games=6 win_sign_sum=%d"
               " game_points_sum=%" PRId64 "\n",
               seed_idx, sum_win, sum_points);
    }
    REQUIRE(games == 12u && total_signed >= -12 && total_signed <= 12);
    printf("MYGAMES_PAIRED_TOTAL_V1 clusters=2 matches=%u"
           " win_sign_sum=%d game_points_sum=%" PRId64 "\n",
           games, total_signed, total_points);
    free(samples);
    return 0;
}
