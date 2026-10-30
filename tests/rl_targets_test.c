#include "mygames/rl_targets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

static mygames_rl_transition example(unsigned step) {
    mygames_rl_transition t = {0};
    t.episode_id = 42u;
    t.state_revision = step + 1u;
    t.player_count = 3u;
    t.actor_id = step % 3u;
    t.next_actor_id = (step + 1u) % 3u;
    t.time_delta = 1u;
    t.model_version = 9u;
    return t;
}

static void test_team_terminal_credit(void) {
    mygames_rl_transition episode[3] = {0};
    double returns[9] = {0.0};
    unsigned t;
    for (t = 0u; t < 3u; ++t) episode[t] = example(t);
    episode[2].terminated = true;
    episode[2].next_actor_id = 3u; /* no actor after terminal */
    episode[2].reward[0] = 12.0;
    episode[2].reward[1] = -6.0;
    episode[2].reward[2] = -6.0;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 3u, 1.0, returns, 9u) == MYGAMES_RL_OK);
    for (t = 0u; t < 3u; ++t) {
        REQUIRE(NEAR(returns[t * 3u], 12.0));
        REQUIRE(NEAR(returns[t * 3u + 1u], -6.0));
        REQUIRE(NEAR(returns[t * 3u + 2u], -6.0));
    }
    /* Current actor changes every turn; each player's return does not. */
    REQUIRE(episode[0].actor_id == 0u && episode[1].actor_id == 1u && episode[2].actor_id == 2u);
}

static void test_reward_and_time_delta(void) {
    mygames_rl_transition episode[3] = {0};
    double returns[9] = {0.0};
    unsigned t;
    for (t = 0u; t < 3u; ++t) episode[t] = example(t);
    episode[0].reward[0] = 1.0;
    episode[0].reward[2] = -1.0;
    episode[1].reward[1] = 3.0;
    episode[1].reward[2] = -3.0;
    episode[1].time_delta = 0u; /* Same logical-time decision stage. */
    episode[2].reward[0] = 4.0;
    episode[2].reward[1] = -2.0;
    episode[2].reward[2] = -2.0;
    episode[2].terminated = true;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 3u, 0.5, returns, 9u) == MYGAMES_RL_OK);
    REQUIRE(NEAR(returns[0], 3.0) && NEAR(returns[1], 0.5) && NEAR(returns[2], -3.5));
    REQUIRE(NEAR(returns[3], 4.0) && NEAR(returns[4], 1.0) && NEAR(returns[5], -5.0));
    REQUIRE(NEAR(returns[6], 4.0) && NEAR(returns[7], -2.0) && NEAR(returns[8], -2.0));
}

static void test_reject_incomplete_and_invalid(void) {
    mygames_rl_transition episode[2] = {example(0u), example(1u)};
    double returns[6] = {123.0, 123.0, 123.0, 123.0, 123.0, 123.0};
    size_t i;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 6u) == MYGAMES_RL_INCOMPLETE_EPISODE);
    episode[1].truncated = true;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 6u) == MYGAMES_RL_INCOMPLETE_EPISODE);
    episode[1].terminated = true;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 6u) == MYGAMES_RL_INVALID_TRAJECTORY);
    episode[1].truncated = false;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 5u) == MYGAMES_RL_CAPACITY);
    episode[0].reward[0] = NAN;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 6u) == MYGAMES_RL_INVALID_TRAJECTORY);
    episode[0].reward[0] = 0.0;
    episode[1].state_revision = episode[0].state_revision;
    REQUIRE(mygames_rl_monte_carlo_returns(episode, 2u, 1.0, returns, 6u) == MYGAMES_RL_INVALID_TRAJECTORY);
    for (i = 0u; i < 6u; ++i) REQUIRE(returns[i] == 123.0);
}

int main(void) {
    test_team_terminal_credit();
    test_reward_and_time_delta();
    test_reject_incomplete_and_invalid();
    return 0;
}
