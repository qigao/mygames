#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "mygames/ddz_match.h"

#include <limits.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <errno.h>
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h>
#endif

typedef bool (*ddz_entropy_fill)(void *context, uint8_t *out, size_t bytes);

static unsigned int ddz_count_bits(uint64_t x) {
    unsigned int count = 0u;
    while (x != UINT64_C(0)) {
        x &= x - UINT64_C(1);
        ++count;
    }
    return count;
}

static bool ddz_os_fill(void *context, uint8_t *out, size_t bytes) {
    (void)context;
    if (!out || bytes == 0u) return false;
#if defined(_WIN32)
    if (bytes > (size_t)ULONG_MAX) return false;
    return BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)bytes,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    size_t consumed = 0u;
    while (consumed < bytes) {
        const ssize_t rc = getrandom(out + consumed, bytes - consumed, 0u);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;
        consumed += (size_t)rc;
    }
    return true;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(out, bytes);
    return true;
#else
    return false; /* Unsupported platform: NEVER fall back to rand(). */
#endif
}

/* Unbiased index in [0,bound) from four independent OS CSPRNG bytes.
 * 2^32 (not UINT32_MAX) must be used for rejection to avoid modulo bias.
 * The retry bound fails closed under a failed/stuck entropy provider. */
static bool ddz_bounded_random(ddz_entropy_fill fill, void *context,
                               uint32_t bound, uint32_t *out_index) {
    const uint64_t space = UINT64_C(1) << 32u;
    uint64_t limit;
    unsigned int attempts;
    if (!fill || !out_index || bound < 2u || bound > MYGAMES_DDZ_CARDS)
        return false;
    limit = space - (space % (uint64_t)bound);
    for (attempts = 0u; attempts < 128u; ++attempts) {
        uint8_t bytes[4];
        uint32_t v;
        if (!fill(context, bytes, sizeof(bytes))) return false;
        v = ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) |
            ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
        if ((uint64_t)v < limit) {
            *out_index = v % bound;
            return true;
        }
    }
    return false;
}

static mygames_ddz_match_status ddz_shuffle_with_entropy(
    uint8_t deck[MYGAMES_DDZ_CARDS], ddz_entropy_fill fill, void *context) {
    uint8_t candidate[MYGAMES_DDZ_CARDS];
    unsigned int i;
    if (!deck || !fill) return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) candidate[i] = (uint8_t)i;
    for (i = MYGAMES_DDZ_CARDS - 1u; i > 0u; --i) {
        uint32_t at;
        uint8_t tmp;
        if (!ddz_bounded_random(fill, context, i + 1u, &at))
            return MYGAMES_DDZ_MATCH_ENTROPY_FAILURE;
        tmp = candidate[i];
        candidate[i] = candidate[at];
        candidate[at] = tmp;
    }
    memcpy(deck, candidate, sizeof(candidate));
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_shuffle_secure(
    uint8_t out_deck[MYGAMES_DDZ_CARDS]) {
    return ddz_shuffle_with_entropy(out_deck, ddz_os_fill, NULL);
}

static bool ddz_room_ready(const mygames_room *room, unsigned int *out_bid_seat) {
    unsigned int i, owner_count = 0u;
    if (!room || !out_bid_seat || room->phase != MYGAMES_ROOM_STARTING ||
        room->players != MYGAMES_DDZ_SEATS || room->owner == 0u ||
        room->revision == 0u) return false;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        unsigned int j;
        if (!room->seats[i].ready || room->seats[i].player == 0u)
            return false;
        if (room->seats[i].player == room->owner) {
            *out_bid_seat = i;
            ++owner_count;
        }
        for (j = 0u; j < i; ++j)
            if (room->seats[i].player == room->seats[j].player) return false;
    }
    return owner_count == 1u;
}

