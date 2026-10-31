#include "mygames/ddz_dmc_kfcore_adapter.h"

bool mygames_ddz_kfcore_q_score_batch(
    void *context, const mygames_ddz_ai_features *actor_features,
    const int8_t *action_rows, size_t action_count, float *out_scores) {
    const mygames_ddz_kfcore_q_context *selected =
        (const mygames_ddz_kfcore_q_context *)context;
    if (!selected || !selected->model || !actor_features ||
        !action_rows || !out_scores ||
        (unsigned)selected->role > MYGAMES_DDZ_AI_LANDLORD_DOWN ||
        selected->role != actor_features->role ||
        actor_features->x_count !=
          (selected->role == MYGAMES_DDZ_AI_LANDLORD ?
           MYGAMES_DDZ_AI_LANDLORD_FEATURES : MYGAMES_DDZ_AI_FARMER_FEATURES))
        return false;
    return kfcore_dmc_q_score(selected->model,
        selected->required_model_version, selected->required_rules_version,
        (uint32_t)selected->role,
        actor_features->x_no_action, actor_features->x_count,
        actor_features->z, MYGAMES_DDZ_AI_HISTORY_FEATURES,
        action_rows, action_count, out_scores) == KFCORE_DMC_Q_OK;
}
