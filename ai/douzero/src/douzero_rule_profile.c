#include "mygames/douzero_rule_profile.h"

/* Only pinned upstream kwai/DouZero@718a5c92 move_detector additions.
 * No independent general DDZ classifier or board/hand rule runtime. */
mygames_ddz_status mygames_douzero_classify_move(
    uint64_t physical_cards, mygames_ddz_move *out_move) {
    mygames_ddz_move candidate = {0};
    mygames_ddz_status status;
    uint8_t ranks[15] = {0};
    unsigned int i, n = 0u, triples = 0u, quads = 0u;

    if (!out_move) return MYGAMES_DDZ_INVALID_MOVE;
    status = mygames_ddz_classify(physical_cards, &candidate);
    if (status == MYGAMES_DDZ_OK) {
        *out_move = candidate;
        return status;
    }
    if (status != MYGAMES_DDZ_INVALID_MOVE || physical_cards == 0u ||
        (physical_cards & ~MYGAMES_DDZ_CARD_MASK) != 0u)
        return status;

    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) {
        if ((physical_cards & (UINT64_C(1) << i)) != 0u) {
            unsigned int rank = i < 52u ? i / 4u : i - 39u;
            ++ranks[rank];
            ++n;
        }
    }

    /* Original get_move_type(TYPE_14_4_22): two quads are interpreted as
     * four-with-two-pairs, picking the higher quad as main rank. The original
     * ordinary move GENERATOR generally does not emit this edge case, but
     * the detector explicitly accepts it. */
    if (n == 8u) {
        uint8_t high = 0u;
        for (i = 0u; i < 15u; ++i) {
            if (ranks[i] == 4u) { ++quads; high = (uint8_t)i; }
        }
        if (quads == 2u) {
            candidate.kind = MYGAMES_DDZ_MOVE_FOUR_TWO_PAIRS;
            candidate.high_rank = high;
            candidate.card_count = 8u;
            candidate.chain_length = 1u;
            *out_move = candidate;
            return MYGAMES_DDZ_OK;
        }
    }

    /* Original get_move_type(TYPE_11_SERIAL_3_1): 4 distinct triples,
     * exactly 3 consecutive eligible triples can act as the airplane body;
     * the fourth triple is its 3 single wings. This does NOT admit arbitrary
     * 3+ wings, bomb wings, or rank 2 as a part of the triple sequence. */
    if (n == 12u) {
        for (i = 0u; i < 15u; ++i)
            if (ranks[i] == 3u) ++triples;
        if (triples == 4u) {
            unsigned int start;
            for (start = 0u; start + 2u <= 11u; ++start) {
                if (ranks[start] == 3u && ranks[start+1u] == 3u &&
                    ranks[start+2u] == 3u) {
                    candidate.kind = MYGAMES_DDZ_MOVE_AIRPLANE_SINGLES;
                    candidate.high_rank = (uint8_t)(start + 2u);
                    candidate.chain_length = 3u;
                    candidate.card_count = 12u;
                    *out_move = candidate;
                    return MYGAMES_DDZ_OK;
                }
            }
        }
    }
    return MYGAMES_DDZ_INVALID_MOVE;
}
