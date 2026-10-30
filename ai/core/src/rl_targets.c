#include "mygames/rl_targets.h"

#include <math.h>
#include <stdbool.h>

static bool rl_compute(const mygames_rl_transition *steps, size_t step_count,
                       unsigned players, double discount, double *out) {
    double continuation[MYGAMES_RL_MAX_PLAYERS] = {0.0};
    size_t t;
    for (t = step_count; t != 0u; --t) {
        const mygames_rl_transition *s = &steps[t - 1u];
        const double factor = pow(discount, (double)s->time_delta);
        unsigned p;
        for (p = 0u; p < players; ++p) {
            continuation[p] = s->reward[p] + factor * continuation[p];
            if (!isfinite(continuation[p])) return false;
            if (out) out[(t - 1u) * players + p] = continuation[p];
        }
    }
    return true;
}

mygames_rl_status mygames_rl_monte_carlo_returns(
    const mygames_rl_transition *steps, size_t step_count,
    double discount, double *out_returns, size_t out_capacity) {
    uint32_t players;
    uint64_t episode_id;
    size_t t;
    unsigned p;
    if (!steps || !out_returns || step_count == 0u ||
        !isfinite(discount) || discount < 0.0 || discount > 1.0)
        return MYGAMES_RL_INVALID_ARGUMENT;
    players = steps[0].player_count;
    episode_id = steps[0].episode_id;
    if (players == 0u || players > MYGAMES_RL_MAX_PLAYERS ||
        episode_id == 0u || step_count > SIZE_MAX / players)
        return MYGAMES_RL_INVALID_ARGUMENT;
    if (out_capacity < step_count * players)
        return MYGAMES_RL_CAPACITY;
    for (t = 0u; t < step_count; ++t) {
        const mygames_rl_transition *s = &steps[t];
        if (s->episode_id != episode_id || s->player_count != players ||
            s->actor_id >= players || s->next_actor_id >= players ||
            (t > 0u && s->state_revision <= steps[t - 1u].state_revision))
            return MYGAMES_RL_INVALID_TRAJECTORY;
        if (s->terminated && s->truncated)
            return MYGAMES_RL_INVALID_TRAJECTORY;
        if (t + 1u < step_count && (s->terminated || s->truncated))
            return MYGAMES_RL_INVALID_TRAJECTORY;
        for (p = 0u; p < MYGAMES_RL_MAX_PLAYERS; ++p) {
            if (!isfinite(s->reward[p]) ||
                (p >= players && s->reward[p] != 0.0))
                return MYGAMES_RL_INVALID_TRAJECTORY;
        }
    }
    if (!steps[step_count - 1u].terminated ||
        steps[step_count - 1u].truncated)
        return MYGAMES_RL_INCOMPLETE_EPISODE;
    if (!rl_compute(steps, step_count, players, discount, NULL))
        return MYGAMES_RL_INVALID_TRAJECTORY;
    (void)rl_compute(steps, step_count, players, discount, out_returns);
    return MYGAMES_RL_OK;
}
