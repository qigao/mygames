#ifndef MYGAMES_DOUDIZHU_ROUND_H
#define MYGAMES_DOUDIZHU_ROUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rank order: 3,4,...,K,A,2,small joker,big joker. IDs 0..51 map to
 * rank=id/4 with four physical suits. 52 and 53 are the two unique jokers. */
#define MYGAMES_DDZ_CARDS 54u
#define MYGAMES_DDZ_SEATS 3u
#define MYGAMES_DDZ_NO_SEAT 3u
#define MYGAMES_DDZ_CARD_MASK ((UINT64_C(1) << MYGAMES_DDZ_CARDS) - UINT64_C(1))

typedef enum mygames_ddz_status {
    MYGAMES_DDZ_OK = 0,
    MYGAMES_DDZ_INVALID_ARGUMENT,
    MYGAMES_DDZ_INVALID_DECK,
    MYGAMES_DDZ_INVALID_STATE,
    MYGAMES_DDZ_INVALID_MOVE,
    MYGAMES_DDZ_NOT_YOUR_TURN,
    MYGAMES_DDZ_STALE_TURN,
    MYGAMES_DDZ_CARDS_NOT_OWNED,
    MYGAMES_DDZ_CANNOT_PASS,
    MYGAMES_DDZ_NOT_HIGHER,
    MYGAMES_DDZ_FINISHED,
    MYGAMES_DDZ_TURN_OVERFLOW
} mygames_ddz_status;

typedef enum mygames_ddz_move_kind {
    MYGAMES_DDZ_MOVE_NONE = 0,
    MYGAMES_DDZ_MOVE_SINGLE,
    MYGAMES_DDZ_MOVE_PAIR,
    MYGAMES_DDZ_MOVE_TRIPLE,
    MYGAMES_DDZ_MOVE_TRIPLE_SINGLE,
    MYGAMES_DDZ_MOVE_TRIPLE_PAIR,
    MYGAMES_DDZ_MOVE_STRAIGHT,
    MYGAMES_DDZ_MOVE_PAIR_CHAIN,
    MYGAMES_DDZ_MOVE_TRIPLE_CHAIN,
    MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES,
    MYGAMES_DDZ_MOVE_AIRPLANE_PAIRS,
    MYGAMES_DDZ_MOVE_FOUR_TWO_SINGLES,
    MYGAMES_DDZ_MOVE_FOUR_TWO_PAIRS,
    MYGAMES_DDZ_MOVE_BOMB,
    MYGAMES_DDZ_MOVE_ROCKET
} mygames_ddz_move_kind;

typedef struct mygames_ddz_move {
    mygames_ddz_move_kind kind;
    uint8_t high_rank;
    uint8_t chain_length;
    uint8_t card_count;
} mygames_ddz_move;

/* This entire state contains PRIVATE hands. It belongs only to the
 * authoritative game Owner. Never serialize, log or publish it to players,
 * Flowie, Cocos or an unprivileged projection. Game persistence will need
 * a separate validated, encrypted/member-scoped contract. */
typedef struct mygames_ddz_round {
    uint64_t hands[MYGAMES_DDZ_SEATS];
    uint64_t played_cards;
    uint64_t last_cards;
    uint64_t turn_id;
    mygames_ddz_move last_move;
    uint8_t remaining[MYGAMES_DDZ_SEATS];
    uint8_t landlord_seat;
    uint8_t turn_seat;
    uint8_t last_seat;
    uint8_t passes;
    uint8_t winner_seat;
    bool finished;
} mygames_ddz_round;

/* Only the public move and public remaining-counts belong in this result.
 * Do not infer a durable Game ACK from success: Caller must stage and commit
 * round + command receipt + sanitized event transactionally before replying. */
typedef struct mygames_ddz_action_result {
    uint64_t next_turn_id;
    mygames_ddz_move move;
    uint8_t acting_seat;
    uint8_t next_seat;
    uint8_t winner_seat;
    uint8_t remaining[MYGAMES_DDZ_SEATS];
    bool passed;
    bool trick_cleared;
    bool finished;
} mygames_ddz_action_result;

/* Pure, bounded, allocation-free rules. 'cards' lists physical IDs, not ranks;
 * conversion rejects repeats and never modifies output on failure. */
mygames_ddz_status mygames_ddz_cards_to_mask(const uint8_t *cards, size_t count,
                                              uint64_t *out_mask);
mygames_ddz_status mygames_ddz_classify(uint64_t cards_mask, mygames_ddz_move *out_move);
bool mygames_ddz_beats(const mygames_ddz_move *challenge,
                       const mygames_ddz_move *previous);

/* Deck is a SERVER-generated secret Fisher-Yates permutation supplied by the
 * caller (no untrusted player seed / PRNG is provided here). Deal 17/17/17 in
 * cyclic seat order; add the last three kitty cards to landlord (20 total).
 * Caller has already chosen landlord through a separately validated bidding
 * phase. All invalid inputs leave the destination state unchanged. */
mygames_ddz_status mygames_ddz_round_deal(mygames_ddz_round *round,
                                          const uint8_t deck[MYGAMES_DDZ_CARDS],
                                          unsigned landlord_seat);
mygames_ddz_status mygames_ddz_round_validate(const mygames_ddz_round *round);

/* cards_mask=0 means PASS. expected_turn_id fences stale model/network
 * decisions. On any non-OK result, round AND result remain unchanged. */
mygames_ddz_status mygames_ddz_round_submit(mygames_ddz_round *round,
                                            unsigned acting_seat,
                                            uint64_t expected_turn_id,
                                            uint64_t cards_mask,
                                            mygames_ddz_action_result *out_result);

#ifdef __cplusplus
}
#endif
#endif