static mygames_ddz_match_status ddz_start_with_entropy(
    mygames_ddz_match *out_match, const mygames_room *room,
    uint64_t room_epoch, uint64_t game_id,
    ddz_entropy_fill fill, void *context) {
    mygames_ddz_match staged = {0};
    unsigned int i, opening_seat = 0u;
    if (!out_match || !room || !fill || room_epoch == 0u || game_id == 0u)
        return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    if (!ddz_room_ready(room, &opening_seat))
        return MYGAMES_DDZ_MATCH_INVALID_ROOM;
    staged.room_epoch = room_epoch;
    staged.game_id = game_id;
    staged.bid_start_seat = (uint8_t)opening_seat;
    staged.bid_turn_seat = (uint8_t)opening_seat;
    staged.bid_turn_id = UINT64_C(1);
    staged.highest_seat = MYGAMES_DDZ_NO_SEAT;
    staged.phase = MYGAMES_DDZ_MATCH_BIDDING;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        staged.players[i] = room->seats[i].player;
    if (ddz_shuffle_with_entropy(staged.secret_deck, fill, context) !=
        MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_ENTROPY_FAILURE;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        const uint64_t bit = UINT64_C(1) << staged.secret_deck[i];
        if (i < 51u)
            staged.initial_hands[i % MYGAMES_DDZ_SEATS] |= bit;
        else staged.secret_kitty |= bit;
    }
    if (mygames_ddz_match_validate(&staged) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    *out_match = staged;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_start(
    mygames_ddz_match *out_match, const mygames_room *room,
    uint64_t room_epoch, uint64_t game_id) {
    return ddz_start_with_entropy(out_match, room, room_epoch, game_id,
                                  ddz_os_fill, NULL);
}

#if defined(MYGAMES_DDZ_MATCH_TESTING)
mygames_ddz_match_status mygames_ddz_match_start_test_entropy(
    mygames_ddz_match *out_match, const mygames_room *room,
    uint64_t room_epoch, uint64_t game_id,
    mygames_ddz_test_entropy_fn fill, void *context) {
    return ddz_start_with_entropy(out_match, room, room_epoch, game_id,
                                  fill, context);
}
#endif

static bool ddz_match_seat(const mygames_ddz_match *match,
                           mygames_player_id player, unsigned int *out_seat) {
    unsigned int i;
    if (player == 0u || !out_seat) return false;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        if (match->players[i] == player) {
            *out_seat = i;
            return true;
        }
    }
    return false;
}

mygames_ddz_match_status mygames_ddz_match_validate(const mygames_ddz_match *match) {
    uint64_t seen = 0u, initial[MYGAMES_DDZ_SEATS] = {0}, kitty = 0u;
    unsigned int i;
    if (!match || match->room_epoch == 0u || match->game_id == 0u ||
        match->phase < MYGAMES_DDZ_MATCH_BIDDING ||
        match->phase > MYGAMES_DDZ_MATCH_FINISHED ||
        match->bid_start_seat >= MYGAMES_DDZ_SEATS ||
        match->bid_turn_id == 0u || match->bid_count > MYGAMES_DDZ_SEATS ||
        match->bid_turn_id != (uint64_t)match->bid_count + UINT64_C(1) ||
        match->highest_bid > 3u ||
        (match->highest_bid == 0u ?
            match->highest_seat != MYGAMES_DDZ_NO_SEAT :
            match->highest_seat >= MYGAMES_DDZ_SEATS))
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    /* A positive bid must originate from an already admitted bidding seat.
     * A score-3 winner must be the last bidder, because it ends bidding. */
    if (match->highest_bid != 0u) {
        bool seen_bidder = false;
        if (match->bid_count == 0u) return MYGAMES_DDZ_MATCH_INVALID_STATE;
        for (i = 0u; i < match->bid_count; ++i) {
            if ((match->bid_start_seat + i) % MYGAMES_DDZ_SEATS ==
                match->highest_seat) seen_bidder = true;
        }
        if (!seen_bidder ||
            (match->highest_bid == 3u &&
             (match->bid_start_seat + match->bid_count - 1u) %
                MYGAMES_DDZ_SEATS != match->highest_seat))
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
    }
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        unsigned int j;
        if (match->players[i] == 0u) return MYGAMES_DDZ_MATCH_INVALID_STATE;
        for (j = 0u; j < i; ++j)
            if (match->players[i] == match->players[j])
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
    }
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        const uint8_t card = match->secret_deck[i];
        uint64_t bit;
        if (card >= MYGAMES_DDZ_CARDS) return MYGAMES_DDZ_MATCH_INVALID_STATE;
        bit = UINT64_C(1) << card;
        if ((seen & bit) != 0u) return MYGAMES_DDZ_MATCH_INVALID_STATE;
        seen |= bit;
        if (i < 51u) initial[i % MYGAMES_DDZ_SEATS] |= bit;
        else kitty |= bit;
    }
    if (seen != MYGAMES_DDZ_CARD_MASK ||
        kitty != match->secret_kitty || ddz_count_bits(kitty) != 3u)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        if (match->initial_hands[i] != initial[i] ||
            ddz_count_bits(initial[i]) != 17u)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
    if (match->phase == MYGAMES_DDZ_MATCH_BIDDING ||
        match->phase == MYGAMES_DDZ_MATCH_REDEAL_REQUIRED) {
        const mygames_ddz_round empty = {0};
        if (memcmp(&match->round, &empty, sizeof(empty)) != 0)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
        /* Pre-landlord state has accepted no card actions, so there
         * cannot be a multiplier/spring carried from another match. */
        if (match->scoring.bombs_rockets != 0u ||
            match->scoring.landlord_nonpass != 0u ||
            match->scoring.farmers_nonpass != 0u)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
        if (match->phase == MYGAMES_DDZ_MATCH_BIDDING) {
            if (match->bid_count >= 3u ||
                match->highest_bid == 3u ||
                match->bid_turn_seat !=
                    (uint8_t)((match->bid_start_seat + match->bid_count) %
                              MYGAMES_DDZ_SEATS))
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
        } else if (match->bid_count != 3u || match->highest_bid != 0u ||
                   match->bid_turn_seat != MYGAMES_DDZ_NO_SEAT)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
        return MYGAMES_DDZ_MATCH_OK;
    }
    if (match->highest_bid == 0u || match->bid_turn_seat != MYGAMES_DDZ_NO_SEAT ||
        (match->bid_count != 3u && match->highest_bid != 3u) ||
        mygames_ddz_round_validate(&match->round) != MYGAMES_DDZ_OK ||
        match->round.landlord_seat != match->highest_seat ||
        match->round.finished != (match->phase == MYGAMES_DDZ_MATCH_FINISHED))
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        const uint64_t original = initial[i] |
            (i == match->highest_seat ? kitty : UINT64_C(0));
        if ((match->round.hands[i] & ~original) != 0u)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
    }
    {
        const uint64_t landlord_cards =
            initial[match->highest_seat] | kitty;
        const unsigned int landlord_played = ddz_count_bits(
            match->round.played_cards & landlord_cards);
        const unsigned int farmers_played = ddz_count_bits(
            match->round.played_cards & ~landlord_cards);
        const mygames_ddz_score_counters counters = match->scoring;
        const unsigned int nonpass =
            (unsigned int)counters.landlord_nonpass + counters.farmers_nonpass;
        if (counters.landlord_nonpass > landlord_played ||
            counters.farmers_nonpass > farmers_played ||
            counters.bombs_rockets > 14u ||
            (unsigned int)counters.bombs_rockets * 2u >
                landlord_played + farmers_played ||
            nonpass > landlord_played + farmers_played ||
            (landlord_played != 0u && counters.landlord_nonpass == 0u) ||
            (landlord_played == 0u && counters.landlord_nonpass != 0u) ||
            (farmers_played != 0u && counters.farmers_nonpass == 0u) ||
            nonpass >= match->round.turn_id)
            return MYGAMES_DDZ_MATCH_INVALID_STATE;
        if (match->phase == MYGAMES_DDZ_MATCH_FINISHED) {
            mygames_ddz_settlement settlement = {0};
            if (mygames_ddz_score_compute(match->highest_bid, counters,
                    match->highest_seat, match->round.winner_seat,
                    &settlement) != MYGAMES_DDZ_MATCH_OK)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
        }
    }
    return MYGAMES_DDZ_MATCH_OK;
}

