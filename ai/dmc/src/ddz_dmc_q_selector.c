#include "mygames/ddz_dmc_q_selector.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

mygames_ddz_dmc_q_status mygames_ddz_dmc_q_pick(
    const mygames_ddz_ai_observation *observation,
    const uint64_t *legal_actions, size_t legal_count,
    uint64_t required_turn_id, uint64_t required_model_version,
    uint32_t required_rules_version,
    const mygames_ddz_dmc_q_provider *provider,
    uint64_t *out_selected) {
    mygames_ddz_ai_features features = {0};
    size_t offset;
    bool has_best = false;
    float best_value = 0.0f;
    uint64_t best_action = UINT64_C(0);
    if (!observation || !legal_actions || !provider || !out_selected ||
        !provider->score_batch || legal_count == 0u ||
        required_turn_id == 0u || required_model_version == 0u ||
        required_rules_version == 0u)
        return MYGAMES_DDZ_DMC_Q_INVALID;
    if (legal_count > MYGAMES_DDZ_DMC_Q_MAX_ACTIONS)
        return MYGAMES_DDZ_DMC_Q_CAPACITY;
    if (provider->model_version != required_model_version ||
        provider->rules_version != required_rules_version ||
        provider->role != observation->role ||
        observation->public_history.current.turn_id != required_turn_id)
        return MYGAMES_DDZ_DMC_Q_STALE;
    if (mygames_ddz_ai_encode_douzero(observation, &features) !=
            MYGAMES_DDZ_AI_OK)
        return MYGAMES_DDZ_DMC_Q_INVALID;
    for (offset = 0u; offset < legal_count; offset += MYGAMES_DDZ_DMC_Q_BATCH) {
        int8_t actions[MYGAMES_DDZ_DMC_Q_BATCH][MYGAMES_DDZ_AI_CARD_FEATURES] = {{0}};
        float results[MYGAMES_DDZ_DMC_Q_BATCH] = {0.0f};
        const size_t count = legal_count - offset < MYGAMES_DDZ_DMC_Q_BATCH ?
                             legal_count - offset : MYGAMES_DDZ_DMC_Q_BATCH;
        size_t i;
        for (i = 0u; i < count; ++i) {
            if (mygames_ddz_ai_encode_action(
                    observation, legal_actions[offset + i], actions[i]) !=
                        MYGAMES_DDZ_AI_OK)
                return MYGAMES_DDZ_DMC_Q_INVALID_ACTION;
        }
        if (!provider->score_batch(provider->context, &features,
                                    &actions[0][0], count, results))
            return MYGAMES_DDZ_DMC_Q_MODEL_FAILED;
        for (i = 0u; i < count; ++i) {
            const float value = results[i];
            const uint64_t mask = legal_actions[offset + i];
            if (!isfinite(value)) return MYGAMES_DDZ_DMC_Q_NONFINITE;
            if (!has_best || value > best_value ||
                (value == best_value && mask < best_action)) {
                best_value = value;
                best_action = mask;
                has_best = true;
            }
        }
    }
    if (!has_best) return MYGAMES_DDZ_DMC_Q_INVALID;
    *out_selected = best_action;
    return MYGAMES_DDZ_DMC_Q_OK;
}

bool mygames_ddz_dmc_q_policy_select(
    void *opaque, const mygames_ddz_ai_observation *observation,
    const uint64_t *legal_actions, size_t legal_count,
    uint64_t *out_selected_action) {
    mygames_ddz_dmc_q_policy *context = (mygames_ddz_dmc_q_policy *)opaque;
    mygames_ddz_ai_role role;
    if (!context || !observation || !out_selected_action)
        return false;
    role = observation->role;
    if ((unsigned)role >= MYGAMES_DDZ_SEATS) {
        context->last_status = MYGAMES_DDZ_DMC_Q_INVALID;
        return false;
    }
    context->last_status = mygames_ddz_dmc_q_pick(
        observation, legal_actions, legal_count,
        observation->public_history.current.turn_id,
        context->required_model_version, context->required_rules_version,
        &context->providers[role], out_selected_action);
    if (context->last_status != MYGAMES_DDZ_DMC_Q_OK)
        return false;
    ++context->admitted_decisions;
    return true;
}
