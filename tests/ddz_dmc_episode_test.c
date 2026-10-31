#include "mygames/ddz_dmc_episode.h"
#include "mygames/ddz_dmc_replay.h"
#include "mygames/rl_targets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d failed %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)

static void started_room(mygames_room *room) {
    unsigned seat = 99u;
    REQUIRE(mygames_room_init(room, "dmc-offline", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    REQUIRE(mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    REQUIRE(mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(room, 101u) == MYGAMES_ROOM_OK);
}

static uint64_t lowest(uint64_t mask) {
    return mask & (~mask + UINT64_C(1));
}

static void test_offline_complete_match(const char *fixture_path) {
    mygames_room room = {0};
    mygames_ddz_match match = {0}, next = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_ai_history history = {0};
    mygames_ddz_public_view view = {0}, after = {0};
    mygames_ddz_dmc_episode episode = {0}, wp_episode = {0};
    mygames_ddz_dmc_sample *samples, *wp_samples;
    mygames_rl_transition transitions[MYGAMES_DDZ_DMC_MAX_STEPS] = {{0}};
    double returns[MYGAMES_DDZ_DMC_MAX_STEPS * 3u] = {0.0};
    mygames_ddz_settlement settlement = {0}, invalid_settlement;
    size_t t = 0u, p;
    unsigned landlord;
    const uint64_t epoch = UINT64_C(42), game_id = UINT64_C(600);
    samples = (mygames_ddz_dmc_sample *)calloc(MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    wp_samples = (mygames_ddz_dmc_sample *)calloc(MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*wp_samples));
    REQUIRE(samples != NULL && wp_samples != NULL);
    started_room(&room);
    REQUIRE(mygames_ddz_match_start(&match, &room, epoch, game_id) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_BIDDING);
    /* The authoritative first bidder bids 3. This is not an AI bidding
     * policy; it puts the real C11 match into PLAYING for DMC sampling. */
    landlord = match.bid_turn_seat;
    REQUIRE(mygames_ddz_match_bid(&match, epoch, game_id, match.players[landlord],
                                   match.bid_turn_id, 3u, &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_PLAYING);
    REQUIRE(match.highest_seat == landlord);
    REQUIRE(mygames_ddz_round_public_view(&match.round, &view) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_ai_history_init(&view, &history) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_dmc_episode_init(&episode, samples, MYGAMES_DDZ_DMC_MAX_STEPS,
                                        UINT64_C(77), UINT64_C(2), 1u,
                                        (uint8_t)landlord) == MYGAMES_DDZ_DMC_OK);
    REQUIRE(mygames_ddz_dmc_episode_seal(&episode, &settlement,
                                        MYGAMES_DDZ_DMC_GAME_POINTS) == MYGAMES_DDZ_DMC_NOT_TERMINAL);
    while (match.phase == MYGAMES_DDZ_MATCH_PLAYING) {
        unsigned actor = match.round.turn_seat;
        uint64_t own_hand = 0u, action;
        mygames_ddz_ai_observation observation = {0};
        mygames_ddz_action_result accepted = {0};
        mygames_rl_transition *transition;
        REQUIRE(t < MYGAMES_DDZ_DMC_MAX_STEPS);
        REQUIRE(mygames_ddz_match_private_hand(&match, epoch, game_id,
                   match.players[actor], &own_hand) == MYGAMES_DDZ_MATCH_OK);
        REQUIRE(mygames_ddz_ai_observe(own_hand, &history, &observation) ==
                 MYGAMES_DDZ_AI_OK);
        action = actor == landlord ? lowest(own_hand) : UINT64_C(0);
        /* Prove the real own-hand + public-only generator admits the first
         * proposed play and the first farmer PASS. */
        if (t < 2u) {
            size_t needed = 0u, count = 0u, i;
            bool found = false;
            uint64_t *candidates;
            REQUIRE(mygames_ddz_ai_legal_actions(own_hand, &view, NULL, 0u,
                                                &needed) == MYGAMES_DDZ_AI_OK);
            REQUIRE(needed > 0u && needed <= UINT64_C(1000000));
            candidates = (uint64_t *)malloc(needed * sizeof(uint64_t));
            REQUIRE(candidates != NULL);
            REQUIRE(mygames_ddz_ai_legal_actions(own_hand, &view,
                 candidates, needed, &count) == MYGAMES_DDZ_AI_OK);
            for (i = 0u; i < count; ++i)
                if (candidates[i] == action) found = true;
            REQUIRE(found);
            free(candidates);
        }
        REQUIRE(mygames_ddz_match_play(&match, epoch, game_id,
                 match.players[actor], match.round.turn_id, action,
                 &next, &accepted) == MYGAMES_DDZ_MATCH_OK);
        REQUIRE(mygames_ddz_dmc_episode_append(&episode, &observation,
                &accepted, action) == MYGAMES_DDZ_DMC_OK);
        REQUIRE(mygames_ddz_round_public_view(&next.round, &after) == MYGAMES_DDZ_OK);
        REQUIRE(mygames_ddz_ai_history_append(&history, &accepted, &after) ==
                 MYGAMES_DDZ_AI_OK);
        transition = &transitions[t];
        transition->episode_id = UINT64_C(77);
        transition->state_revision = match.round.turn_id;
        transition->model_version = UINT64_C(2);
        transition->player_count = 3u;
        transition->actor_id = actor;
        transition->next_actor_id = accepted.finished ? 3u : accepted.next_seat;
        transition->time_delta = 1u;
        transition->terminated = accepted.finished;
        match = next;
        view = after;
        ++t;
    }
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_FINISHED);
    REQUIRE(t == 58u);
    REQUIRE(episode.terminated && !episode.sealed && episode.count == t);
    REQUIRE(mygames_ddz_match_settlement(&match, &settlement) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(settlement.landlord_seat == landlord &&
            settlement.winner_seat == landlord &&
            settlement.bid_base == 3u && settlement.spring == MYGAMES_DDZ_SPRING_LANDLORD);
    REQUIRE(settlement.delta[landlord] == 12);
    for (p = 0u; p < 3u; ++p)
        if (p != landlord) REQUIRE(settlement.delta[p] == -6);
    for (p = 0u; p < 3u; ++p)
        transitions[t - 1u].reward[p] = (double)settlement.delta[p];
    REQUIRE(mygames_rl_monte_carlo_returns(transitions, t, 1.0, returns,
                                            t * 3u) == MYGAMES_RL_OK);
    /* A forged terminal score must not label the dataset. */
    invalid_settlement = settlement;
    invalid_settlement.delta[landlord]++;
    REQUIRE(mygames_ddz_dmc_episode_seal(&episode, &invalid_settlement,
                   MYGAMES_DDZ_DMC_GAME_POINTS) == MYGAMES_DDZ_DMC_INVALID);
    REQUIRE(!episode.sealed && episode.samples[0].target == 0.0);
    memcpy(wp_samples, samples, t * sizeof(*samples));
    wp_episode = episode;
    wp_episode.samples = wp_samples;
    REQUIRE(mygames_ddz_dmc_episode_seal(&episode, &settlement,
                   MYGAMES_DDZ_DMC_GAME_POINTS) == MYGAMES_DDZ_DMC_OK);
    REQUIRE(mygames_ddz_dmc_episode_seal(&wp_episode, &settlement,
                   MYGAMES_DDZ_DMC_WIN_SIGN) == MYGAMES_DDZ_DMC_OK);
    REQUIRE(mygames_ddz_dmc_episode_seal(&episode, &settlement,
                   MYGAMES_DDZ_DMC_GAME_POINTS) == MYGAMES_DDZ_DMC_SEALED);
    for (p = 0u; p < t; ++p) {
        const unsigned actor = transitions[p].actor_id;
        REQUIRE(samples[p].source_turn_id == transitions[p].state_revision);
        REQUIRE(samples[p].actor_seat == actor);
        REQUIRE(samples[p].target == (double)settlement.delta[actor]);
        REQUIRE(samples[p].target == returns[p * 3u + actor]);
        REQUIRE(wp_samples[p].target == (actor == landlord ? 1.0 : -1.0));
        REQUIRE(samples[p].features.x_count ==
            (actor == landlord ? MYGAMES_DDZ_AI_LANDLORD_FEATURES :
                                 MYGAMES_DDZ_AI_FARMER_FEATURES));
    }
    /* R1 private replay v1: a completed, authorized C11 match only.
     * The serialized bytes are caller-owned, never a public Outbox event. */
    {
        uint8_t *encoded;
        mygames_ddz_dmc_sample *decoded;
        mygames_ddz_dmc_episode reconstructed = {0};
        mygames_ddz_dmc_episode before = {0};
        size_t bytes = 0u, actual = 0u, required = 0u, k;
        const double original = samples[0].target;
        encoded = (uint8_t *)malloc(MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
        decoded = (mygames_ddz_dmc_sample *)calloc(
            MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*decoded));
        REQUIRE(encoded != NULL && decoded != NULL);
        REQUIRE(mygames_ddz_dmc_replay_encode(
            &episode, NULL, 0u, &bytes) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(bytes == MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES +
                 episode.count * MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES +
                 MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES);
        memset(encoded, 0x71, bytes);
        actual = 0u;
        REQUIRE(mygames_ddz_dmc_replay_encode(
            &episode, encoded, bytes - 1u, &actual) == MYGAMES_DDZ_REPLAY_CAPACITY);
        REQUIRE(actual == bytes);
        for (k = 0u; k < bytes; ++k) REQUIRE(encoded[k] == 0x71);
        REQUIRE(mygames_ddz_dmc_replay_encode(
            &episode, encoded, bytes, &actual) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(actual == bytes);
        REQUIRE(mygames_ddz_dmc_replay_decode(
            encoded, bytes, NULL, NULL, 0u, &required) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(required == episode.count);
        memset(&reconstructed, 0x4c, sizeof(reconstructed));
        before = reconstructed;
        required = 999u;
        REQUIRE(mygames_ddz_dmc_replay_decode(
            encoded, bytes, &reconstructed, decoded, 1u,
            &required) == MYGAMES_DDZ_REPLAY_CAPACITY);
        REQUIRE(required == episode.count &&
                memcmp(&reconstructed, &before, sizeof(before)) == 0);
        for (k = 0u; k < MYGAMES_DDZ_DMC_MAX_STEPS; ++k)
            REQUIRE(decoded[k].episode_id == 0u);
        required = 0u;
        REQUIRE(mygames_ddz_dmc_replay_decode(
            encoded, bytes, &reconstructed, decoded, MYGAMES_DDZ_DMC_MAX_STEPS,
            &required) == MYGAMES_DDZ_REPLAY_OK);
        REQUIRE(required == episode.count && reconstructed.sealed &&
                reconstructed.terminated &&
                reconstructed.episode_id == episode.episode_id &&
                reconstructed.model_version == episode.model_version &&
                reconstructed.objective == MYGAMES_DDZ_DMC_GAME_POINTS &&
                reconstructed.terminal_winner == episode.terminal_winner &&
                reconstructed.count == episode.count);
        for (k = 0u; k < episode.count; ++k) {
            REQUIRE(decoded[k].source_turn_id == samples[k].source_turn_id &&
                    decoded[k].chosen_cards == samples[k].chosen_cards &&
                    decoded[k].actor_seat == samples[k].actor_seat &&
                    decoded[k].role == samples[k].role &&
                    decoded[k].target == samples[k].target &&
                    decoded[k].features.x_count == samples[k].features.x_count &&
                    memcmp(decoded[k].features.x_no_action,
                           samples[k].features.x_no_action,
                           MYGAMES_DDZ_AI_MAX_FEATURES) == 0 &&
                    memcmp(decoded[k].features.z, samples[k].features.z,
                           MYGAMES_DDZ_AI_HISTORY_FEATURES) == 0 &&
                    memcmp(decoded[k].action_features,
                           samples[k].action_features,
                           MYGAMES_DDZ_AI_CARD_FEATURES) == 0);
        }
        if (fixture_path) {
            FILE *fp = fopen(fixture_path, "wb");
            REQUIRE(fp != NULL);
            REQUIRE(fwrite(encoded, 1u, bytes, fp) == bytes);
            REQUIRE(fclose(fp) == 0);
        }
        before = reconstructed;
        encoded[MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES + 32u] ^= 1u;
        required = 777u;
        REQUIRE(mygames_ddz_dmc_replay_decode(
            encoded, bytes, &reconstructed, decoded, MYGAMES_DDZ_DMC_MAX_STEPS,
            &required) == MYGAMES_DDZ_REPLAY_CORRUPT);
        REQUIRE(required == 777u &&
                memcmp(&reconstructed, &before, sizeof(before)) == 0);
        encoded[MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES + 32u] ^= 1u;
        REQUIRE(mygames_ddz_dmc_replay_decode(
            encoded, bytes - 1u, &reconstructed, decoded,
            MYGAMES_DDZ_DMC_MAX_STEPS, &required) == MYGAMES_DDZ_REPLAY_CORRUPT);
        REQUIRE(memcmp(&reconstructed, &before, sizeof(before)) == 0);
        samples[0].target += 1.0;
        actual = 888u;
        REQUIRE(mygames_ddz_dmc_replay_encode(
            &episode, encoded, bytes, &actual) == MYGAMES_DDZ_REPLAY_INVALID);
        REQUIRE(actual == 888u);
        samples[0].target = original;
        free(encoded);
        free(decoded);
    }
    free(samples);
    free(wp_samples);
}

int main(int argc, char **argv) {
    if (argc != 1 && argc != 2) return 2;
    test_offline_complete_match(argc == 2 ? argv[1] : NULL);
    return 0;
}
