#include "mygames/ddz_ai_features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d failed %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)

static unsigned popcount64(uint64_t x) {
    unsigned count = 0u;
    while (x != 0u) { ++count; x &= x - UINT64_C(1); }
    return count;
}

static uint64_t lowest(uint64_t x) {
    return x & (~x + UINT64_C(1));
}

static void assert_observation_is_public(
    const mygames_ddz_round *round,
    const mygames_ddz_ai_history *hist) {
    mygames_ddz_ai_observation one = {0};
    mygames_ddz_ai_observation two = {0};
    mygames_ddz_ai_features a = {0}, b = {0};
    mygames_ddz_round variant = *round;
    const unsigned actor = round->turn_seat;
    unsigned first = (actor + 1u) % 3u;
    unsigned second = (actor + 2u) % 3u;
    uint64_t x = lowest(variant.hands[first]);
    uint64_t y = lowest(variant.hands[second]);
    uint64_t temp;
    REQUIRE(first != actor && second != actor);
    REQUIRE(x != 0u && y != 0u && x != y);
    variant.hands[first] ^= x | y;
    variant.hands[second] ^= x | y;
    REQUIRE(mygames_ddz_round_validate(&variant) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&variant, &(mygames_ddz_public_view){0}) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_ai_observe(round->hands[actor], hist, &one) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_observe(variant.hands[actor], hist, &two) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_encode_douzero(&one, &a) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_encode_douzero(&two, &b) == MYGAMES_DDZ_AI_OK);
    REQUIRE(a.x_count == b.x_count && a.role == b.role);
    REQUIRE(memcmp(&a, &b, sizeof(a)) == 0);
    REQUIRE(one.own_hand == two.own_hand);
    /* A changed opponent hand does not change our actual legal candidates.
     * Allocate the EXACT advertised count; never assume a fixed Top-K. */
    {
        uint64_t *first_actions, *other_actions;
        size_t needed = 0u, n = 0u, m = 0u;
        REQUIRE(mygames_ddz_ai_legal_actions(round->hands[actor], &hist->current,
                 NULL, 0u, &needed) == MYGAMES_DDZ_AI_OK);
        REQUIRE(needed > 0u && needed <= UINT64_C(1000000));
        first_actions = (uint64_t *)malloc(needed * sizeof(uint64_t));
        other_actions = (uint64_t *)malloc(needed * sizeof(uint64_t));
        REQUIRE(first_actions != NULL && other_actions != NULL);
        REQUIRE(mygames_ddz_ai_legal_actions(round->hands[actor], &hist->current,
                     first_actions, needed, &n) == MYGAMES_DDZ_AI_OK);
        REQUIRE(mygames_ddz_ai_legal_actions(variant.hands[actor], &hist->current,
                     other_actions, needed, &m) == MYGAMES_DDZ_AI_OK);
        REQUIRE(n == needed && n == m &&
                memcmp(first_actions, other_actions, n * sizeof(uint64_t)) == 0);
        free(first_actions);
        free(other_actions);
    }
    temp = variant.hands[actor];
    REQUIRE(temp == round->hands[actor]);
}

