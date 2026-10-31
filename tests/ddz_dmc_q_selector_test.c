#include "mygames/ddz_dmc_q_selector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d FAIL %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

typedef struct test_scorer {
    unsigned batches;
    size_t total_actions;
    bool reject;
    bool nan_score;
    bool omit_last_score;
    unsigned role_seen[MYGAMES_DDZ_SEATS];
} test_scorer;

static unsigned count_cards(uint64_t x) {
    unsigned n = 0u;
    while (x) { x &= x - UINT64_C(1); ++n; }
    return n;
}

/* Deterministic TEST score: number of selected physical cards.
 * This is NOT a trained DMC or a provider/fallback shipped to production. */
static bool test_score_batch(
    void *opaque, const mygames_ddz_ai_features *features,
    const int8_t *actions,
    size_t count, float *out) {
    test_scorer *ctx = (test_scorer *)opaque;
    size_t i, j;
    if (!ctx || !features || !actions || !out ||
        !count || count > MYGAMES_DDZ_DMC_Q_BATCH ||
        (unsigned)features->role >= MYGAMES_DDZ_SEATS ||
        features->x_count !=
        (features->role == MYGAMES_DDZ_AI_LANDLORD ?
            MYGAMES_DDZ_AI_LANDLORD_FEATURES :
            MYGAMES_DDZ_AI_FARMER_FEATURES))
        return false;
    ++ctx->batches;
    ++ctx->role_seen[features->role];
    ctx->total_actions += count;
    if (ctx->reject) return false;
    for (i = 0u; i < count; ++i) {
        unsigned cards = 0u;
        if (ctx->omit_last_score && i + 1u == count)
            continue; /* malformed model: claimed success, omitted Q */
        for (j = 0u; j < MYGAMES_DDZ_AI_CARD_FEATURES; ++j) {
            if (actions[i * MYGAMES_DDZ_AI_CARD_FEATURES + j] != 0 &&
                actions[i * MYGAMES_DDZ_AI_CARD_FEATURES + j] != 1)
                return false;
            cards += (unsigned)actions[i * MYGAMES_DDZ_AI_CARD_FEATURES + j];
        }
        out[i] = (float)cards;
    }
    if (ctx->nan_score) out[count - 1u] = NAN;
    return true;
}

static bool has_action(const uint64_t *v, size_t n, uint64_t m) {
    size_t i;
    for (i = 0u; i < n; ++i) if (v[i] == m) return true;
    return false;
}

static void test_full_legal_argmax_and_fail_closed(void) {
    mygames_ddz_round round = {0};
    mygames_ddz_public_view view = {0};
    mygames_ddz_ai_history public_history = {0};
    mygames_ddz_ai_observation observation = {0};
    mygames_ddz_dmc_q_provider model = {0};
    test_scorer scorer = {0};
    uint8_t deck[MYGAMES_DDZ_CARDS];
    uint64_t *masks, chosen = UINT64_C(0), expected = UINT64_C(0);
    uint64_t sentinel = UINT64_C(0xabcdeffedcba);
    size_t needed = 0u, actual = 0u, i;
    unsigned largest = 0u;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) deck[i] = (uint8_t)i;
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&round, &view) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_ai_history_init(&view, &public_history) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_observe(round.hands[0], &public_history,
                                   &observation) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_legal_actions(round.hands[0], &view, NULL, 0u,
                                         &needed) == MYGAMES_DDZ_AI_OK);
    REQUIRE(needed > 0u && needed <= MYGAMES_DDZ_DMC_Q_MAX_ACTIONS);
    masks = (uint64_t *)malloc(needed * sizeof(masks[0]));
    REQUIRE(masks != NULL);
    REQUIRE(mygames_ddz_ai_legal_actions(round.hands[0], &view, masks,
                                         needed, &actual) == MYGAMES_DDZ_AI_OK &&
            actual == needed);
    model.model_version = 42u;
    model.rules_version = 1u;
    model.role = MYGAMES_DDZ_AI_LANDLORD;
    model.score_batch = test_score_batch;
    model.context = &scorer;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_OK);
    REQUIRE(scorer.total_actions == needed && scorer.batches != 0u);
    REQUIRE(has_action(masks, needed, chosen));
    for (i = 0u; i < needed; ++i) {
        const unsigned cards = count_cards(masks[i]);
        if (cards > largest || (cards == largest && masks[i] < expected)) {
            largest = cards;
            expected = masks[i];
        }
    }
    REQUIRE(chosen == expected);

    chosen = sentinel;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 99u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_STALE &&
            chosen == sentinel);
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 2u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_STALE &&
            chosen == sentinel);
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               2u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_STALE &&
            chosen == sentinel);
    model.role = MYGAMES_DDZ_AI_LANDLORD_UP;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_STALE &&
            chosen == sentinel);
    model.role = MYGAMES_DDZ_AI_LANDLORD;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks,
               MYGAMES_DDZ_DMC_Q_MAX_ACTIONS + 1u,
               1u, 42u, 1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_CAPACITY &&
            chosen == sentinel);
    scorer.reject = true;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_MODEL_FAILED &&
            chosen == sentinel);
    scorer.reject = false;
    scorer.nan_score = true;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_NONFINITE &&
            chosen == sentinel);
    scorer.nan_score = false;
    scorer.omit_last_score = true;
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, masks, needed, 1u, 42u,
               1u, &model, &chosen) == MYGAMES_DDZ_DMC_Q_NONFINITE &&
            chosen == sentinel);
    scorer.omit_last_score = false;
    /* PASS is illegal in a leading trick, even if an attacker supplied it
     * in place of the canonical complete legal list. */
    REQUIRE(mygames_ddz_dmc_q_pick(&observation, &((uint64_t){UINT64_C(0)}),
               1u, 1u, 42u, 1u, &model, &chosen) ==
                MYGAMES_DDZ_DMC_Q_INVALID_ACTION && chosen == sentinel);
    {
        uint64_t repeated[MYGAMES_DDZ_DMC_Q_BATCH + 7u];
        const unsigned previous_batches = scorer.batches;
        for (i = 0u; i < MYGAMES_DDZ_DMC_Q_BATCH + 7u; ++i)
            repeated[i] = masks[i % needed];
        REQUIRE(mygames_ddz_dmc_q_pick(&observation, repeated,
                   MYGAMES_DDZ_DMC_Q_BATCH + 7u, 1u, 42u, 1u,
                   &model, &chosen) == MYGAMES_DDZ_DMC_Q_OK);
        REQUIRE(scorer.batches == previous_batches + 2u);
    }
    free(masks);
}

