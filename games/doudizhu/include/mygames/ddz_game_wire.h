#ifndef MYGAMES_DDZ_GAME_WIRE_H
#define MYGAMES_DDZ_GAME_WIRE_H

#include "mygames/wire.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mygames_ddz_game_command_kind {
    MYGAMES_DDZ_GAME_START = 1,
    MYGAMES_DDZ_GAME_BID,
    MYGAMES_DDZ_GAME_PLAY,
    MYGAMES_DDZ_GAME_PASS,
    MYGAMES_DDZ_GAME_SNAPSHOT,
    MYGAMES_DDZ_GAME_HAND
} mygames_ddz_game_command_kind;

typedef struct mygames_ddz_game_wire_command {
    mygames_ddz_game_command_kind kind;
    char request_id[MYGAMES_REQUEST_ID_CAPACITY];
    uint64_t game_id;
    uint64_t turn_id;
    uint64_t cards_mask;
    uint8_t bid_score;
} mygames_ddz_game_wire_command;

/* Strict version 1 and EXACT field set for each command, 512 byte cap.
 * Numeric 64-bit game/turn IDs travel as canonical quoted decimals so
 * browsers never lose >2^53 precision. Played physical card IDs use
 * exactly 14 lower-case hex digits (54-bit mask), not JavaScript Number.
 *
 * game.start: request_id only; game_id is SERVER-minted by CFlow.
 * game.bid: request_id, game_id, turn_id, bid (integer 0..3).
 * game.play: request_id, game_id, turn_id, cards_hex (nonempty).
 * game.pass: request_id, game_id, turn_id; mask is zero.
 * game.snapshot.get / game.hand.get: request_id, game_id.
 * Unrecognized/duplicate/malformed/escaped fields fail before affecting
 * the output or any authoritative Owner/database.
 */
mygames_wire_status mygames_ddz_game_wire_decode(
    const void *bytes, size_t size, mygames_ddz_game_wire_command *out);

#ifdef __cplusplus
}
#endif
#endif
