#ifndef MYGAMES_DDZ_DMC_KFCORE_ADAPTER_H
#define MYGAMES_DDZ_DMC_KFCORE_ADAPTER_H

#include "mygames/ddz_dmc_q_selector.h"
#include "kfcore/dmc_q.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional C11-only adapter. The selected KFCore handle is caller-owned,
 * immutable and loaded from a verified role-specific ONNX model package.
 * Do NOT use this callback without the original DMCQSelector full-legal-set
 * and model/rules/turn fences; no heuristic or C11 reference fallback.
 */
typedef struct mygames_ddz_kfcore_q_context {
    kfcore_dmc_q_handle *model;
    uint64_t required_model_version;
    uint32_t required_rules_version;
    mygames_ddz_ai_role role;
} mygames_ddz_kfcore_q_context;

bool mygames_ddz_kfcore_q_score_batch(
    void *context, const mygames_ddz_ai_features *actor_features,
    const int8_t *action_rows, size_t action_count, float *out_scores);

#ifdef __cplusplus
}
#endif

#endif
