#ifndef MYGAMES_RL_TARGETS_H
#define MYGAMES_RL_TARGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_RL_MAX_PLAYERS 8u

typedef enum mygames_rl_status {
    MYGAMES_RL_OK = 0,
    MYGAMES_RL_INVALID_ARGUMENT,
    MYGAMES_RL_INVALID_TRAJECTORY,
    MYGAMES_RL_INCOMPLETE_EPISODE,
    MYGAMES_RL_CAPACITY
} mygames_rl_status;

/* An environment transition's rewards belong to all players, not just the
 * actor. A fixed evaluator player p uses column p through the entire episode.
 * model_version denotes the policy which selected this step's action;
 * different roles may deliberately use different immutable snapshots.
 * time_delta=0 denotes a decision phase which does not advance logical time. */
typedef struct mygames_rl_transition {
    uint64_t episode_id;
    uint64_t state_revision;
    uint64_t model_version;
    uint32_t player_count;
    uint32_t actor_id;
    uint32_t next_actor_id;
    uint32_t time_delta;
    bool terminated;
    bool truncated;
    double reward[MYGAMES_RL_MAX_PLAYERS];
} mygames_rl_transition;

/* For each step t and player p, compute the full-episode Monte Carlo return:
 * G[t,p] = reward[t,p] + discount^time_delta[t] * G[t+1,p].
 * Outputs are step-major, with exactly player_count entries per step.
 * A true terminal episode is REQUIRED; truncated episodes need an explicit
 * bootstrap algorithm and must not be mislabeled as terminal MC targets.
 * The output is unchanged on all non-OK statuses. */
mygames_rl_status mygames_rl_monte_carlo_returns(
    const mygames_rl_transition *steps, size_t step_count,
    double discount, double *out_returns, size_t out_capacity);

#ifdef __cplusplus
}
#endif
#endif