static void test_initial_and_following(void) {
    mygames_ddz_round round = {0};
    mygames_ddz_public_view view = {0}, next_view = {0};
    mygames_ddz_ai_history history = {0}, before;
    mygames_ddz_ai_observation obs = {0};
    mygames_ddz_ai_features f = {0};
    mygames_ddz_action_result result = {0}, fake;
    int8_t action[54], guard[54];
    uint8_t deck[54];
    uint64_t selected;
    unsigned i;
    for (i = 0u; i < 54u; ++i) deck[i] = (uint8_t)i;
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&round, &view) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_ai_history_init(&view, &history) == MYGAMES_DDZ_AI_OK);
    REQUIRE(mygames_ddz_ai_observe(round.hands[0], &history, &obs) == MYGAMES_DDZ_AI_OK);
    REQUIRE(obs.role == MYGAMES_DDZ_AI_LANDLORD);
    REQUIRE(mygames_ddz_ai_encode_douzero(&obs, &f) == MYGAMES_DDZ_AI_OK);
    REQUIRE(f.x_count == 319u && f.role == MYGAMES_DDZ_AI_LANDLORD);
    REQUIRE(f.x_no_action[286] == 1); /* Landlord_up has 17 */
    REQUIRE(f.x_no_action[303] == 1); /* Landlord_down has 17 */
    REQUIRE(f.x_no_action[304] == 1); /* 0 bombs */
    for (i = 0u; i < MYGAMES_DDZ_AI_HISTORY_FEATURES; ++i)
        REQUIRE(f.z[i] == 0);
    memset(guard, 0x2a, sizeof(guard));
    REQUIRE(mygames_ddz_ai_encode_action(&obs, 0u, guard) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
    for (i = 0u; i < 54u; ++i) REQUIRE(guard[i] == 0x2a);
    selected = lowest(round.hands[0]);
    REQUIRE(mygames_ddz_ai_encode_action(&obs, selected, action) == MYGAMES_DDZ_AI_OK);
    REQUIRE(action[0] == 1);
    REQUIRE(mygames_ddz_round_submit(&round, 0u, view.turn_id,
                                      selected, &result) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&round, &next_view) == MYGAMES_DDZ_OK);
    before = history;
    fake = result;
    fake.next_turn_id += 1u;
    REQUIRE(mygames_ddz_ai_history_append(&history, &fake, &next_view) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
    REQUIRE(memcmp(&history, &before, sizeof(history)) == 0);
    REQUIRE(mygames_ddz_ai_history_append(&history, &result, &next_view) == MYGAMES_DDZ_AI_OK);
    REQUIRE(history.recent_count == 1u && history.recent_actions[0] == selected);
    REQUIRE(mygames_ddz_ai_observe(round.hands[1], &history, &obs) == MYGAMES_DDZ_AI_OK);
    REQUIRE(obs.role == MYGAMES_DDZ_AI_LANDLORD_DOWN);
    REQUIRE(mygames_ddz_ai_encode_douzero(&obs, &f) == MYGAMES_DDZ_AI_OK);
    REQUIRE(f.x_count == 430u && f.role == MYGAMES_DDZ_AI_LANDLORD_DOWN);
    REQUIRE(f.x_no_action[108] == 1); /* landlord played rank 3 */
    REQUIRE(f.x_no_action[216] == 1); /* current last trick rank 3 */
    REQUIRE(f.x_no_action[270] == 1); /* last landlord action rank 3 */
    REQUIRE(f.x_no_action[378 + 18] == 1); /* 19 landlord cards remain */
    REQUIRE(f.x_no_action[398 + 16] == 1); /* teammate has 17 */
    REQUIRE(f.x_no_action[415] == 1); /* 0 bombs */
    REQUIRE(f.z[14u * 54u] == 1); /* left padded 14 action slots */
    for (i = 0u; i < 14u * 54u; ++i) REQUIRE(f.z[i] == 0);
    assert_observation_is_public(&round, &history);
    /* Following PASS is encoded as an all-zero action. */
    REQUIRE(mygames_ddz_ai_encode_action(&obs, 0u, action) == MYGAMES_DDZ_AI_OK);
    for (i = 0u; i < 54u; ++i) REQUIRE(action[i] == 0);
}

