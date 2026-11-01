#ifndef MYGAMES_DOUZERO_RULE_PROFILE_H
#define MYGAMES_DOUZERO_RULE_PROFILE_H

#include "mygames/ddz_round.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Offline-only classifier contract matching kwai/DouZero's pinned
 * get_move_type() for candidate actions. This calls the EXISTING authoritative
 * MyGames C11 classifier first and handles only explicitly verified upstream
 * divergences. It is NOT a second Round/Match engine and must never be used
 * as an online Room Owner legality/commit bypass. No hidden hand access.
 *
 * The original MyGames::DouDizhuRound classifier and its house rules remain
 * unchanged. The caller must separately prove complete actions, comparison,
 * turn/pass, bidding, feature and rewards semantics before a C11 Env adapter
 * may use this profile for native self-play. Error is atomic on out_move. */
mygames_ddz_status mygames_douzero_classify_move(
    uint64_t physical_cards, mygames_ddz_move *out_move);

#ifdef __cplusplus
}
#endif
#endif
