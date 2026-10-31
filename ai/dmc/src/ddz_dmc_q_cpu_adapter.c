#include "mygames/ddz_dmc_q_cpu_adapter.h"

bool mygames_ddz_q_cpu_score_batch(
    void *model_context, const mygames_ddz_ai_features *features,
    const int8_t *action_rows, size_t action_count, float *out_scores) {
    const mygames_ddz_q_cpu_model *model =
        (const mygames_ddz_q_cpu_model *)model_context;
    if (!model || !features ||
        (unsigned)features->role != model->role ||
        features->x_count != model->x_count)
        return false;
    return mygames_ddz_q_cpu_score(model,
        features->x_no_action, features->x_count, features->z,
        action_rows, action_count, out_scores) == MYGAMES_DDZ_Q_CPU_OK;
}
