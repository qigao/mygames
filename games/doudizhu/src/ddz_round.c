#include "mygames/ddz_round.h"

#include <limits.h>
#include <string.h>

#define DDZ_RANK_COUNT 15u
#define DDZ_STRAIGHT_LAST_RANK 11u /* Ace; 2 and jokers cannot join chains. */

static unsigned int count_bits(uint64_t bits) {
    unsigned int result = 0u;
    while (bits != 0u) {
        bits &= bits - UINT64_C(1);
        ++result;
    }
    return result;
}

static bool valid_mask(uint64_t mask) {
    return (mask & ~MYGAMES_DDZ_CARD_MASK) == 0u;
}

static bool find_same(const uint8_t ranks[DDZ_RANK_COUNT], unsigned int wanted,
                      uint8_t *rank) {
    unsigned int i;
    for (i = 0u; i < DDZ_RANK_COUNT; ++i) {
        if (ranks[i] == wanted) {
            *rank = (uint8_t)i;
            return true;
        }
    }
    return false;
}

/* Checks that ALL selected cards are an unbroken chain of exactly 'each'
 * cards at each rank. Returning true never accepts 2 or joker as chain. */
static bool complete_chain(const uint8_t ranks[DDZ_RANK_COUNT], unsigned int each,
                           unsigned int minimum, unsigned int total, uint8_t *high,
                           uint8_t *length) {
    unsigned int lo = DDZ_RANK_COUNT, hi = 0u, occupied = 0u;
    unsigned int i;
    if (total < each * minimum || total % each != 0u) return false;
    for (i = 0u; i < DDZ_RANK_COUNT; ++i) {
        if (ranks[i] == 0u) continue;
        if (i > DDZ_STRAIGHT_LAST_RANK || ranks[i] != each) return false;
        if (lo == DDZ_RANK_COUNT) lo = i;
        hi = i;
        ++occupied;
    }
    if (lo == DDZ_RANK_COUNT || hi - lo + 1u != occupied ||
        occupied < minimum || occupied != total / each) return false;
    *high = (uint8_t)hi;
    *length = (uint8_t)occupied;
    return true;
}

/* Airplane wings exclude all ranks in the triple chain. Pair wings must
 * occupy distinct ranks; single wings may form a pair, but may not contain
 * an extra triple/four. These are explicit, version-1 house rules. */
static bool airplane(const uint8_t ranks[DDZ_RANK_COUNT], unsigned int total,
                     unsigned int per_group, uint8_t *high, uint8_t *length) {
    unsigned int groups, low, i, remaining, wings;
    bool found = false;
    if (per_group != 4u && per_group != 5u) return false;
    if (total < 2u * per_group || total % per_group != 0u) return false;
    groups = total / per_group;
    if (groups > DDZ_STRAIGHT_LAST_RANK + 1u) return false;
    for (low = 0u; low + groups <= DDZ_STRAIGHT_LAST_RANK + 1u; ++low) {
        bool valid = true;
        remaining = 0u;
        wings = 0u;
        for (i = 0u; i < DDZ_RANK_COUNT; ++i) {
            if (i >= low && i < low + groups) {
                if (ranks[i] != 3u) { valid = false; break; }
            } else {
                if (per_group == 4u) {
                    if (ranks[i] > 2u) { valid = false; break; }
                    remaining += ranks[i];
                } else {
                    if (ranks[i] != 0u && ranks[i] != 2u) {
                        valid = false;
                        break;
                    }
                    wings += (ranks[i] == 2u) ? 1u : 0u;
                }
            }
        }
        if (valid && (per_group == 4u ? remaining == groups : wings == groups)) {
            *high = (uint8_t)(low + groups - 1u);
            *length = (uint8_t)groups;
            found = true; /* Highest legal chain wins if ambiguous. */
        }
    }
    return found;
}

mygames_ddz_status mygames_ddz_cards_to_mask(const uint8_t *cards, size_t count,
                                              uint64_t *out_mask) {
    uint64_t mask = 0u;
    size_t i;
    if (!cards || !out_mask || count == 0u || count > 20u)
        return MYGAMES_DDZ_INVALID_ARGUMENT;
    for (i = 0u; i < count; ++i) {
        uint64_t bit;
        if (cards[i] >= MYGAMES_DDZ_CARDS) return MYGAMES_DDZ_INVALID_ARGUMENT;
        bit = UINT64_C(1) << cards[i];
        if ((mask & bit) != 0u) return MYGAMES_DDZ_INVALID_ARGUMENT;
        mask |= bit;
    }
    *out_mask = mask;
    return MYGAMES_DDZ_OK;
}

