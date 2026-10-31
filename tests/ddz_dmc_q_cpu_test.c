#include "mygames/ddz_dmc_q_cpu.h"
#include "mygames/ddz_dmc_q_cpu_adapter.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static void put32(uint8_t *p, uint32_t n) {
    unsigned i;
    for (i = 0u; i < 4u; ++i) p[i] = (uint8_t)(n >> (8u * i));
}
static void put64(uint8_t *p, uint64_t n) {
    put32(p, (uint32_t)n);
    put32(p + 4u, (uint32_t)(n >> 32u));
}
static uint32_t crc32_zero_checksum(const uint8_t *p, size_t bytes) {
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    for (i = 0u; i < bytes; ++i) {
        unsigned bit;
        crc ^= (i >= 48u && i < 52u) ? 0u : p[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    }
    return ~crc;
}

static void build_image(uint8_t *blob, size_t total, uint32_t role,
                        size_t count) {
    static const uint8_t magic[] = {'M', 'G', 'Q', 'F', '0', '0', '0', '1'};
    memset(blob, 0, total);
    memcpy(blob, magic, sizeof(magic));
    put32(blob + 8u, 1u);
    put64(blob + 16u, 20u);
    put32(blob + 24u, 1u);
    put32(blob + 28u, 1u);
    put32(blob + 32u, role);
    put32(blob + 36u, role == 0u ? 319u : 430u);
    put32(blob + 40u, (uint32_t)count);
    put32(blob + 44u, (uint32_t)(4u * count));
    put32(blob + 48u, crc32_zero_checksum(blob, total));
}

static void test_role(uint32_t role, size_t weight_count) {
    const size_t bytes = MYGAMES_DDZ_Q_CPU_HEADER_SIZE + 4u * weight_count;
    const unsigned xcount = role == 0u ? 319u : 430u;
    uint8_t *blob = (uint8_t *)malloc(bytes);
    float *arena = (float *)calloc(weight_count, sizeof(*arena));
    mygames_ddz_q_cpu_model model = {0}, snapshot;
    size_t required = 0u;
    float outputs[2] = {-111.0f, -222.0f};
    int8_t x[430] = {0}, z[810] = {0}, action[108] = {0};
    uint32_t bad = UINT32_C(0x7f800000); /* +Infinity */
    unsigned i;
    REQUIRE(blob && arena);
    build_image(blob, bytes, role, weight_count);
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              NULL, 0u, NULL, &required) == MYGAMES_DDZ_Q_CPU_OK);
    REQUIRE(required == weight_count);
    required = 0u;
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              arena, weight_count - 1u, &model, &required) ==
                MYGAMES_DDZ_Q_CPU_CAPACITY);
    REQUIRE(required == weight_count && model.weights == NULL);
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              arena, weight_count, &model, &required) == MYGAMES_DDZ_Q_CPU_OK);
    REQUIRE(model.weights == arena && model.weight_count == weight_count &&
            model.role == role && model.model_version == 20u);
    for (i = 0u; i < xcount; ++i) x[i] = (int8_t)(i % 2u);
    for (i = 0u; i < 810u; ++i) z[i] = (int8_t)(i % 3u == 0u);
    for (i = 0u; i < 108u; ++i) action[i] = (int8_t)(i % 5u == 0u);
    REQUIRE(mygames_ddz_q_cpu_score(&model, x, xcount, z, action,
                                    2u, outputs) == MYGAMES_DDZ_Q_CPU_OK);
    REQUIRE(outputs[0] == 0.0f && outputs[1] == 0.0f);
    /* Modify ONLY FC6.bias (the final float): an independent known golden
     * result Q=+1.5, regardless of state/history/actions. */
    put32(blob + bytes - 4u, UINT32_C(0x3fc00000));
    put32(blob + 48u, crc32_zero_checksum(blob, bytes));
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              arena, weight_count, &model, &required) == MYGAMES_DDZ_Q_CPU_OK);
    REQUIRE(mygames_ddz_q_cpu_score(&model, x, xcount, z, action,
                                    2u, outputs) == MYGAMES_DDZ_Q_CPU_OK);
    REQUIRE(outputs[0] == 1.5f && outputs[1] == 1.5f);
    {
        mygames_ddz_ai_features authorized_features = {0};
        authorized_features.role = (mygames_ddz_ai_role)role;
        authorized_features.x_count = xcount;
        REQUIRE(mygames_ddz_q_cpu_score_batch(&model, &authorized_features,
                    action, 2u, outputs));
        REQUIRE(outputs[0] == 1.5f && outputs[1] == 1.5f);
        authorized_features.role = (mygames_ddz_ai_role)((role + 1u) % 3u);
        outputs[0] = -111.0f;
        REQUIRE(!mygames_ddz_q_cpu_score_batch(&model, &authorized_features,
                    action, 2u, outputs));
        REQUIRE(outputs[0] == -111.0f);
        outputs[0] = 1.5f;
    }
    snapshot = model;
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 21u, 1u, 1u, role,
              arena, weight_count, &model, &required) == MYGAMES_DDZ_Q_CPU_VERSION);
    REQUIRE(memcmp(&model, &snapshot, sizeof(model)) == 0);
    blob[bytes - 4u] ^= 1u; /* corruption not reflected in CRC */
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              arena, weight_count, &model, &required) == MYGAMES_DDZ_Q_CPU_CORRUPT);
    REQUIRE(memcmp(&model, &snapshot, sizeof(model)) == 0);
    blob[bytes - 4u] ^= 1u;
    put32(blob + bytes - 4u, bad);
    put32(blob + 48u, crc32_zero_checksum(blob, bytes));
    REQUIRE(mygames_ddz_q_cpu_load(blob, bytes, 20u, 1u, 1u, role,
              arena, weight_count, &model, &required) == MYGAMES_DDZ_Q_CPU_CORRUPT);
    /* Invalid binary feature and too-large batch never touch output. */
    x[0] = 2;
    outputs[0] = -111.0f;
    REQUIRE(mygames_ddz_q_cpu_score(&model, x, xcount, z, action,
              2u, outputs) == MYGAMES_DDZ_Q_CPU_INVALID);
    REQUIRE(outputs[0] == -111.0f);
    x[0] = 0;
    REQUIRE(mygames_ddz_q_cpu_score(&model, x, xcount, z, action,
              MYGAMES_DDZ_Q_CPU_BATCH_MAX + 1u,
              outputs) == MYGAMES_DDZ_Q_CPU_INVALID);
    REQUIRE(outputs[0] == -111.0f);
    free(arena);
    free(blob);
}

int main(void) {
    test_role(0u, 1457665u);
    test_role(1u, 1514497u);
    test_role(2u, 1514497u);
    puts("mygames.ddz_dmc_q_cpu_reference: PASS");
    return 0;
}
