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
    {   /* A restored state cannot invent a landlord who has never bid. */
        mygames_ddz_match corrupt = next;
        corrupt.highest_seat = 2u;
        REQUIRE(mygames_ddz_match_validate(&corrupt) ==
                MYGAMES_DDZ_MATCH_INVALID_STATE);
    }
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
    {   /* An early conclusion requires a legitimate winning 3-point bid. */
        mygames_ddz_match corrupt = next;
        corrupt.highest_bid = 2u;
        REQUIRE(mygames_ddz_match_validate(&corrupt) ==
                MYGAMES_DDZ_MATCH_INVALID_STATE);
    }
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
    REQUIRE(next.scoring.landlord_nonpass == 1u &&
            next.scoring.farmers_nonpass == 0u &&
            next.scoring.bombs_rockets == 0u);
    REQUIRE(mygames_ddz_match_validate(&next) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_public_view(&match, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(public_view.round.last_cards == bit &&
            public_view.round.turn_seat == 1u);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 607u, 202u, 2u, 0u,
                                   &next, &play) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(play.passed && play.played_cards == 0u);
    REQUIRE(next.scoring.landlord_nonpass == 1u &&
            next.scoring.farmers_nonpass == 0u);
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
            public_view.round.winner_seat == 0u &&
            public_view.has_settlement &&
            public_view.settlement.bid_base == 3u &&
            public_view.settlement.bombs_rockets == 0u &&
            public_view.settlement.spring == MYGAMES_DDZ_SPRING_LANDLORD &&
            public_view.settlement.multiplier == 2u &&
            public_view.settlement.unit == 6u &&
            public_view.settlement.delta[0] == 12 &&
            public_view.settlement.delta[1] == -6 &&
            public_view.settlement.delta[2] == -6);
    (void)others;
}

static void test_real_bomb_and_rocket_only_after_legal_play(void) {
    mygames_ddz_match match = begin_test_match(), next = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result action = {0};
    mygames_ddz_match_public public_view = {0};
    mygames_ddz_settlement unchanged = {0};
    uint8_t landlord[20], farmers[34];
    uint64_t selected = rank_cards(0u, 4u) |
        (UINT64_C(1) << 52u) | (UINT64_C(1) << 53u);
    unsigned int i, own = 0u, other = 0u;
    const uint64_t four = rank_cards(0u, 4u);
    const uint64_t rocket = (UINT64_C(1) << 52u) | (UINT64_C(1) << 53u);
    {   /* Account for preselected four-of-kind + two jokers; 'own'
         * counts deck extraction below, NOT this selection loop. */
        unsigned int chosen = count_cards(selected);
        for (i = 0u; i < MYGAMES_DDZ_CARDS && chosen < 20u; ++i) {
            if ((selected & (UINT64_C(1) << i)) == 0u) {
                selected |= UINT64_C(1) << i;
                ++chosen;
            }
        }
        REQUIRE(chosen == 20u);
    }
    REQUIRE(count_cards(selected) == 20u);
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        if (selected & (UINT64_C(1) << i))
            landlord[own++] = (uint8_t)i;
        else farmers[other++] = (uint8_t)i;
    }
    REQUIRE(own == 20u && other == 34u);
    own = 0u; other = 0u;
    memset(match.initial_hands, 0, sizeof(match.initial_hands));
    match.secret_kitty = UINT64_C(0);
    for (i = 0u; i < 51u; ++i) {
        const uint8_t card = (i % 3u == 0u) ?
            landlord[own++] : farmers[other++];
        match.secret_deck[i] = card;
        match.initial_hands[i % 3u] |= UINT64_C(1) << card;
    }
    for (i = 51u; i < MYGAMES_DDZ_CARDS; ++i) {
        const uint8_t card = landlord[own++];
        match.secret_deck[i] = card;
        match.secret_kitty |= UINT64_C(1) << card;
    }
    REQUIRE(mygames_ddz_match_validate(&match) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 3u,
                                   &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE((match.round.hands[0] & four) == four);
    REQUIRE((match.round.hands[0] & rocket) == rocket);
    REQUIRE(mygames_ddz_match_settlement(&match, &unchanged) ==
            MYGAMES_DDZ_MATCH_WRONG_PHASE);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 1u, four,
                                   &next, &action) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(action.move.kind == MYGAMES_DDZ_MOVE_BOMB);
    REQUIRE(next.scoring.bombs_rockets == 1u &&
            next.scoring.landlord_nonpass == 1u);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 202u, 2u, 0u,
                                   &next, &action) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 303u, 3u, 0u,
                                   &next, &action) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.scoring.bombs_rockets == 1u);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 4u, rocket,
                                   &next, &action) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(action.move.kind == MYGAMES_DDZ_MOVE_ROCKET);
    REQUIRE(next.scoring.bombs_rockets == 2u &&
            next.scoring.landlord_nonpass == 2u &&
            next.scoring.farmers_nonpass == 0u);
    REQUIRE(mygames_ddz_match_public_view(&next, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(!public_view.has_settlement);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 4u, rocket,
                                   &next, &action) == MYGAMES_DDZ_MATCH_STALE_TURN);
    REQUIRE(match.scoring.bombs_rockets == 2u);
}