static void make_started_room(mygames_room *room) {
    unsigned seat = 77u;
    REQUIRE(mygames_room_init(room, "dmc-q-bounded", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(room, 101u) == MYGAMES_ROOM_OK);
}

static void test_actual_offline_selfplay_policy_adapter(void) {
    mygames_room room = {0};
    mygames_ddz_dmc_q_policy policy = {0};
    mygames_ddz_dmc_sample *storage;
    mygames_ddz_dmc_selfplay_config cfg = {0};
    mygames_ddz_dmc_episode episode = {0}, untouched;
    mygames_ddz_settlement score = {0};
    test_scorer scoring = {0};
    unsigned role;
    make_started_room(&room);
    storage = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*storage));
    REQUIRE(storage != NULL);
    cfg.room_epoch = 15u;
    cfg.game_id = 17u;
    cfg.episode_id = 31u;
    cfg.model_version = 42u;
    cfg.rules_version = 1u;
    cfg.objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    cfg.max_legal_actions = MYGAMES_DDZ_DMC_Q_MAX_ACTIONS;
    cfg.offline_landlord_seat = 1u;
    policy.required_model_version = 42u;
    policy.required_rules_version = 1u;
    for (role = 0u; role < MYGAMES_DDZ_SEATS; ++role) {
        policy.providers[role].model_version = 42u;
        policy.providers[role].rules_version = 1u;
        policy.providers[role].role = (mygames_ddz_ai_role)role;
        policy.providers[role].score_batch = test_score_batch;
        policy.providers[role].context = &scoring;
    }
    REQUIRE(mygames_ddz_dmc_selfplay_run(&room, &cfg,
                mygames_ddz_dmc_q_policy_select, &policy,
                storage, MYGAMES_DDZ_DMC_MAX_STEPS,
                &episode, &score) == MYGAMES_DDZ_SELFPLAY_OK);
    REQUIRE(episode.sealed && episode.terminated &&
            episode.landlord_seat == 1u &&
            episode.count == policy.admitted_decisions &&
            score.landlord_seat == 1u);
    REQUIRE(policy.last_status == MYGAMES_DDZ_DMC_Q_OK);
    REQUIRE(scoring.role_seen[0u] > 0u);
    REQUIRE(scoring.total_actions >= episode.count);
    untouched = episode;
    policy.required_model_version = 43u;
    REQUIRE(mygames_ddz_dmc_selfplay_run(&room, &cfg,
                mygames_ddz_dmc_q_policy_select, &policy,
                storage, MYGAMES_DDZ_DMC_MAX_STEPS,
                &episode, &score) == MYGAMES_DDZ_SELFPLAY_POLICY_REJECTED);
    REQUIRE(policy.last_status == MYGAMES_DDZ_DMC_Q_STALE);
    REQUIRE(memcmp(&episode, &untouched, sizeof(untouched)) == 0);
    free(storage);
}

int main(void) {
    test_full_legal_argmax_and_fail_closed();
    test_actual_offline_selfplay_policy_adapter();
    puts("mygames.ddz_dmc_q_selector: PASS");
    return 0;
}
