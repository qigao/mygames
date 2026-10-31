#include "mygames/ddz_dmc_selfplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d failed %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

typedef struct test_policy_state {
    uint32_t rng;
    size_t calls;
    unsigned role_calls[3];
} test_policy_state;

static void started_room(mygames_room *room) {
    unsigned seat = 99u;
    REQUIRE(mygames_room_init(room, "offline-dmc", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    REQUIRE(mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    REQUIRE(mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(room, 101u) == MYGAMES_ROOM_OK);
}

static bool sample_policy(void *user, const mygames_ddz_ai_observation *obs,
                          const uint64_t *legal, size_t count,
                          uint64_t *out) {
    test_policy_state *s = (test_policy_state *)user;
    unsigned seat;
    REQUIRE(s && obs && legal && count > 0u && out);
    seat = obs->public_history.current.turn_seat;
    REQUIRE(seat < 3u && obs->role <= MYGAMES_DDZ_AI_LANDLORD_DOWN);
    REQUIRE(obs->own_hand != 0u);
    REQUIRE(obs->public_history.current.remaining[seat] > 0u);
    /* The seed affects only TEST POLICY selection. The actual match/deck
     * is always allocated using real OS CSPRNG by ddz_match_start(). */
    s->rng ^= s->rng << 13u;
    s->rng ^= s->rng >> 17u;
    s->rng ^= s->rng << 5u;
    *out = legal[s->rng % count];
    REQUIRE((*out & ~obs->own_hand) == 0u);
    ++s->calls;
    ++s->role_calls[obs->role];
    return true;
}

static bool refuse_policy(void *user, const mygames_ddz_ai_observation *obs,
                          const uint64_t *legal, size_t count,
                          uint64_t *out) {
    (void)user; (void)obs; (void)legal; (void)count;
    *out = UINT64_MAX; /* Never valid; no fallback to first legal card. */
    return true;
}

static bool first_single_policy(void *user, const mygames_ddz_ai_observation *obs,
                                const uint64_t *legal, size_t count,
                                uint64_t *out) {
    size_t i;
    (void)user; (void)obs;
    for (i = 0u; i < count; ++i) {
        const uint64_t mask = legal[i];
        if (mask && (mask & (mask - UINT64_C(1))) == 0u) {
            *out = mask;
            return true;
        }
    }
    return false;
}

static void assert_episode(
    const mygames_ddz_dmc_episode *ep, const mygames_ddz_settlement *score,
    mygames_ddz_dmc_objective objective, const char *fixture_path) {
    uint8_t *buffer;
    mygames_ddz_dmc_sample *restored;
    mygames_ddz_dmc_episode decoded = {0};
    size_t bytes = 0u, needed = 0u, i;
    unsigned role_count[3] = {0u};
    REQUIRE(ep->sealed && ep->terminated && ep->count > 0u &&
            ep->count <= MYGAMES_DDZ_DMC_MAX_STEPS &&
            ep->terminal_winner == score->winner_seat &&
            ep->landlord_seat == score->landlord_seat &&
            score->delta[0] + score->delta[1] + score->delta[2] == 0);
    buffer = (uint8_t *)malloc(MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
    restored = (mygames_ddz_dmc_sample *)calloc(MYGAMES_DDZ_DMC_MAX_STEPS,
                                                 sizeof(*restored));
    REQUIRE(buffer && restored);
    REQUIRE(mygames_ddz_dmc_replay_encode(ep, NULL, 0u, &bytes) ==
            MYGAMES_DDZ_REPLAY_OK);
    REQUIRE(mygames_ddz_dmc_replay_encode(ep, buffer,
             MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES, &needed) == MYGAMES_DDZ_REPLAY_OK);
    REQUIRE(bytes == needed);
    REQUIRE(mygames_ddz_dmc_replay_decode(buffer, bytes, &decoded,
             restored, MYGAMES_DDZ_DMC_MAX_STEPS, &needed) == MYGAMES_DDZ_REPLAY_OK);
    REQUIRE(needed == ep->count && decoded.objective == objective &&
            decoded.episode_id == ep->episode_id);
    for (i = 0u; i < ep->count; ++i) {
        const mygames_ddz_dmc_sample *sample = &ep->samples[i];
        double expected;
        ++role_count[sample->role];
        REQUIRE(sample->source_turn_id == i + UINT64_C(1) &&
                sample->model_version == ep->model_version &&
                sample->actor_seat == (ep->landlord_seat + i) % 3u);
        expected = objective == MYGAMES_DDZ_DMC_GAME_POINTS ?
                   (double)score->delta[sample->actor_seat] :
                   ((sample->actor_seat == score->landlord_seat) ==
                     (score->winner_seat == score->landlord_seat) ? 1.0 : -1.0);
        REQUIRE(sample->target == expected &&
                restored[i].target == expected &&
                restored[i].chosen_cards == sample->chosen_cards);
    }
    REQUIRE(role_count[0] + role_count[1] + role_count[2] == ep->count);
    REQUIRE(role_count[0] > 0u);
    if (fixture_path) {
        FILE *file = fopen(fixture_path, "wb");
        REQUIRE(file != NULL);
        REQUIRE(fwrite(buffer, 1u, bytes, file) == bytes);
        REQUIRE(fclose(file) == 0);
    }
    free(buffer);
    free(restored);
}

static void run_real_selfplay(const mygames_room *room, const char *path) {
    unsigned trial;
    mygames_ddz_dmc_sample *storage = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*storage));
    REQUIRE(storage);
    for (trial = 0u; trial < 3u; ++trial) {
        mygames_ddz_dmc_selfplay_config config = {0};
        mygames_ddz_dmc_episode episode = {0};
        mygames_ddz_settlement settlement = {0};
        test_policy_state policy = {.rng = UINT32_C(0x1234abcd) + trial};
        config.room_epoch = 42u;
        config.game_id = 1000u + trial;
        config.episode_id = 5000u + trial;
        config.model_version = 12u;
        config.rules_version = 1u;
        config.objective = trial == 0u ?
            MYGAMES_DDZ_DMC_WIN_SIGN : MYGAMES_DDZ_DMC_GAME_POINTS;
        config.max_legal_actions = 65536u;
        REQUIRE(mygames_ddz_dmc_selfplay_run(room, &config, sample_policy,
                 &policy, storage, MYGAMES_DDZ_DMC_MAX_STEPS,
                 &episode, &settlement) == MYGAMES_DDZ_SELFPLAY_OK);
        REQUIRE(episode.count == policy.calls);
        REQUIRE(policy.role_calls[0] + policy.role_calls[1] +
                policy.role_calls[2] == episode.count);
        REQUIRE(policy.role_calls[0] > 0u);
        assert_episode(&episode, &settlement, config.objective,
                       trial == 0u ? path : NULL);
    }
    free(storage);
}

static void test_reject_bad_policy_and_truncation(const mygames_room *room) {
    mygames_ddz_dmc_selfplay_config config = {0};
    mygames_ddz_dmc_episode output = {0};
    mygames_ddz_dmc_episode sentinel;
    mygames_ddz_settlement score = {0}, original;
    mygames_ddz_dmc_sample sample[1] = {{0}};
    config.room_epoch = 1u;
    config.game_id = 900u;
    config.episode_id = 777u;
    config.model_version = 1u;
    config.rules_version = 1u;
    config.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    config.max_legal_actions = 1u;
    memset(&output, 0x7b, sizeof(output));
    memset(&score, 0x6c, sizeof(score));
    sentinel = output;
    original = score;
    REQUIRE(mygames_ddz_dmc_selfplay_run(room, &config, refuse_policy, NULL,
             sample, 1u, &output, &score) == MYGAMES_DDZ_SELFPLAY_CAPACITY);
    REQUIRE(memcmp(&output, &sentinel, sizeof(output)) == 0 &&
            memcmp(&score, &original, sizeof(score)) == 0);
    config.max_legal_actions = 65536u;
    REQUIRE(mygames_ddz_dmc_selfplay_run(room, &config, refuse_policy, NULL,
             sample, 1u, &output, &score) ==
             MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED);
    REQUIRE(memcmp(&output, &sentinel, sizeof(output)) == 0);
    REQUIRE(mygames_ddz_dmc_selfplay_run(room, &config, first_single_policy, NULL,
             sample, 1u, &output, &score) == MYGAMES_DDZ_SELFPLAY_TRUNCATED);
    REQUIRE(memcmp(&output, &sentinel, sizeof(output)) == 0 &&
            memcmp(&score, &original, sizeof(score)) == 0);
}

int main(int argc, char **argv) {
    mygames_room room = {0};
    if (argc != 1 && argc != 2) return 2;
    started_room(&room);
    run_real_selfplay(&room, argc == 2 ? argv[1] : NULL);
    test_reject_bad_policy_and_truncation(&room);
    puts("mygames.ddz_dmc_selfplay: PASS");
    return 0;
}
