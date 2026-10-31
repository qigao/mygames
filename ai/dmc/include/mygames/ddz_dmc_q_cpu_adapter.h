#ifndef MYGAMES_DDZ_DMC_Q_CPU_ADAPTER_H
#define MYGAMES_DDZ_DMC_Q_CPU_ADAPTER_H

#include "mygames/ddz_dmc_q_cpu.h"
#include "mygames/ddz_dmc_q_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Offline reference CPU callback for the EXISTING DMCQSelector.
 * The selector remains the unique authority for complete legal actions.
 * A model snapshot is caller-owned and immutable; no hidden cards, rule
 * fallback, interpreter, HTTP/ORM or live Bot enters this bridge.
 * Not a deployment/performance-ready replacement for KFCore. */
bool mygames_ddz_q_cpu_score_batch(
    void *model_context, const mygames_ddz_ai_features *features,
    const int8_t *action_rows, size_t action_count, float *out_scores);

#ifdef __cplusplus
}
#endif
#endif
