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
    /* R9e: real C11 mixed opponents and reserved untrainable source marker.
     * Assignment is by PHYSICAL seat, not landlord/farmer role. */
    {
        mygames_ddz_dmc_mixed_config mixed = {0};
        mygames_ddz_dmc_mixed_provenance meta = {0};
        fake_score mixed_q = {0};
        unsigned j;
        mixed.frozen_model_version = UINT64_C(102);
        mixed.rules_version = 1u;
        mixed.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
        for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
            uint32_t sum = 0u;
            mixed.episode_id = UINT64_C(91001) + i;
            mixed.offline_landlord_seat = (uint8_t)i;
            mixed.seat_behavior[0] = MYGAMES_DDZ_SEAT_FROZEN_Q;
            mixed.seat_behavior[1] = MYGAMES_DDZ_SEAT_SHED_MOST;
            mixed.seat_behavior[2] = MYGAMES_DDZ_SEAT_LEGAL_INDEX;
            returned = 0u;
            decisions = 0u;
            REQUIRE(mygames_ddz_dmc_mixed_collect(
                &mixed, q_from_cards, &mixed_q, bytes,
                MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
                &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_OK);
            REQUIRE(mygames_ddz_dmc_replay_decode(bytes, returned,
                &episode, samples, MYGAMES_DDZ_DMC_MAX_STEPS,
                &required) == MYGAMES_DDZ_REPLAY_OK);
            REQUIRE(episode.episode_id == mixed.episode_id &&
                episode.model_version == MYGAMES_DDZ_DMC_MIXED_REPLAY_SOURCE &&
                episode.model_version != mixed.frozen_model_version &&
                episode.sealed && episode.terminated &&
                episode.rules_version == 1u && episode.landlord_seat == i &&
                episode.count == required && episode.count == decisions);
            REQUIRE(meta.episode_id == mixed.episode_id &&
                meta.frozen_model_version == mixed.frozen_model_version &&
                meta.replay_source_marker == UINT64_MAX &&
                meta.landlord_seat == i &&
                meta.winner_seat == episode.terminal_winner);
            for (j = 0u; j < MYGAMES_DDZ_SEATS; ++j) {
                REQUIRE(meta.seat_behavior[j] == mixed.seat_behavior[j]);
                REQUIRE(meta.seat_decisions[j] > 0u);
                sum += meta.seat_decisions[j];
            }
            REQUIRE(sum == decisions);
            for (j = 0u; j < episode.count; ++j)
                REQUIRE(samples[j].model_version == UINT64_MAX &&
                        samples[j].episode_id == mixed.episode_id);
        }
        /* The physical Q seat becomes all three semantic roles as landlord
         * rotates; no unseen opponent-hand features cross the scorer ABI. */
        REQUIRE(mixed_q.calls > 0u && mixed_q.roles[0] &&
                mixed_q.roles[1] && mixed_q.roles[2]);

        memset(bytes, 0xA5, MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
        memset(&meta, 0x5A, sizeof(meta));
        returned = (size_t)0x12345;
        decisions = UINT32_C(123456);
        mixed.episode_id = UINT64_C(92001);
        mixed.offline_landlord_seat = 0u;
        mixed.seat_behavior[0] = MYGAMES_DDZ_SEAT_FROZEN_Q;
        mixed.seat_behavior[1] = MYGAMES_DDZ_SEAT_SHED_MOST;
        mixed.seat_behavior[2] = MYGAMES_DDZ_SEAT_LEGAL_INDEX;
        mixed_q.reject = true;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED);
        mixed_q.reject = false;
        mixed_q.nonfinite = true;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_MODEL_REJECTED);
        mixed_q.nonfinite = false;
        mixed.seat_behavior[1] = MYGAMES_DDZ_SEAT_FROZEN_Q;
        mixed.seat_behavior[2] = MYGAMES_DDZ_SEAT_FROZEN_Q;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_INVALID);
        mixed.seat_behavior[1] = MYGAMES_DDZ_SEAT_SHED_MOST;
        mixed.seat_behavior[0] = MYGAMES_DDZ_SEAT_LEGAL_INDEX;
        mixed.seat_behavior[2] = MYGAMES_DDZ_SEAT_LEGAL_INDEX;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_INVALID);
        mixed.seat_behavior[0] = MYGAMES_DDZ_SEAT_FROZEN_Q;
        mixed.seat_behavior[2] = 99u;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_INVALID);
        mixed.seat_behavior[2] = MYGAMES_DDZ_SEAT_LEGAL_INDEX;
        mixed.frozen_model_version = UINT64_MAX;
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes,
            MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_INVALID);
        mixed.frozen_model_version = UINT64_C(102);
        REQUIRE(mygames_ddz_dmc_mixed_collect(
            &mixed, q_from_cards, &mixed_q, bytes, 8u,
            &returned, &decisions, &meta) == MYGAMES_DDZ_ONPOLICY_CAPACITY);
        REQUIRE(returned == (size_t)0x12345 && decisions == UINT32_C(123456));
        for (j = 0u; j < MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES; ++j)
            REQUIRE(bytes[j] == (uint8_t)0xA5);
        {
            const uint8_t *p = (const uint8_t *)&meta;
            for (j = 0u; j < sizeof(meta); ++j)
                REQUIRE(p[j] == (uint8_t)0x5A);
        }
    }
    free(bytes);
    free(samples);
    puts("mygames.ddz_dmc_onpolicy: PASS");
    return 0;
}
