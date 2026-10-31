#ifndef MYGAMES_DDZ_DMC_EPISODE_H
#define MYGAMES_DDZ_DMC_EPISODE_H

#include "mygames/ddz_ai_features.h"
#include "mygames/ddz_match.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No completed DDZ v1 match needs more than 54 card-consuming actions
 * and two intervening PASS actions per consumed hand. Bidding is separate. */
#define MYGAMES_DDZ_DMC_MAX_STEPS 162u
#define MYGAMES_DDZ_DMC_DATA_VERSION 1u

typedef enum mygames_ddz_dmc_objective {
    MYGAMES_DDZ_DMC_WIN_SIGN = 1, /* +1 winning team / -1 losing team */
    MYGAMES_DDZ_DMC_GAME_POINTS = 2 /* actual nonmonetary M5g delta */
} mygames_ddz_dmc_objective;

typedef enum mygames_ddz_dmc_status {
    MYGAMES_DDZ_DMC_OK = 0,
    MYGAMES_DDZ_DMC_INVALID,
    MYGAMES_DDZ_DMC_CAPACITY,
    MYGAMES_DDZ_DMC_NOT_TERMINAL,
    MYGAMES_DDZ_DMC_SEALED
} mygames_ddz_dmc_status;

/* Owned, fixed-shape training record: NO full game/match, no opponent hand,
 * no secret deck, no credentials. This is NOT a durable serialized format.
 * Label becomes valid ONLY after seal with an authoritative settlement. */
typedef struct mygames_ddz_dmc_sample {
    uint64_t episode_id;
    uint64_t source_turn_id;
    uint64_t model_version;
    uint64_t chosen_cards;
    uint32_t rules_version;
    uint8_t actor_seat;
    mygames_ddz_ai_role role;
    mygames_ddz_ai_features features;
    int8_t action_features[MYGAMES_DDZ_AI_CARD_FEATURES];
    double target;
} mygames_ddz_dmc_sample;

typedef struct mygames_ddz_dmc_episode {
    mygames_ddz_dmc_sample *samples; /* caller-owned, private */
    size_t capacity;
    size_t count;
    uint64_t episode_id;
    uint64_t model_version;
    uint64_t next_turn_id;
    uint32_t rules_version;
    uint8_t landlord_seat;
    uint8_t terminal_winner;
    bool terminated;
    bool sealed;
    mygames_ddz_dmc_objective objective;
} mygames_ddz_dmc_episode;

mygames_ddz_dmc_status mygames_ddz_dmc_episode_init(
    mygames_ddz_dmc_episode *out,
    mygames_ddz_dmc_sample *owned_samples,
    size_t capacity, uint64_t episode_id,
    uint64_t model_version, uint32_t rules_version,
    uint8_t landlord_seat);

/* Called ONLY AFTER the existing authoritative round has accepted a move,
 * never on a speculative or uncommitted online command. */
mygames_ddz_dmc_status mygames_ddz_dmc_episode_append(
    mygames_ddz_dmc_episode *episode,
    const mygames_ddz_ai_observation *before,
    const mygames_ddz_action_result *accepted,
    uint64_t chosen_card_mask);

/* Only a real confirmed FINISHED result can label a complete episode.
 * Training must never use label from max-step truncation or partial match. */
mygames_ddz_dmc_status mygames_ddz_dmc_episode_seal(
    mygames_ddz_dmc_episode *episode,
    const mygames_ddz_settlement *authoritative_settlement,
    mygames_ddz_dmc_objective objective);

#ifdef __cplusplus
}
#endif
#endif
