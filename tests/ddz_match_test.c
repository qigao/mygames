#include "mygames/ddz_match.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tests stay enabled in NDEBUG/Release. No real player-supplied entropy. */
#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

typedef struct test_entropy {
    unsigned int calls;
    unsigned int reject_first;
    unsigned int fail_after;
} test_entropy;

static bool fill_test_entropy(void *context, uint8_t *out, size_t bytes) {
    test_entropy *rng = (test_entropy *)context;
    uint32_t value;
    unsigned int i;
    if (!rng || !out || bytes != 4u) return false;
    if (rng->fail_after != 0u && rng->calls >= rng->fail_after) return false;
    ++rng->calls;
    if (rng->reject_first && rng->calls == 1u) {
        value = UINT32_MAX; /* Must be rejected when bound=54. */
    } else {
        value = (uint32_t)(UINT32_C(0x12345678) +
                           (uint32_t)rng->calls * UINT32_C(0x9e3779b9));
    }
    for (i = 0u; i < 4u; ++i)
        out[i] = (uint8_t)(value >> (i * 8u));
    return true;
}

static void started_room(mygames_room *room) {
    unsigned int seat = 99u;
    REQUIRE(mygames_room_init(room, "match-m5b", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    REQUIRE(mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    REQUIRE(mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(room, 101u) == MYGAMES_ROOM_OK);
    REQUIRE(room->phase == MYGAMES_ROOM_STARTING && room->players == 3u);
}

static mygames_ddz_match begin_test_match(void) {
    mygames_room room;
    mygames_ddz_match match = {0};
    test_entropy random = {0};
    started_room(&room);
    random.reject_first = 1u;
    REQUIRE(mygames_ddz_match_start_test_entropy(
                &match, &room, 42u, 500u, fill_test_entropy, &random) ==
            MYGAMES_DDZ_MATCH_OK);
    /* 53 bounded selections plus the provably rejected 0xffffffff. */
    REQUIRE(random.calls >= 54u);
    REQUIRE(mygames_ddz_match_validate(&match) == MYGAMES_DDZ_MATCH_OK);
    return match;
}

static unsigned int count_cards(uint64_t bits) {
    unsigned int count = 0u;
    while (bits != 0u) { ++count; bits &= bits - UINT64_C(1); }
    return count;
}

static uint64_t rank_cards(unsigned int rank, unsigned int count) {
    uint64_t mask = UINT64_C(0);
    unsigned int i;
    REQUIRE(rank < 13u && count <= 4u);
    for (i = 0u; i < count; ++i)
        mask |= UINT64_C(1) << (rank * 4u + i);
    return mask;
}

static void test_secure_deal_and_bid_privacy(void) {
    mygames_ddz_match match = begin_test_match();
    mygames_ddz_match next = {0}, original;
    mygames_ddz_match_public view = {0}, old_public;
    mygames_ddz_bid_result event = {0}, old_event;
    uint64_t private_hand = UINT64_C(0xabad1dea), other_hand = 0u;
    REQUIRE(match.room_epoch == 42u && match.game_id == 500u);
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_BIDDING &&
            match.bid_turn_seat == 0u && match.bid_turn_id == 1u);
    REQUIRE(mygames_ddz_match_public_view(&match, &view) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(!view.has_round && view.revealed_kitty == 0u &&
            view.highest_seat == MYGAMES_DDZ_NO_SEAT);
    old_public = view;
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 500u, 101u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(count_cards(private_hand) == 17u);
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 500u, 202u,
                                          &other_hand) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(count_cards(other_hand) == 17u && (private_hand & other_hand) == 0u);
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 500u, 404u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER);
    REQUIRE(count_cards(private_hand) == 17u);
    REQUIRE(mygames_ddz_match_private_hand(&match, 41u, 500u, 101u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_STALE_EPOCH);
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 499u, 101u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_STALE_GAME);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 202u, 1u, 2u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_NOT_YOUR_TURN);
    REQUIRE(mygames_ddz_match_bid(&match, 41u, 500u, 101u, 1u, 1u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_STALE_EPOCH);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 501u, 101u, 1u, 1u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_STALE_GAME);
    original = match;
    old_event = event;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 1u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(memcmp(&match, &original, sizeof(match)) == 0);
    REQUIRE(next.bid_count == 1u && next.bid_turn_id == 2u &&
            next.highest_bid == 1u && next.highest_seat == 0u &&
            event.next_bid_seat == 1u && event.revealed_kitty == 0u);
    match = next;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 202u, 1u, 2u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_STALE_TURN);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 202u, 2u, 1u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_BID_TOO_LOW);
    old_event = event;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 202u, 2u, 0u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(event.revealed_kitty == 0u && next.bid_turn_seat == 2u);
    match = next;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 303u, 3u, 2u,
                                   &next, &event) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.phase == MYGAMES_DDZ_MATCH_PLAYING &&
            next.highest_seat == 2u && next.highest_bid == 2u);
    REQUIRE(event.landlord_seat == 2u &&
            count_cards(event.revealed_kitty) == 3u);
    REQUIRE(event.revealed_kitty == match.secret_kitty);
    match = next;
    REQUIRE(mygames_ddz_match_public_view(&match, &view) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(view.has_round && view.revealed_kitty == event.revealed_kitty &&
            view.round.remaining[2] == 20u && view.round.turn_seat == 2u);
    REQUIRE(old_public.revealed_kitty == 0u && !old_public.has_round);
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 500u, 303u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(count_cards(private_hand) == 20u &&
            (private_hand & match.secret_kitty) == match.secret_kitty);
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 500u, 202u,
                                          &other_hand) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(count_cards(other_hand) == 17u &&
            (other_hand & match.secret_kitty) == 0u);
    (void)old_event;
}

