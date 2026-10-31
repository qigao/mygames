#include "mygames/ddz_dmc_q_cpu.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define DDZ_H 128u
#define DDZ_G (4u * DDZ_H)
#define DDZ_INPUT 162u
#define DDZ_WIDE 512u
#define DDZ_STEPS 5u

static const uint8_t q_magic[8] = { 'M', 'G', 'Q', 'F', '0', '0', '0', '1' };

static uint32_t q_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t q_get64(const uint8_t *p) {
    return (uint64_t)q_get32(p) | ((uint64_t)q_get32(p + 4u) << 32u);
}

static uint32_t q_crc32_zero_checksum(const uint8_t *p, size_t count) {
    uint32_t c = UINT32_C(0xffffffff);
    size_t i;
    for (i = 0u; i < count; ++i) {
        unsigned bit;
        c ^= (i >= 48u && i < 52u) ? 0u : p[i];
        for (bit = 0u; bit < 8u; ++bit)
            c = (c >> 1u) ^ ((c & 1u) ? UINT32_C(0xedb88320) : 0u);
    }
    return ~c;
}

static size_t q_weight_count(unsigned x_count) {
    if (x_count != 319u && x_count != 430u) return 0u;
    return DDZ_G * DDZ_INPUT + DDZ_G * DDZ_H + 2u * DDZ_G +
           DDZ_WIDE * (x_count + 54u + DDZ_H) + DDZ_WIDE +
           4u * (DDZ_WIDE * DDZ_WIDE + DDZ_WIDE) + DDZ_WIDE + 1u;
}

mygames_ddz_q_cpu_status mygames_ddz_q_cpu_load(
    const void *serialized, size_t serialized_len,
    uint64_t expected_model_version, uint32_t expected_rules_version,
    uint32_t expected_objective, uint32_t expected_role,
    float *owned_weights, size_t weight_capacity,
    mygames_ddz_q_cpu_model *out_model, size_t *out_required_weights) {
    const uint8_t *src = (const uint8_t *)serialized;
    uint32_t role, x_count, objective, rules, recorded_count;
    uint64_t model_version;
    size_t expected_count, i;
    mygames_ddz_q_cpu_model result = {0};
    if (!src || !out_required_weights ||
        serialized_len < MYGAMES_DDZ_Q_CPU_HEADER_SIZE ||
        serialized_len > MYGAMES_DDZ_Q_CPU_IMAGE_MAX ||
        expected_role > 2u || expected_model_version == 0u ||
        expected_rules_version == 0u ||
        (expected_objective != 1u && expected_objective != 2u) ||
        sizeof(float) != 4u || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        ((!owned_weights || !out_model) &&
         (owned_weights || out_model || weight_capacity != 0u)))
        return MYGAMES_DDZ_Q_CPU_INVALID;
    if (memcmp(src, q_magic, 8u) != 0 ||
        q_get32(src + 8u) != 1u || q_get32(src + 12u) != 0u ||
        q_get32(src + 52u) != 0u || q_get64(src + 56u) != 0u)
        return MYGAMES_DDZ_Q_CPU_CORRUPT;
    model_version = q_get64(src + 16u);
    rules = q_get32(src + 24u);
    objective = q_get32(src + 28u);
    role = q_get32(src + 32u);
    x_count = q_get32(src + 36u);
    recorded_count = q_get32(src + 40u);
    expected_count = q_weight_count(role == 0u ? 319u : 430u);
    if (role > 2u || x_count != (role == 0u ? 319u : 430u) ||
        recorded_count != expected_count ||
        q_get32(src + 44u) != expected_count * 4u ||
        serialized_len != MYGAMES_DDZ_Q_CPU_HEADER_SIZE + expected_count * 4u ||
        q_crc32_zero_checksum(src, serialized_len) != q_get32(src + 48u))
        return MYGAMES_DDZ_Q_CPU_CORRUPT;
    if (model_version != expected_model_version || rules != expected_rules_version ||
        objective != expected_objective || role != expected_role)
        return MYGAMES_DDZ_Q_CPU_VERSION;
    for (i = 0u; i < expected_count; ++i) {
        const uint32_t bits = q_get32(src + MYGAMES_DDZ_Q_CPU_HEADER_SIZE + 4u * i);
        float number;
        memcpy(&number, &bits, sizeof(number));
        if (!isfinite(number)) return MYGAMES_DDZ_Q_CPU_CORRUPT;
    }
    if (!owned_weights && !out_model) {
        *out_required_weights = expected_count;
        return MYGAMES_DDZ_Q_CPU_OK;
    }
    if (weight_capacity < expected_count) {
        *out_required_weights = expected_count;
        return MYGAMES_DDZ_Q_CPU_CAPACITY;
    }
    for (i = 0u; i < expected_count; ++i) {
        uint32_t bits = q_get32(src + MYGAMES_DDZ_Q_CPU_HEADER_SIZE + 4u * i);
        memcpy(&owned_weights[i], &bits, sizeof(bits));
    }
    result.weights = owned_weights;
    result.weight_count = expected_count;
    result.model_version = model_version;
    result.rules_version = rules;
    result.objective = objective;
    result.role = role;
    result.x_count = x_count;
    *out_model = result;
    *out_required_weights = expected_count;
    return MYGAMES_DDZ_Q_CPU_OK;
}

static bool q_binary(const int8_t *p, size_t count) {
    size_t i;
    if (!p) return false;
    for (i = 0u; i < count; ++i)
        if (p[i] != 0 && p[i] != 1) return false;
    return true;
}