mygames_ddz_status mygames_ddz_classify(uint64_t cards_mask, mygames_ddz_move *out_move) {
    uint8_t ranks[DDZ_RANK_COUNT] = {0};
    mygames_ddz_move result = {0};
    uint8_t main_rank = 0u, high = 0u, chain_length = 1u;
    unsigned int count, i, pairs;
    if (!out_move || !valid_mask(cards_mask) || cards_mask == 0u)
        return MYGAMES_DDZ_INVALID_MOVE;
    count = count_bits(cards_mask);
    if (count > 20u) return MYGAMES_DDZ_INVALID_MOVE;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        if ((cards_mask & (UINT64_C(1) << i)) != 0u)
            ++ranks[i < 52u ? i / 4u : i - 39u];
    }
    result.card_count = (uint8_t)count;
    result.chain_length = 1u;
    if (count == 2u && ranks[13] == 1u && ranks[14] == 1u) {
        result.kind = MYGAMES_DDZ_MOVE_ROCKET;
        result.high_rank = 14u;
    } else if (count == 1u && find_same(ranks, 1u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_SINGLE;
        result.high_rank = main_rank;
    } else if (count == 2u && find_same(ranks, 2u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_PAIR;
        result.high_rank = main_rank;
    } else if (count == 3u && find_same(ranks, 3u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_TRIPLE;
        result.high_rank = main_rank;
    } else if (count == 4u && find_same(ranks, 4u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_BOMB;
        result.high_rank = main_rank;
    } else if (count == 4u && find_same(ranks, 3u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_TRIPLE_SINGLE;
        result.high_rank = main_rank;
    } else if (count == 5u && find_same(ranks, 3u, &main_rank) &&
               find_same(ranks, 2u, &high)) {
        result.kind = MYGAMES_DDZ_MOVE_TRIPLE_PAIR;
        result.high_rank = main_rank;
    } else if (complete_chain(ranks, 1u, 5u, count, &high, &chain_length)) {
        result.kind = MYGAMES_DDZ_MOVE_STRAIGHT;
        result.high_rank = high;
        result.chain_length = chain_length;
    } else if (complete_chain(ranks, 2u, 3u, count, &high, &chain_length)) {
        result.kind = MYGAMES_DDZ_MOVE_PAIR_CHAIN;
        result.high_rank = high;
        result.chain_length = chain_length;
    } else if (complete_chain(ranks, 3u, 2u, count, &high, &chain_length)) {
        result.kind = MYGAMES_DDZ_MOVE_TRIPLE_CHAIN;
        result.high_rank = high;
        result.chain_length = chain_length;
    } else if (count == 6u && find_same(ranks, 4u, &main_rank)) {
        result.kind = MYGAMES_DDZ_MOVE_FOUR_TWO_SINGLES;
        result.high_rank = main_rank;
    } else if (count == 8u && find_same(ranks, 4u, &main_rank)) {
        pairs = 0u;
        for (i = 0u; i < DDZ_RANK_COUNT; ++i) {
            if (i == main_rank) continue;
            if (ranks[i] == 2u) ++pairs;
            else if (ranks[i] != 0u) break;
        }
        if (i == DDZ_RANK_COUNT && pairs == 2u) {
            result.kind = MYGAMES_DDZ_MOVE_FOUR_TWO_PAIRS;
            result.high_rank = main_rank;
        }
    }
    if (result.kind == MYGAMES_DDZ_MOVE_NONE &&
        airplane(ranks, count, 4u, &high, &chain_length)) {
        result.kind = MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES;
        result.high_rank = high;
        result.chain_length = chain_length;
    }
    if (result.kind == MYGAMES_DDZ_MOVE_NONE &&
        airplane(ranks, count, 5u, &high, &chain_length)) {
        result.kind = MYGAMES_DDZ_MOVE_AIRPLANE_PAIRS;
        result.high_rank = high;
        result.chain_length = chain_length;
    }
    if (result.kind == MYGAMES_DDZ_MOVE_NONE) return MYGAMES_DDZ_INVALID_MOVE;
    *out_move = result;
    return MYGAMES_DDZ_OK;
}

bool mygames_ddz_beats(const mygames_ddz_move *challenge,
                       const mygames_ddz_move *previous) {
    if (!challenge || !previous || challenge->kind == MYGAMES_DDZ_MOVE_NONE)
        return false;
    if (previous->kind == MYGAMES_DDZ_MOVE_NONE) return true;
    if (previous->kind == MYGAMES_DDZ_MOVE_ROCKET) return false;
    if (challenge->kind == MYGAMES_DDZ_MOVE_ROCKET) return true;
    if (challenge->kind == MYGAMES_DDZ_MOVE_BOMB &&
        previous->kind != MYGAMES_DDZ_MOVE_BOMB) return true;
    if (challenge->kind != previous->kind ||
        challenge->chain_length != previous->chain_length ||
        challenge->card_count != previous->card_count) return false;
    return challenge->high_rank > previous->high_rank;
}

mygames_ddz_status mygames_ddz_round_deal(mygames_ddz_round *round,
                                          const uint8_t deck[MYGAMES_DDZ_CARDS],
                                          unsigned landlord_seat) {
    mygames_ddz_round candidate = {0};
    uint64_t seen = 0u;
    unsigned int i;
    if (!round || !deck || landlord_seat >= MYGAMES_DDZ_SEATS)
        return MYGAMES_DDZ_INVALID_ARGUMENT;
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        uint64_t bit;
        if (deck[i] >= MYGAMES_DDZ_CARDS) return MYGAMES_DDZ_INVALID_DECK;
        bit = UINT64_C(1) << deck[i];
        if ((seen & bit) != 0u) return MYGAMES_DDZ_INVALID_DECK;
        seen |= bit;
        candidate.hands[i < 51u ? i % MYGAMES_DDZ_SEATS : landlord_seat] |= bit;
    }
    if (seen != MYGAMES_DDZ_CARD_MASK) return MYGAMES_DDZ_INVALID_DECK;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        candidate.remaining[i] = (uint8_t)count_bits(candidate.hands[i]);
    candidate.landlord_seat = (uint8_t)landlord_seat;
    candidate.turn_seat = (uint8_t)landlord_seat;
    candidate.last_seat = MYGAMES_DDZ_NO_SEAT;
    candidate.winner_seat = MYGAMES_DDZ_NO_SEAT;
    candidate.turn_id = UINT64_C(1);
    *round = candidate;
    return MYGAMES_DDZ_OK;
}

mygames_ddz_status mygames_ddz_round_validate(const mygames_ddz_round *round) {
    uint64_t union_cards;
    unsigned int i;
    mygames_ddz_move classified;
    if (!round || round->landlord_seat >= MYGAMES_DDZ_SEATS ||
        round->turn_id == 0u || !valid_mask(round->played_cards) ||
        !valid_mask(round->last_cards) ||
        (round->last_cards & ~round->played_cards) != 0u ||
        round->passes > 1u) return MYGAMES_DDZ_INVALID_STATE;
    union_cards = round->played_cards;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i) {
        if (!valid_mask(round->hands[i]) ||
            (union_cards & round->hands[i]) != 0u ||
            count_bits(round->hands[i]) != round->remaining[i] ||
            round->remaining[i] > (i == round->landlord_seat ? 20u : 17u))
            return MYGAMES_DDZ_INVALID_STATE;
        union_cards |= round->hands[i];
    }
    if (union_cards != MYGAMES_DDZ_CARD_MASK) return MYGAMES_DDZ_INVALID_STATE;
    if (round->last_cards == 0u) {
        if (round->last_move.kind != MYGAMES_DDZ_MOVE_NONE ||
            round->last_seat != MYGAMES_DDZ_NO_SEAT || round->passes != 0u)
            return MYGAMES_DDZ_INVALID_STATE;
    } else {
        if (round->last_seat >= MYGAMES_DDZ_SEATS ||
            mygames_ddz_classify(round->last_cards, &classified) != MYGAMES_DDZ_OK ||
            classified.kind != round->last_move.kind ||
            classified.high_rank != round->last_move.high_rank ||
            classified.chain_length != round->last_move.chain_length ||
            classified.card_count != round->last_move.card_count)
            return MYGAMES_DDZ_INVALID_STATE;
    }
    if (round->finished) {
        if (round->winner_seat >= MYGAMES_DDZ_SEATS ||
            round->turn_seat != MYGAMES_DDZ_NO_SEAT ||
            round->remaining[round->winner_seat] != 0u ||
            round->last_seat != round->winner_seat)
            return MYGAMES_DDZ_INVALID_STATE;
    } else {
        if (round->turn_seat >= MYGAMES_DDZ_SEATS ||
            round->winner_seat != MYGAMES_DDZ_NO_SEAT ||
            round->remaining[0] == 0u || round->remaining[1] == 0u ||
            round->remaining[2] == 0u)
            return MYGAMES_DDZ_INVALID_STATE;
    }
    return MYGAMES_DDZ_OK;
}

mygames_ddz_status mygames_ddz_round_submit(mygames_ddz_round *round,
                                            unsigned acting_seat,
                                            uint64_t expected_turn_id,
                                            uint64_t cards_mask,
                                            mygames_ddz_action_result *out_result) {
    mygames_ddz_round next;
    mygames_ddz_action_result event = {0};
    mygames_ddz_move move = {0};
    unsigned int i;
    if (!round || !out_result || acting_seat >= MYGAMES_DDZ_SEATS ||
        !valid_mask(cards_mask)) return MYGAMES_DDZ_INVALID_ARGUMENT;
    if (mygames_ddz_round_validate(round) != MYGAMES_DDZ_OK)
        return MYGAMES_DDZ_INVALID_STATE;
    if (round->finished) return MYGAMES_DDZ_FINISHED;
    if (expected_turn_id != round->turn_id) return MYGAMES_DDZ_STALE_TURN;
    if (acting_seat != round->turn_seat) return MYGAMES_DDZ_NOT_YOUR_TURN;
    if (round->turn_id == UINT64_MAX) return MYGAMES_DDZ_TURN_OVERFLOW;
    if ((cards_mask & ~round->hands[acting_seat]) != 0u)
        return MYGAMES_DDZ_CARDS_NOT_OWNED;
    next = *round;
    event.acting_seat = (uint8_t)acting_seat;
    event.winner_seat = MYGAMES_DDZ_NO_SEAT;
    if (cards_mask == 0u) {
        if (next.last_seat == MYGAMES_DDZ_NO_SEAT)
            return MYGAMES_DDZ_CANNOT_PASS;
        ++next.passes;
        if (next.passes == MYGAMES_DDZ_SEATS - 1u) {
            next.passes = 0u;
            next.last_seat = MYGAMES_DDZ_NO_SEAT;
            next.last_cards = 0u;
            next.last_move = (mygames_ddz_move){0};
            event.trick_cleared = true;
        }
        event.passed = true;
    } else {
        if (mygames_ddz_classify(cards_mask, &move) != MYGAMES_DDZ_OK)
            return MYGAMES_DDZ_INVALID_MOVE;
        if (next.last_seat != MYGAMES_DDZ_NO_SEAT &&
            !mygames_ddz_beats(&move, &next.last_move))
            return MYGAMES_DDZ_NOT_HIGHER;
        next.hands[acting_seat] &= ~cards_mask;
        next.played_cards |= cards_mask;
        next.remaining[acting_seat] = (uint8_t)count_bits(next.hands[acting_seat]);
        next.last_cards = cards_mask;
        next.last_move = move;
        next.last_seat = (uint8_t)acting_seat;
        next.passes = 0u;
        event.move = move;
        if (next.remaining[acting_seat] == 0u) {
            next.finished = true;
            next.winner_seat = (uint8_t)acting_seat;
            event.finished = true;
            event.winner_seat = (uint8_t)acting_seat;
        }
    }
    ++next.turn_id;
    next.turn_seat = next.finished ? MYGAMES_DDZ_NO_SEAT :
                                      (uint8_t)((acting_seat + 1u) % MYGAMES_DDZ_SEATS);
    event.next_seat = next.turn_seat;
    event.next_turn_id = next.turn_id;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        event.remaining[i] = next.remaining[i];
    if (mygames_ddz_round_validate(&next) != MYGAMES_DDZ_OK)
        return MYGAMES_DDZ_INVALID_STATE;
    *round = next;
    *out_result = event;
    return MYGAMES_DDZ_OK;
}
