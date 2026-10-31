#ifndef MYGAMES_DDZ_DMC_Q_CPU_H
#define MYGAMES_DDZ_DMC_Q_CPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit little-endian offline frozen-weight reference ABI. No game rules,
 * filesystem, model fallback or network dependency enters this module.
 * This slow C11 float32 CPU path is a numerical CONFORMANCE oracle for a
 * future optimized native/KFCore backend, not a production inference claim.
 * Actual trained weights are exported by the trusted offline PyTorch process.
 * CRC32 detects corruption, NOT identity or authenticity. */
#define MYGAMES_DDZ_Q_CPU_HEADER_SIZE 64u
#define MYGAMES_DDZ_Q_CPU_BATCH_MAX 64u
#define MYGAMES_DDZ_Q_CPU_X_MAX 430u
#define MYGAMES_DDZ_Q_CPU_Z_SIZE 810u
#define MYGAMES_DDZ_Q_CPU_ACTION_SIZE 54u
#define MYGAMES_DDZ_Q_CPU_WEIGHTS_MAX 1514497u
#define MYGAMES_DDZ_Q_CPU_IMAGE_MAX (MYGAMES_DDZ_Q_CPU_HEADER_SIZE + 4u * MYGAMES_DDZ_Q_CPU_WEIGHTS_MAX)

typedef enum mygames_ddz_q_cpu_status {
    MYGAMES_DDZ_Q_CPU_OK = 0,
    MYGAMES_DDZ_Q_CPU_INVALID,
    MYGAMES_DDZ_Q_CPU_CORRUPT,
    MYGAMES_DDZ_Q_CPU_CAPACITY,
    MYGAMES_DDZ_Q_CPU_VERSION,
    MYGAMES_DDZ_Q_CPU_NONFINITE
} mygames_ddz_q_cpu_status;

typedef struct mygames_ddz_q_cpu_model {
    const float *weights; /* caller-owned; MUST remain stable while scoring */
    size_t weight_count;
    uint64_t model_version;
    uint32_t rules_version;
    uint32_t objective;
    uint32_t role; /* 0 landlord, 1 landlord_up, 2 landlord_down */
    uint32_t x_count;
} mygames_ddz_q_cpu_model;

/* Query exact finite weight count by passing NULL arena/model (capacity 0).
 * On successful admission, fills caller-owned contiguous float32 arena and
 * publishes a model VIEW. Failure leaves both arena and model unchanged.
 * Complete exact byte length, reserved bytes, CRC32, float finiteness and
 * expected role/version/objective are checked BEFORE any writes. */
mygames_ddz_q_cpu_status mygames_ddz_q_cpu_load(
    const void *serialized, size_t serialized_len,
    uint64_t expected_model_version, uint32_t expected_rules_version,
    uint32_t expected_objective, uint32_t expected_role,
    float *owned_weights, size_t weight_capacity,
    mygames_ddz_q_cpu_model *out_model, size_t *out_required_weights);

/* z 5x162, role x 319/430, action rows 54. All input bytes MUST be binary;
 * score 1..64 candidates in source order. This computes LSTM context ONCE
 * per call and full five-layer ReLU Q for every action. On any failure,
 * including NaN/Inf internal computation, out_scores is byte-unchanged. */
mygames_ddz_q_cpu_status mygames_ddz_q_cpu_score(
    const mygames_ddz_q_cpu_model *model,
    const int8_t *x, size_t x_count,
    const int8_t z[MYGAMES_DDZ_Q_CPU_Z_SIZE],
    const int8_t *action_rows, size_t action_count,
    float *out_scores);

#ifdef __cplusplus
}
#endif
#endif