static void test_early_three_all_pass_and_faults(void) {
    mygames_ddz_match match = begin_test_match(), next = {0};
    mygames_ddz_bid_result result = {0};
    mygames_ddz_match_public public_view = {0}, saved_public;
    mygames_ddz_action_result action = {0};
    uint64_t private_hand = UINT64_C(0xdeadbeef);
    mygames_room room;
    test_entropy failed = {.fail_after = 2u};
    started_room(&room);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 3u,
                                   &next, &result) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.phase == MYGAMES_DDZ_MATCH_PLAYING &&
            next.bid_count == 1u && next.highest_seat == 0u);
    REQUIRE(result.landlord_seat == 0u && count_cards(result.revealed_kitty) == 3u);
    REQUIRE(mygames_ddz_match_bid(&next, 42u, 500u, 202u, 2u, 0u,
                                   &match, &result) == MYGAMES_DDZ_MATCH_WRONG_PHASE);
    match = begin_test_match();
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 0u,
                                   &next, &result) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 202u, 2u, 0u,
                                   &next, &result) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 303u, 3u, 0u,
                                   &next, &result) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.phase == MYGAMES_DDZ_MATCH_REDEAL_REQUIRED &&
            result.landlord_seat == MYGAMES_DDZ_NO_SEAT &&
            result.revealed_kitty == 0u);
    REQUIRE(mygames_ddz_match_public_view(&next, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(!public_view.has_round && public_view.revealed_kitty == 0u);
    REQUIRE(mygames_ddz_match_private_hand(&next, 42u, 500u, 101u,
                                          &private_hand) == MYGAMES_DDZ_MATCH_WRONG_PHASE);
    REQUIRE(private_hand == UINT64_C(0xdeadbeef));
    REQUIRE(mygames_ddz_match_play(&next, 42u, 500u, 101u, 1u, 0u,
                                   &match, &action) == MYGAMES_DDZ_MATCH_WRONG_PHASE);
    saved_public = public_view;
    next.secret_deck[1] = next.secret_deck[0];
    REQUIRE(mygames_ddz_match_validate(&next) == MYGAMES_DDZ_MATCH_INVALID_STATE);
    REQUIRE(mygames_ddz_match_public_view(&next, &public_view) ==
            MYGAMES_DDZ_MATCH_INVALID_STATE);
    REQUIRE(memcmp(&public_view, &saved_public, sizeof(public_view)) == 0);
    match = begin_test_match();
    REQUIRE(mygames_ddz_match_start_test_entropy(
                &match, &room, 42u, 500u,
                fill_test_entropy, &failed) == MYGAMES_DDZ_MATCH_ENTROPY_FAILURE);
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_BIDDING &&
            match.game_id == 500u); /* Atomic failure. */
    room.seats[1].ready = false;
    REQUIRE(mygames_ddz_match_start(&match, &room, 42u, 501u) ==
            MYGAMES_DDZ_MATCH_INVALID_ROOM);
    REQUIRE(match.game_id == 500u);
    room.seats[1].ready = true;
    REQUIRE(mygames_ddz_match_start(&match, &room, 0u, 501u) ==
            MYGAMES_DDZ_MATCH_INVALID_ARGUMENT);
    REQUIRE(match.game_id == 500u);
}

