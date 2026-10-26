#ifndef MYGAMES_DOUDIZHU_MATCH_H
#define MYGAMES_DOUDIZHU_MATCH_H

#include "mygames/ddz_round.h"
#include "mygames/room.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mygames_ddz_match_phase {
    MYGAMES_DDZ_MATCH_UNINITIALIZED = 0,
    MYGAMES_DDZ_MATCH_BIDDING,
    MYGAMES_DDZ_MATCH_REDEAL_REQUIRED,
    MYGAMES_DDZ_MATCH_PLAYING,
    MYGAMES_DDZ_MATCH_FINISHED
} mygames_ddz_match_phase;

typedef enum mygames_ddz_match_status {
    MYGAMES_DDZ_MATCH_OK = 0,
    MYGAMES_DDZ_MATCH_INVALID_ARGUMENT,
    MYGAMES_DDZ_MATCH_INVALID_ROOM,
    MYGAMES_DDZ_MATCH_INVALID_STATE,
    MYGAMES_DDZ_MATCH_ENTROPY_FAILURE,
    MYGAMES_DDZ_MATCH_WRONG_PHASE,
    MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER,
    MYGAMES_DDZ_MATCH_STALE_EPOCH,
    MYGAMES_DDZ_MATCH_STALE_GAME,
    MYGAMES_DDZ_MATCH_STALE_TURN,
    MYGAMES_DDZ_MATCH_NOT_YOUR_TURN,
    MYGAMES_DDZ_MATCH_BID_TOO_LOW,
    MYGAMES_DDZ_MATCH_ILLEGAL_PLAY,
    MYGAMES_DDZ_MATCH_GAME_FINISHED
} mygames_ddz_match_status;

/* M5g v1 scoring is one explicit (non-monetary) three-player house rule:
 * bid 1..3 points; every 4-card BOMB or both-joker ROCKET doubles; a
 * landlord victory with ZERO farmer non-pass plays is a spring; a farmer
 * victory after EXACTLY one landlord non-pass play is a counter-spring.
 * No client-supplied settlement, user money or hidden game history.
 * This aggregate is incremented ONLY after M5a accepts a real move and
 * is written with the private Game checkpoint in the SAME TurboDB COMMIT. */
typedef enum mygames_ddz_spring_kind {
    MYGAMES_DDZ_SPRING_NONE = 0,
    MYGAMES_DDZ_SPRING_LANDLORD = 1,
    MYGAMES_DDZ_SPRING_FARMERS = 2
} mygames_ddz_spring_kind;

typedef struct mygames_ddz_score_counters {
    uint8_t bombs_rockets;
    uint8_t landlord_nonpass;
    uint8_t farmers_nonpass;
} mygames_ddz_score_counters;

/* Public terminal-only scores. Values are integer game points, not cash.
 * Landlord victory: landlord +2*unit, each farmer -unit. Farmer victory:
 * landlord -2*unit, each farmer +unit. Always sum(delta)=0. */
typedef struct mygames_ddz_settlement {
    uint8_t bid_base;
    uint8_t bombs_rockets;
    mygames_ddz_spring_kind spring;
    uint8_t landlord_seat;
    uint8_t winner_seat;
    uint32_t multiplier;
    uint32_t unit;
    int32_t delta[MYGAMES_DDZ_SEATS];
} mygames_ddz_settlement;

/* PRIVATE: secret shuffled deck, undealt kitty, and ALL player hands are
 * Owner-only state. Never serialize this struct to MQTT, Cocos, logs, a
 * shared room snapshot or a public API. Do not print it for diagnostics.
 * A separately authenticated transaction owner must persist private data. */
typedef struct mygames_ddz_match {
    uint64_t room_epoch;
    uint64_t game_id;
    mygames_player_id players[MYGAMES_DDZ_SEATS];
    uint8_t secret_deck[MYGAMES_DDZ_CARDS];
    uint64_t initial_hands[MYGAMES_DDZ_SEATS];
    uint64_t secret_kitty;
    mygames_ddz_round round;
    uint64_t bid_turn_id;
    mygames_ddz_match_phase phase;
    uint8_t bid_start_seat;
    uint8_t bid_turn_seat;
    uint8_t bid_count;
    uint8_t highest_bid;
    uint8_t highest_seat;
    mygames_ddz_score_counters scoring;
} mygames_ddz_match;

/* A PUBLIC snapshot contains no hand masks, deck, hidden kitty or ticket.
 * The three bottom cards are revealed only AFTER the bidding terminates
 * with a landlord; all-pass keeps them hidden and requires a fresh shuffle.
 */
