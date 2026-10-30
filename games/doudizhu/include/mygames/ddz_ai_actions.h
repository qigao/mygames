#ifndef MYGAMES_DDZ_AI_ACTIONS_H
#define MYGAMES_DDZ_AI_ACTIONS_H

#include "mygames/ddz_round.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mygames_ddz_ai_status {
    MYGAMES_DDZ_AI_OK = 0,
    MYGAMES_DDZ_AI_INVALID_ARGUMENT,
    MYGAMES_DDZ_AI_INVALID_OBSERVATION,
    MYGAMES_DDZ_AI_CAPACITY
} mygames_ddz_ai_status;

/* Enumerate all distinct rank-combination actions for the ACTING player,
 * represented by one canonical subset of their owned physical-card IDs.
 * Suits are irrelevant under MyGames DDZ v1 rules; the lowest owned IDs of
 * each rank are chosen deterministically. No private match/other hands enter.
 *
 * Pass (mask 0) is included iff a live preceding play can be passed on.
 * All moves are reclassified using the authoritative v1 classifier and
 * compared to the actual public trick. The output order is deterministic.
 *
 * Query mode: out_masks=NULL, capacity=0, out_count receives required count.
 * Capacity failure: out_masks unchanged, out_count receives required count.
 * All other errors leave both output and out_count unchanged.
 * The caller owns the observation, output and all memory. */
mygames_ddz_ai_status mygames_ddz_ai_legal_actions(
    uint64_t own_hand,
    const mygames_ddz_public_view *public_view,
    uint64_t *out_masks,
    size_t capacity,
    size_t *out_count);

#ifdef __cplusplus
}
#endif
#endif