static void test_live_os_shuffle_and_gameplay(void) {
    mygames_room room;
    mygames_ddz_match match = {0}, next = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result play = {0}, prior_play = {0};
    mygames_ddz_match_public public_view = {0};
    uint64_t landlord_hand, bit;
    unsigned int repetitions;
    started_room(&room);
    /* This invokes the real host CSPRNG in Linux/macOS/Windows CI. */
    for (repetitions = 0u; repetitions < 8u; ++repetitions) {
        REQUIRE(mygames_ddz_match_start(&match, &room, 42u,
                                       600u + repetitions) == MYGAMES_DDZ_MATCH_OK);
        REQUIRE(mygames_ddz_match_validate(&match) == MYGAMES_DDZ_MATCH_OK);
        REQUIRE(count_cards(match.secret_kitty) == 3u);
    }
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 607u, 101u, 1u, 3u,
                                  &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_private_hand(&match, 42u, 607u, 101u,
                                          &landlord_hand) == MYGAMES_DDZ_MATCH_OK);
    bit = landlord_hand & (~landlord_hand + UINT64_C(1));
    prior_play = play;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 202u, 1u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_NOT_YOUR_TURN);
    REQUIRE(memcmp(&play, &prior_play, sizeof(play)) == 0);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 101u, 0u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_STALE_TURN);
    REQUIRE(mygames_ddz_match_play(&match, 41u, 607u, 101u, 1u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_STALE_EPOCH);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 608u, 101u, 1u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_STALE_GAME);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 404u, 1u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 101u, 1u, UINT64_C(0),
                                   &next, &play) == MYGAMES_DDZ_MATCH_ILLEGAL_PLAY);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 101u, 1u, bit,
                                   &next, &play) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(play.played_cards == bit && play.next_turn_id == 2u);
    REQUIRE(mygames_ddz_match_validate(&next) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_public_view(&match, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(public_view.round.last_cards == bit &&
            public_view.round.turn_seat == 1u);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 202u, 2u, 0u,
                                   &next, &play) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(play.passed && play.played_cards == 0u);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 303u, 3u, 0u,
                                   &next, &play) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(play.trick_cleared && next.round.turn_seat == 0u);
}

static void test_winning_game_through_authored_fixture(void) {
    mygames_ddz_match match = begin_test_match(), next = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result result = {0};
    mygames_ddz_match_public public_view = {0};
    uint64_t winning = UINT64_C(0), others = UINT64_C(0);
    uint8_t landlord[20], foreign[34];
    unsigned int a = 0u, b = 0u, i;
    for (i = 0u; i < 5u; ++i) winning |= rank_cards(i, 3u);
    for (i = 5u; i < 10u; ++i) winning |= rank_cards(i, 1u);
    REQUIRE(count_cards(winning) == 20u);
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        if (winning & (UINT64_C(1) << i)) landlord[a++] = (uint8_t)i;
        else foreign[b++] = (uint8_t)i;
    }
    REQUIRE(a == 20u && b == 34u);
    a = 0u; b = 0u;
    memset(match.initial_hands, 0, sizeof(match.initial_hands));
    match.secret_kitty = 0u;
    for (i = 0u; i < 51u; ++i) {
        const uint8_t card = i % 3u == 0u ? landlord[a++] : foreign[b++];
        match.secret_deck[i] = card;
        match.initial_hands[i % 3u] |= UINT64_C(1) << card;
    }
    for (i = 51u; i < MYGAMES_DDZ_CARDS; ++i) {
        const uint8_t card = landlord[a++];
        match.secret_deck[i] = card;
        match.secret_kitty |= UINT64_C(1) << card;
    }
    REQUIRE(a == 20u && b == 34u);
    REQUIRE(mygames_ddz_match_validate(&match) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 3u,
                                  &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(match.round.hands[0] == winning);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 1u, winning,
                                   &next, &result) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.phase == MYGAMES_DDZ_MATCH_FINISHED &&
            next.round.finished && next.round.winner_seat == 0u &&
            result.finished && result.played_cards == winning);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 202u, 2u,
                                   UINT64_C(0), &next, &result) ==
            MYGAMES_DDZ_MATCH_GAME_FINISHED);
    REQUIRE(mygames_ddz_match_public_view(&match, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(public_view.round.finished &&
            public_view.round.winner_seat == 0u);
    (void)others;
}

int main(void) {
    test_secure_deal_and_bid_privacy();
    test_early_three_all_pass_and_faults();
    test_live_os_shuffle_and_gameplay();
    test_winning_game_through_authored_fixture();
    puts("mygames.ddz_match: PASS");
    return 0;
}