static float q_sigmoid(float x) {
    /* Avoid exp overflow; the final sigmoid is always within [0,1]. */
    if (x >= 0.0f) return 1.0f / (1.0f + expf(-x));
    {
        const float e = expf(x);
        return e / (1.0f + e);
    }
}

static void q_lstm(const float *w, const int8_t z[MYGAMES_DDZ_Q_CPU_Z_SIZE],
                   float context[DDZ_H]) {
    const float *wih = w;
    const float *whh = wih + DDZ_G * DDZ_INPUT;
    const float *bih = whh + DDZ_G * DDZ_H;
    const float *bhh = bih + DDZ_G;
    float h[DDZ_H] = {0.0f}, c[DDZ_H] = {0.0f};
    unsigned step;
    for (step = 0u; step < DDZ_STEPS; ++step) {
        float gate[DDZ_G];
        unsigned g, i;
        for (g = 0u; g < DDZ_G; ++g) {
            float sum = bih[g];
            const float *wx = wih + g * DDZ_INPUT;
            const float *wh = whh + g * DDZ_H;
            for (i = 0u; i < DDZ_INPUT; ++i)
                sum += wx[i] * (float)z[step * DDZ_INPUT + i];
            {
                float hidden = bhh[g];
                for (i = 0u; i < DDZ_H; ++i) hidden += wh[i] * h[i];
                gate[g] = sum + hidden;
            }
        }
        for (i = 0u; i < DDZ_H; ++i) {
            const float input = q_sigmoid(gate[i]);
            const float forget = q_sigmoid(gate[DDZ_H + i]);
            const float candidate = tanhf(gate[2u * DDZ_H + i]);
            const float output = q_sigmoid(gate[3u * DDZ_H + i]);
            c[i] = forget * c[i] + input * candidate;
            h[i] = output * tanhf(c[i]);
        }
    }
    memcpy(context, h, sizeof(h));
}

static const float *q_fully_connected(const float *weights, const float *input,
                                      unsigned rows, unsigned cols,
                                      float *output, bool use_relu) {
    const float *bias = weights + (size_t)rows * cols;
    unsigned row, col;
    for (row = 0u; row < rows; ++row) {
        const float *w = weights + (size_t)row * cols;
        float sum = bias[row];
        for (col = 0u; col < cols; ++col) sum += w[col] * input[col];
        output[row] = use_relu && sum < 0.0f ? 0.0f : sum;
    }
    return bias + rows;
}

mygames_ddz_q_cpu_status mygames_ddz_q_cpu_score(
    const mygames_ddz_q_cpu_model *model,
    const int8_t *x, size_t x_count,
    const int8_t z[MYGAMES_DDZ_Q_CPU_Z_SIZE],
    const int8_t *action_rows, size_t action_count, float *out_scores) {
    float context[DDZ_H];
    float xa[DDZ_H + MYGAMES_DDZ_Q_CPU_X_MAX + MYGAMES_DDZ_Q_CPU_ACTION_SIZE];
    float hidden0[DDZ_WIDE], hidden1[DDZ_WIDE];
    float results[MYGAMES_DDZ_Q_CPU_BATCH_MAX];
    const float *fc1;
    unsigned role, x_size, i;
    size_t a;
    if (!model || !model->weights || !x || !z || !action_rows || !out_scores ||
        action_count == 0u || action_count > MYGAMES_DDZ_Q_CPU_BATCH_MAX ||
        model->role > 2u || model->model_version == 0u ||
        model->rules_version == 0u ||
        (model->objective != 1u && model->objective != 2u))
        return MYGAMES_DDZ_Q_CPU_INVALID;
    role = model->role;
    x_size = role == 0u ? 319u : 430u;
    if (x_count != x_size || model->x_count != x_size ||
        model->weight_count != q_weight_count(x_size) ||
        !q_binary(x, x_size) || !q_binary(z, MYGAMES_DDZ_Q_CPU_Z_SIZE) ||
        !q_binary(action_rows, action_count * MYGAMES_DDZ_Q_CPU_ACTION_SIZE))
        return MYGAMES_DDZ_Q_CPU_INVALID;
    q_lstm(model->weights, z, context);
    for (i = 0u; i < DDZ_H; ++i) xa[i] = context[i];
    for (i = 0u; i < x_size; ++i) xa[DDZ_H + i] = (float)x[i];
    fc1 = model->weights + DDZ_G * DDZ_INPUT + DDZ_G * DDZ_H + 2u * DDZ_G;
    for (a = 0u; a < action_count; ++a) {
        const float *layer;
        unsigned k;
        for (k = 0u; k < MYGAMES_DDZ_Q_CPU_ACTION_SIZE; ++k)
            xa[DDZ_H + x_size + k] =
                (float)action_rows[a * MYGAMES_DDZ_Q_CPU_ACTION_SIZE + k];
        layer = q_fully_connected(fc1, xa, DDZ_WIDE,
                                   DDZ_H + x_size + MYGAMES_DDZ_Q_CPU_ACTION_SIZE,
                                   hidden0, true);
        for (k = 0u; k < 4u; ++k) {
            if (k % 2u == 0u)
                layer = q_fully_connected(layer, hidden0,
                                              DDZ_WIDE, DDZ_WIDE, hidden1, true);
            else
                layer = q_fully_connected(layer, hidden1,
                                              DDZ_WIDE, DDZ_WIDE, hidden0, true);
        }
        (void)q_fully_connected(layer, hidden0, 1u, DDZ_WIDE,
                                &results[a], false);
        if (!isfinite(results[a])) return MYGAMES_DDZ_Q_CPU_NONFINITE;
    }
    memcpy(out_scores, results, action_count * sizeof(results[0]));
    return MYGAMES_DDZ_Q_CPU_OK;
}