static void test_window_and_terminal(void) {
    mygames_ddz_round round = {0};
    mygames_ddz_public_view view = {0}, following = {0};
    mygames_ddz_ai_history history = {0}, reconstructed = {0};
    mygames_ddz_ai_public_move moves[MYGAMES_DDZ_AI_MAX_PUBLIC_MOVES] = {0};
    mygames_ddz_ai_observation obs = {0};
    mygames_ddz_ai_features f = {0};
    mygames_ddz_action_result event = {0};
    uint8_t deck[54];
    unsigned i, action_count = 0u;
    for (i = 0u; i < 54u; ++i) deck[i] = (uint8_t)i;
    REQUIRE(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_round_public_view(&round, &view) == MYGAMES_DDZ_OK);
    REQUIRE(mygames_ddz_ai_history_init(&view, &history) == MYGAMES_DDZ_AI_OK);
    while (!round.finished) {
        uint64_t selected;
        unsigned seat = round.turn_seat;
        REQUIRE(action_count < 65u);
        selected = seat == 0u ? lowest(round.hands[0]) : UINT64_C(0);
        moves[action_count].turn_id = round.turn_id;
        moves[action_count].actor_seat = (uint8_t)seat;
        moves[action_count].cards_mask = selected;
        REQUIRE(mygames_ddz_round_submit(&round, seat, round.turn_id,
                                          selected, &event) == MYGAMES_DDZ_OK);
        REQUIRE(mygames_ddz_round_public_view(&round, &following) == MYGAMES_DDZ_OK);
        REQUIRE(mygames_ddz_ai_history_append(&history, &event, &following) == MYGAMES_DDZ_AI_OK);
        if (!round.finished) {
            REQUIRE(mygames_ddz_ai_observe(round.hands[round.turn_seat],
                                           &history, &obs) == MYGAMES_DDZ_AI_OK);
            REQUIRE(mygames_ddz_ai_encode_douzero(&obs, &f) == MYGAMES_DDZ_AI_OK);
            REQUIRE(f.x_count == (round.turn_seat == 0u ? 319u : 430u));
        }
        ++action_count;
    }
    REQUIRE(action_count == 58u); /* 20 landlord singles, 19x2 farmer PASS */
    REQUIRE(history.recent_count == MYGAMES_DDZ_AI_HISTORY_MOVES);
    REQUIRE(popcount64(history.played_by_seat[0]) == 20u);
    REQUIRE(history.played_by_seat[1] == 0u && history.played_by_seat[2] == 0u);
    REQUIRE(history.current.finished && history.current.remaining[0] == 0u);
    REQUIRE(mygames_ddz_ai_history_reconstruct(0u, moves, action_count,
                &following, &reconstructed) == MYGAMES_DDZ_AI_OK);
    REQUIRE(reconstructed.recent_count == history.recent_count &&
            reconstructed.bomb_count == history.bomb_count &&
            reconstructed.current.finished);
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        REQUIRE(reconstructed.played_by_seat[i] == history.played_by_seat[i]);
        REQUIRE(reconstructed.last_action_by_seat[i] ==
                history.last_action_by_seat[i]);
    }
    for (i = 0u; i < MYGAMES_DDZ_AI_HISTORY_MOVES; ++i) {
        REQUIRE(reconstructed.recent_actions[i] == history.recent_actions[i]);
        REQUIRE(reconstructed.recent_seats[i] == history.recent_seats[i]);
    }
    {
        const mygames_ddz_ai_history before = reconstructed;
        const mygames_ddz_public_view terminal = following;
        mygames_ddz_public_view wrong = following;
        moves[1].actor_seat = 0u; /* Must be farmer seat 1. */
        REQUIRE(mygames_ddz_ai_history_reconstruct(0u, moves, action_count,
                    &terminal, &reconstructed) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
        REQUIRE(memcmp(&reconstructed, &before, sizeof(before)) == 0);
        moves[1].actor_seat = 1u;
        wrong.turn_id++;
        REQUIRE(mygames_ddz_ai_history_reconstruct(0u, moves, action_count,
                    &wrong, &reconstructed) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
        REQUIRE(memcmp(&reconstructed, &before, sizeof(before)) == 0);
        REQUIRE(mygames_ddz_ai_history_reconstruct(0u, moves, action_count - 1u,
                    &terminal, &reconstructed) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
        REQUIRE(memcmp(&reconstructed, &before, sizeof(before)) == 0);
    }
    {
        mygames_ddz_ai_observation guard;
        memset(&guard, 0x7c, sizeof(guard));
        REQUIRE(mygames_ddz_ai_observe(0u, &history, &guard) == MYGAMES_DDZ_AI_INVALID_OBSERVATION);
        REQUIRE(((unsigned char *)&guard)[0] == 0x7c);
    }
}

int main(void) {
    test_initial_and_following();
    test_window_and_terminal();
    return 0;
}