/* Only admitted legal cards contribute. A pass, attempted play or receipt
 * replay never increments multipliers. 54 physical cards bound accepted
 * scoring events and the maximum possible base*2^15 fits int32. */
mygames_ddz_match_status mygames_ddz_score_compute(
    uint8_t bid_base, mygames_ddz_score_counters counters,
    uint8_t landlord_seat, uint8_t winner_seat,
    mygames_ddz_settlement *out_settlement) {
    mygames_ddz_settlement result = {0};
    unsigned int seat, exponent;
    bool landlord_won;
    if (!out_settlement || bid_base < 1u || bid_base > 3u ||
        landlord_seat >= MYGAMES_DDZ_SEATS || winner_seat >= MYGAMES_DDZ_SEATS ||
        counters.bombs_rockets > 14u ||
        counters.landlord_nonpass < 1u || counters.landlord_nonpass > 20u ||
        counters.farmers_nonpass > 34u ||
        counters.bombs_rockets >
            (unsigned int)counters.landlord_nonpass + counters.farmers_nonpass)
        return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    landlord_won = winner_seat == landlord_seat;
    if (!landlord_won && counters.farmers_nonpass == 0u)
        return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    result.spring = landlord_won && counters.farmers_nonpass == 0u ?
        MYGAMES_DDZ_SPRING_LANDLORD :
        (!landlord_won && counters.landlord_nonpass == 1u ?
         MYGAMES_DDZ_SPRING_FARMERS : MYGAMES_DDZ_SPRING_NONE);
    exponent = (unsigned int)counters.bombs_rockets +
               (result.spring != MYGAMES_DDZ_SPRING_NONE ? 1u : 0u);
    result.multiplier = UINT32_C(1) << exponent;
    result.unit = (uint32_t)bid_base * result.multiplier;
    result.bid_base = bid_base;
    result.bombs_rockets = counters.bombs_rockets;
    result.landlord_seat = landlord_seat;
    result.winner_seat = winner_seat;
    result.delta[landlord_seat] = landlord_won ?
        (int32_t)(result.unit * 2u) : -(int32_t)(result.unit * 2u);
    for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat) {
        if (seat != landlord_seat)
            result.delta[seat] = landlord_won ?
                -(int32_t)result.unit : (int32_t)result.unit;
    }
    *out_settlement = result;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_settlement(
    const mygames_ddz_match *match, mygames_ddz_settlement *out_settlement) {
    if (!out_settlement || !match) return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    if (mygames_ddz_match_validate(match) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    if (match->phase != MYGAMES_DDZ_MATCH_FINISHED)
        return MYGAMES_DDZ_MATCH_WRONG_PHASE;
    return mygames_ddz_score_compute(match->highest_bid, match->scoring,
            match->highest_seat, match->round.winner_seat, out_settlement);
}

static mygames_ddz_match_status ddz_scope(const mygames_ddz_match *match,
                                          uint64_t epoch, uint64_t game_id) {
    if (mygames_ddz_match_validate(match) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    if (epoch != match->room_epoch) return MYGAMES_DDZ_MATCH_STALE_EPOCH;
    if (game_id != match->game_id) return MYGAMES_DDZ_MATCH_STALE_GAME;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_bid(
    const mygames_ddz_match *match, uint64_t epoch, uint64_t game_id,
    mygames_player_id player, uint64_t expected_bid_turn_id,
    uint8_t score, mygames_ddz_match *out_next, mygames_ddz_bid_result *out_result) {
    mygames_ddz_match next;
    mygames_ddz_bid_result event = {0};
    unsigned int seat;
    mygames_ddz_match_status status;
    if (!match || !out_next || !out_result || score > 3u)
        return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    status = ddz_scope(match, epoch, game_id);
    if (status != MYGAMES_DDZ_MATCH_OK) return status;
    if (match->phase != MYGAMES_DDZ_MATCH_BIDDING)
        return MYGAMES_DDZ_MATCH_WRONG_PHASE;
    if (!ddz_match_seat(match, player, &seat))
        return MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER;
    if (match->bid_turn_id != expected_bid_turn_id)
        return MYGAMES_DDZ_MATCH_STALE_TURN;
    if (seat != match->bid_turn_seat)
        return MYGAMES_DDZ_MATCH_NOT_YOUR_TURN;
    if (score != 0u && score <= match->highest_bid)
        return MYGAMES_DDZ_MATCH_BID_TOO_LOW;
    next = *match;
    ++next.bid_count;
    ++next.bid_turn_id;
    event.acting_seat = (uint8_t)seat;
    event.bid = score;
    event.landlord_seat = MYGAMES_DDZ_NO_SEAT;
    if (score != 0u) {
        next.highest_bid = score;
        next.highest_seat = (uint8_t)seat;
    }
    if (score == 3u || next.bid_count == MYGAMES_DDZ_SEATS) {
        next.bid_turn_seat = MYGAMES_DDZ_NO_SEAT;
        if (next.highest_bid == 0u) {
            next.phase = MYGAMES_DDZ_MATCH_REDEAL_REQUIRED;
        } else {
            if (mygames_ddz_round_deal(&next.round, next.secret_deck,
                                      next.highest_seat) != MYGAMES_DDZ_OK)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
            next.phase = MYGAMES_DDZ_MATCH_PLAYING;
            event.landlord_seat = next.highest_seat;
            event.revealed_kitty = next.secret_kitty;
        }
    } else {
        next.bid_turn_seat = (uint8_t)((seat + 1u) % MYGAMES_DDZ_SEATS);
    }
    event.highest_bid = next.highest_bid;
    event.phase = next.phase;
    event.next_bid_seat = next.bid_turn_seat;
    event.next_bid_turn_id = next.bid_turn_id;
    if (mygames_ddz_match_validate(&next) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    *out_next = next;
    *out_result = event;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_play(
    const mygames_ddz_match *match, uint64_t epoch, uint64_t game_id,
    mygames_player_id player, uint64_t expected_turn_id,
    uint64_t cards_mask, mygames_ddz_match *out_next,
    mygames_ddz_action_result *out_result) {
    mygames_ddz_match next;
    mygames_ddz_action_result event = {0};
    mygames_ddz_match_status status;
    mygames_ddz_status rules;
    unsigned int seat;
    if (!match || !out_next || !out_result)
        return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    status = ddz_scope(match, epoch, game_id);
    if (status != MYGAMES_DDZ_MATCH_OK) return status;
    if (match->phase == MYGAMES_DDZ_MATCH_FINISHED)
        return MYGAMES_DDZ_MATCH_GAME_FINISHED;
    if (match->phase != MYGAMES_DDZ_MATCH_PLAYING)
        return MYGAMES_DDZ_MATCH_WRONG_PHASE;
    if (!ddz_match_seat(match, player, &seat))
        return MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER;
    if (expected_turn_id != match->round.turn_id)
        return MYGAMES_DDZ_MATCH_STALE_TURN;
    if (seat != match->round.turn_seat)
        return MYGAMES_DDZ_MATCH_NOT_YOUR_TURN;
    next = *match;
    rules = mygames_ddz_round_submit(&next.round, seat, expected_turn_id,
                                     cards_mask, &event);
    if (rules == MYGAMES_DDZ_STALE_TURN)
        return MYGAMES_DDZ_MATCH_STALE_TURN;
    if (rules == MYGAMES_DDZ_NOT_YOUR_TURN)
        return MYGAMES_DDZ_MATCH_NOT_YOUR_TURN;
    if (rules != MYGAMES_DDZ_OK)
        return MYGAMES_DDZ_MATCH_ILLEGAL_PLAY;
    if (event.played_cards != UINT64_C(0)) {
        if (seat == next.highest_seat) {
            if (next.scoring.landlord_nonpass >= 20u)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
            ++next.scoring.landlord_nonpass;
        } else {
            if (next.scoring.farmers_nonpass >= 34u)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
            ++next.scoring.farmers_nonpass;
        }
        if (event.move.kind == MYGAMES_DDZ_MOVE_BOMB ||
            event.move.kind == MYGAMES_DDZ_MOVE_ROCKET) {
            if (next.scoring.bombs_rockets >= 14u)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
            ++next.scoring.bombs_rockets;
        }
    }
    if (next.round.finished) next.phase = MYGAMES_DDZ_MATCH_FINISHED;
    if (mygames_ddz_match_validate(&next) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    *out_next = next;
    *out_result = event;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_private_hand(
    const mygames_ddz_match *match, uint64_t epoch, uint64_t game_id,
    mygames_player_id player, uint64_t *out_cards) {
    mygames_ddz_match_status status;
    unsigned int seat;
    uint64_t cards;
    if (!match || !out_cards) return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    status = ddz_scope(match, epoch, game_id);
    if (status != MYGAMES_DDZ_MATCH_OK) return status;
    if (!ddz_match_seat(match, player, &seat))
        return MYGAMES_DDZ_MATCH_UNKNOWN_PLAYER;
    if (match->phase == MYGAMES_DDZ_MATCH_REDEAL_REQUIRED)
        return MYGAMES_DDZ_MATCH_WRONG_PHASE;
    cards = match->phase == MYGAMES_DDZ_MATCH_BIDDING ?
        match->initial_hands[seat] : match->round.hands[seat];
    *out_cards = cards;
    return MYGAMES_DDZ_MATCH_OK;
}

mygames_ddz_match_status mygames_ddz_match_public_view(
    const mygames_ddz_match *match, mygames_ddz_match_public *out_view) {
    mygames_ddz_match_public view = {0};
    if (!out_view) return MYGAMES_DDZ_MATCH_INVALID_ARGUMENT;
    if (mygames_ddz_match_validate(match) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_MATCH_INVALID_STATE;
    view.room_epoch = match->room_epoch;
    view.game_id = match->game_id;
    view.bid_turn_id = match->bid_turn_id;
    view.phase = match->phase;
    view.bid_turn_seat = match->bid_turn_seat;
    view.bid_count = match->bid_count;
    view.highest_bid = match->highest_bid;
    view.highest_seat = match->highest_seat;
    if (match->phase == MYGAMES_DDZ_MATCH_PLAYING ||
        match->phase == MYGAMES_DDZ_MATCH_FINISHED) {
        if (mygames_ddz_round_public_view(&match->round, &view.round) !=
            MYGAMES_DDZ_OK) return MYGAMES_DDZ_MATCH_INVALID_STATE;
        view.has_round = true;
        view.revealed_kitty = match->secret_kitty;
        if (match->phase == MYGAMES_DDZ_MATCH_FINISHED) {
            if (mygames_ddz_match_settlement(match, &view.settlement) !=
                MYGAMES_DDZ_MATCH_OK)
                return MYGAMES_DDZ_MATCH_INVALID_STATE;
            view.has_settlement = true;
        }
    }
    *out_view = view;
    return MYGAMES_DDZ_MATCH_OK;
}