/* True M5b gameplay path: landlord has exactly ONE non-pass play;
 * one farmer responds and empties all 17 cards while the other farmer and
 * landlord pass. The winner must be a COUNTER-SPRING, not calculated from
 * a spoofed client-supplied tally. */
static void test_real_farmer_reverse_spring(void) {
    mygames_ddz_match match = begin_test_match(), next = {0};
    mygames_ddz_action_result play = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_settlement score = {0};
    mygames_ddz_match_public public_view = {0};
    uint64_t remaining;
    unsigned int i;
    memset(match.initial_hands, 0, sizeof(match.initial_hands));
    match.secret_kitty = UINT64_C(0);
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        const uint64_t bit = UINT64_C(1) << i;
        match.secret_deck[i] = (uint8_t)i;
        if (i < 51u) match.initial_hands[i % MYGAMES_DDZ_SEATS] |= bit;
        else match.secret_kitty |= bit;
    }
    REQUIRE(mygames_ddz_match_validate(&match) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_match_bid(&match, 42u, 500u, 101u, 1u, 3u,
                                   &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE((match.round.hands[0] & UINT64_C(1)) != 0u);
    REQUIRE((match.round.hands[1] & (UINT64_C(1) << 4u)) != 0u);
    /* Landlord leads the three-of-clubs; farmer responds with a FOUR. */
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 1u,
                                   UINT64_C(1), &next, &play) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 202u, 2u,
                                   UINT64_C(1) << 4u, &next, &play) ==
            MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 303u, 3u,
                                   UINT64_C(0), &next, &play) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u, 4u,
                                   UINT64_C(0), &next, &play) == MYGAMES_DDZ_MATCH_OK);
    match = next;
    REQUIRE(match.round.turn_seat == 1u);
    remaining = match.round.hands[1];
    REQUIRE(count_cards(remaining) == 16u);
    while (remaining) {
        const uint64_t single = remaining & (~remaining + UINT64_C(1));
        const uint64_t turn = match.round.turn_id;
        REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 202u, turn,
                                       single, &next, &play) ==
                MYGAMES_DDZ_MATCH_OK);
        match = next;
        remaining &= ~single;
        if (!remaining) break;
        REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 303u,
                match.round.turn_id, UINT64_C(0), &next, &play) ==
                MYGAMES_DDZ_MATCH_OK);
        match = next;
        REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 101u,
                match.round.turn_id, UINT64_C(0), &next, &play) ==
                MYGAMES_DDZ_MATCH_OK);
        match = next;
    }
    REQUIRE(match.phase == MYGAMES_DDZ_MATCH_FINISHED);
    REQUIRE(match.round.winner_seat == 1u && match.round.remaining[1] == 0u);
    REQUIRE(match.scoring.landlord_nonpass == 1u &&
            match.scoring.farmers_nonpass == 17u &&
            match.scoring.bombs_rockets == 0u);
    REQUIRE(mygames_ddz_match_settlement(&match, &score) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(score.spring == MYGAMES_DDZ_SPRING_FARMERS &&
            score.bid_base == 3u && score.multiplier == 2u &&
            score.unit == 6u);
    REQUIRE(score.delta[0] == -12 && score.delta[1] == 6 &&
            score.delta[2] == 6);
    REQUIRE(mygames_ddz_match_public_view(&match, &public_view) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(public_view.has_settlement &&
            public_view.settlement.spring == MYGAMES_DDZ_SPRING_FARMERS);
    REQUIRE(mygames_ddz_match_play(&match, 42u, 500u, 303u,
            match.round.turn_id, UINT64_C(0), &next, &play) ==
            MYGAMES_DDZ_MATCH_GAME_FINISHED);
}

static void test_authoritative_zero_sum_scoring(void) {
    mygames_ddz_settlement score = {0};
    mygames_ddz_settlement sentinel = {0};
    const mygames_ddz_score_counters spring = {0u, 1u, 0u};
    const mygames_ddz_score_counters ordinary_farmer = {1u, 3u, 2u};
    const mygames_ddz_score_counters reverse_spring = {2u, 1u, 3u};
    const mygames_ddz_score_counters bounded = {14u, 20u, 0u};

    REQUIRE(mygames_ddz_score_compute(3u, spring, 0u, 0u, &score) ==
            MYGAMES_DDZ_MATCH_OK);
    REQUIRE(score.spring == MYGAMES_DDZ_SPRING_LANDLORD &&
            score.multiplier == 2u && score.unit == 6u);
    REQUIRE(score.delta[0] == 12 && score.delta[1] == -6 &&
            score.delta[2] == -6);
    REQUIRE(score.delta[0] + score.delta[1] + score.delta[2] == 0);

    REQUIRE(mygames_ddz_score_compute(2u, ordinary_farmer, 1u, 0u,
                                     &score) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(score.spring == MYGAMES_DDZ_SPRING_NONE &&
            score.bombs_rockets == 1u && score.multiplier == 2u &&
            score.unit == 4u);
    REQUIRE(score.delta[0] == 4 && score.delta[1] == -8 &&
            score.delta[2] == 4);

    REQUIRE(mygames_ddz_score_compute(3u, reverse_spring, 2u, 0u,
                                     &score) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(score.spring == MYGAMES_DDZ_SPRING_FARMERS &&
            score.multiplier == 8u && score.unit == 24u);
    REQUIRE(score.delta[0] == 24 && score.delta[1] == 24 &&
            score.delta[2] == -48);

    REQUIRE(mygames_ddz_score_compute(3u, bounded, 1u, 1u,
                                     &score) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(score.multiplier == 32768u && score.unit == 98304u &&
            score.delta[0] == -98304 && score.delta[1] == 196608 &&
            score.delta[2] == -98304);

    sentinel = score;
    REQUIRE(mygames_ddz_score_compute(0u, spring, 0u, 0u, &score) ==
            MYGAMES_DDZ_MATCH_INVALID_ARGUMENT);
    REQUIRE(mygames_ddz_score_compute(3u,
            (mygames_ddz_score_counters){15u, 20u, 0u},
            0u, 0u, &score) == MYGAMES_DDZ_MATCH_INVALID_ARGUMENT);
    REQUIRE(mygames_ddz_score_compute(3u,
            (mygames_ddz_score_counters){0u, 0u, 1u},
            0u, 1u, &score) == MYGAMES_DDZ_MATCH_INVALID_ARGUMENT);
    REQUIRE(mygames_ddz_score_compute(3u, spring, 3u, 0u, &score) ==
            MYGAMES_DDZ_MATCH_INVALID_ARGUMENT);
    REQUIRE(memcmp(&sentinel, &score, sizeof(score)) == 0);
}

int main(void) {
    test_secure_deal_and_bid_privacy();
    test_early_three_all_pass_and_faults();
    test_live_os_shuffle_and_gameplay();
    test_winning_game_through_authored_fixture();
    test_authoritative_zero_sum_scoring();
    test_real_bomb_and_rocket_only_after_legal_play();
    test_real_farmer_reverse_spring();
    puts("mygames.ddz_match: PASS");
    return 0;
}
