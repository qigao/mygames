#include "mygames/ddz_dmc_onpolicy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d FAIL: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

typedef struct fake_score {
    unsigned calls;
    unsigned roles[MYGAMES_DDZ_SEATS];
    bool reject;
    bool nonfinite;
    bool omit_last;
} fake_score;

static bool q_from_cards(void *opaque, uint32_t role, const int8_t *x,
                         size_t x_count, const int8_t *z, size_t z_count,
                         const int8_t *actions, size_t action_count,
                         float *out_q) {
    fake_score *f = (fake_score *)opaque;
    size_t i, j;
    if (!f || !x || !z || !actions || !out_q || role >= MYGAMES_DDZ_SEATS ||
        x_count != (role == 0u ? 319u : 430u) || z_count != 810u ||
        action_count == 0u || action_count > MYGAMES_DDZ_DMC_Q_BATCH)
        return false;
    ++f->calls;
    ++f->roles[role];
    if (f->reject) return false;
    for (i = 0u; i < action_count; ++i) {
        unsigned cards = 0u;
        if (f->omit_last && i + 1u == action_count) continue;
        for (j = 0u; j < 54u; ++j) {
            if (actions[i * 54u + j] != 0 && actions[i * 54u + j] != 1)
                return false;
            cards += (unsigned)actions[i * 54u + j];
        }
        out_q[i] = (float)cards;
    }
    if (f->nonfinite) out_q[action_count - 1u] = NAN;
    return true;
}

int main(void) {
    mygames_ddz_dmc_onpolicy_config cfg = {0};
    fake_score fake = {0};
    uint8_t *bytes = (uint8_t *)malloc(MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
    mygames_ddz_dmc_sample *samples = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    mygames_ddz_dmc_episode episode = {0};
    size_t returned = 0u, required = 0u;
    uint32_t decisions = 0u;
    unsigned i;
    REQUIRE(bytes != NULL && samples != NULL);
    cfg.frozen_model_version = 42u;
    cfg.rules_version = 1u;
    cfg.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        cfg.episode_id = UINT64_C(7001) + i;
        cfg.offline_landlord_seat = (uint8_t)i;
        REQUIRE(mygames_ddz_dmc_onpolicy_collect(
            &cfg, q_from_cards, &fake, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES, &returned, &decisions) ==
                MYGAMES_DDZ_ONPOLICY_OK);
        REQUIRE(returned >= MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES +
                MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES +
                MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES);
        REQUIRE(mygames_ddz_dmc_replay_decode(bytes, returned,
                    NULL, NULL, 0u, &required) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(required == decisions && required > 0u);
        REQUIRE(mygames_ddz_dmc_replay_decode(bytes, returned,
                    &episode, samples, MYGAMES_DDZ_DMC_MAX_STEPS,
                    &required) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(episode.episode_id == cfg.episode_id &&
                episode.model_version == 42u && episode.rules_version == 1u &&
                episode.objective == MYGAMES_DDZ_DMC_WIN_SIGN &&
                episode.landlord_seat == i && episode.count == decisions &&
                episode.sealed && episode.terminated);
        {
            unsigned roles[3] = {0};
            size_t j;
            for (j = 0u; j < episode.count; ++j) {
                REQUIRE(samples[j].model_version == 42u &&
                        samples[j].episode_id == cfg.episode_id &&
                        (samples[j].target == 1.0 || samples[j].target == -1.0));
                REQUIRE((unsigned)samples[j].role < 3u);
                ++roles[samples[j].role];
            }
            REQUIRE(roles[0] > 0u && roles[1] > 0u && roles[2] > 0u);
        }
    }
    REQUIRE(fake.calls > 0u && fake.roles[0] && fake.roles[1] && fake.roles[2]);
    memset(bytes, 0xA5, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
    returned = (size_t)0x12345;
    decisions = UINT32_C(123456);
    cfg.episode_id = UINT64_C(8001);
    cfg.offline_landlord_seat = 0u;
    fake.reject = true;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED);
    fake.reject = false;
    fake.nonfinite = true;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED);
    fake.nonfinite = false;
    fake.omit_last = true;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED);
    fake.omit_last = false;
    cfg.frozen_model_version = 1u;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_INVALID);
    cfg.frozen_model_version = 42u;
    cfg.rules_version = 2u;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_INVALID);
    cfg.rules_version = 1u;
    REQUIRE(mygames_ddz_dmc_onpolicy_collect(
        &cfg, q_from_cards, &fake, bytes, 8u,
        &returned, &decisions) == MYGAMES_DDZ_ONPOLICY_CAPACITY);
    REQUIRE(returned == (size_t)0x12345 && decisions == UINT32_C(123456));
    for (i = 0u; i < MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES; ++i)
        REQUIRE(bytes[i] == (uint8_t)0xA5);
    free(bytes);
    free(samples);
    puts("mygames.ddz_dmc_onpolicy: PASS");
    return 0;
}