typedef struct mygames_ddz_match_public {
    uint64_t room_epoch;
    uint64_t game_id;
    uint64_t bid_turn_id;
    mygames_ddz_match_phase phase;
    uint8_t bid_turn_seat;
    uint8_t bid_count;
    uint8_t highest_bid;
    uint8_t highest_seat;
    uint64_t revealed_kitty;
    bool has_round;
    mygames_ddz_public_view round;
    bool has_settlement;
    mygames_ddz_settlement settlement;
} mygames_ddz_match_public;

typedef struct mygames_ddz_bid_result {
    uint64_t next_bid_turn_id;
    uint64_t revealed_kitty;
    uint8_t acting_seat;
    uint8_t bid;
    uint8_t next_bid_seat;
    uint8_t highest_bid;
    uint8_t landlord_seat;
    mygames_ddz_match_phase phase;
} mygames_ddz_bid_result;

/* Pure checked zero-sum calculation, no game state mutation. Output is
 * unchanged on failure, including on malformed winner/counter combination. */
mygames_ddz_match_status mygames_ddz_score_compute(
    uint8_t bid_base, mygames_ddz_score_counters counters,
    uint8_t landlord_seat, uint8_t winner_seat,
    mygames_ddz_settlement *out_settlement);

/* OS CSPRNG only: Linux getrandom, Windows BCryptGenRandom, Apple
 * arc4random_buf. Unbiased rejection-sampled Fisher-Yates. Fails closed
 * with destination unchanged if no supported system CSPRNG is available. */
mygames_ddz_match_status mygames_ddz_shuffle_secure(uint8_t out_deck[MYGAMES_DDZ_CARDS]);

/* Room must have transitioned to STARTING after three different seated,
 * ready players. A valid epoch and game_id come from the authoritative
 * Room Owner, never an untrusted WebSocket command. No Game ACK/commit.
 * On failure, out_match is unchanged. */
mygames_ddz_match_status mygames_ddz_match_start(mygames_ddz_match *out_match,
                                                 const mygames_room *started_room,
                                                 uint64_t room_epoch,
                                                 uint64_t game_id);
mygames_ddz_match_status mygames_ddz_match_validate(const mygames_ddz_match *match);

/* Immutable staged transition: caller MUST transactionally commit the
 * candidate private match + command digest/receipt + sanitized public
 * Outbox event before installing next or returning a gameplay Game ACK.
 * The player argument MUST already be authenticated by CHttp/CFlow. */
mygames_ddz_match_status mygames_ddz_match_bid(
    const mygames_ddz_match *match, uint64_t room_epoch, uint64_t game_id,
    mygames_player_id authenticated_player, uint64_t expected_bid_turn_id,
    uint8_t score, mygames_ddz_match *out_next, mygames_ddz_bid_result *out_result);
mygames_ddz_match_status mygames_ddz_match_play(
    const mygames_ddz_match *match, uint64_t room_epoch, uint64_t game_id,
    mygames_player_id authenticated_player, uint64_t expected_turn_id,
    uint64_t played_cards, mygames_ddz_match *out_next,
    mygames_ddz_action_result *out_result);

/* API output does not grant authorization by itself. Caller MUST bind
 * authenticated_player to an independently verified session principal and
 * room-epoch generation before releasing a private hand to the network.
 * All errors leave output unchanged. */
mygames_ddz_match_status mygames_ddz_match_private_hand(
    const mygames_ddz_match *match, uint64_t room_epoch, uint64_t game_id,
    mygames_player_id authenticated_player, uint64_t *out_cards);
/* Returns only after the authoritative game reached FINISHED.
 * All displayed points derive from the persisted legal move counters.
 * Never accept a client-provided score or claim settlement before COMMIT. */
mygames_ddz_match_status mygames_ddz_match_settlement(
    const mygames_ddz_match *match, mygames_ddz_settlement *out_settlement);

mygames_ddz_match_status mygames_ddz_match_public_view(
    const mygames_ddz_match *match, mygames_ddz_match_public *out_view);

#if defined(MYGAMES_DDZ_MATCH_TESTING)
/* Uninstalled, build-test-only deterministic entropy hook. It must never be
 * offered to a player, production ingress, or the published game API. */
typedef bool (*mygames_ddz_test_entropy_fn)(void *context, uint8_t *out, size_t bytes);
mygames_ddz_match_status mygames_ddz_match_start_test_entropy(
    mygames_ddz_match *out_match, const mygames_room *started_room,
    uint64_t room_epoch, uint64_t game_id,
    mygames_ddz_test_entropy_fn fill, void *context);
#endif

#ifdef __cplusplus
}
#endif
#endif
