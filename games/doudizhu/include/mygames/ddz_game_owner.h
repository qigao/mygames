#ifndef MYGAMES_DDZ_GAME_OWNER_H
#define MYGAMES_DDZ_GAME_OWNER_H

#include "mygames/room_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_DDZ_GAME_ACK_MAX 256u

typedef enum mygames_ddz_owner_status {
    MYGAMES_DDZ_OWNER_OK = 0,
    MYGAMES_DDZ_OWNER_REPLAY,
    MYGAMES_DDZ_OWNER_INVALID,
    MYGAMES_DDZ_OWNER_UNAUTHORIZED,
    MYGAMES_DDZ_OWNER_STALE,
    MYGAMES_DDZ_OWNER_WRONG_PHASE,
    MYGAMES_DDZ_OWNER_CAPACITY,
    MYGAMES_DDZ_OWNER_STORAGE_HALTED
} mygames_ddz_owner_status;

/* Bounded, SINGLE-CFLOW-ROOM-OWNER ONLY state. Not thread safe.
 * CHttp callbacks must COPY a bounded game command to the CFlow Owner,
 * authenticate its player identity/generation, and invoke these APIs only
 * within its serialized task. No direct WebSocket call or database access
 * from network, Flowie, model or other worker callbacks.
 *
 * Store, directory and sequence are BORROWED and must outlive the Owner.
 * The sequence pointer is the SAME durable global Room/Outbox cursor that
 * ordinary room commands update. Never keep a different local cursor.
 * The private match is never available as a public serialized object. */
typedef struct mygames_ddz_game_owner {
    mygames_room_store *store;
    const mygames_room_directory *rooms;
    uint64_t *sequence;
    uint64_t epoch;
    char room_id[MYGAMES_ROOM_ID_CAPACITY];
    mygames_ddz_match private_match;
    bool has_match;
    bool halted;
} mygames_ddz_game_owner;

/* Verify the canonical RoomDirectory matches persisted checkpoint/epoch,
 * and load the existing private match if any. Only the unique authoritative
 * Room Owner may initialize or drive this structure. */
mygames_ddz_owner_status mygames_ddz_game_owner_init(
    mygames_ddz_game_owner *owner, mygames_room_store *store,
    const mygames_room_directory *authoritative_rooms,
    uint64_t room_epoch, uint64_t *authoritative_sequence,
    const char *room_id);

/* The player ID is already authenticated and bound to this room/seat by
 * an external CHttp/CFlow session authority. game_id is minted by that
 * same trusted Room Owner, never accepted from an unauthenticated client.
 * Error outputs remain unchanged. ACK is returned only on a positive
 * real TurboDB COMMIT or on a matching durable REPLAY. */
mygames_ddz_owner_status mygames_ddz_game_owner_start(
    mygames_ddz_game_owner *owner, mygames_player_id authenticated_player,
    uint64_t game_id, const char *request_id,
    void *out_game_ack, size_t capacity, size_t *out_bytes);
mygames_ddz_owner_status mygames_ddz_game_owner_bid(
    mygames_ddz_game_owner *owner, mygames_player_id authenticated_player,
    uint64_t game_id, uint64_t expected_bid_turn_id, uint8_t score,
    const char *request_id, void *out_game_ack, size_t capacity, size_t *out_bytes);
mygames_ddz_owner_status mygames_ddz_game_owner_play(
    mygames_ddz_game_owner *owner, mygames_player_id authenticated_player,
    uint64_t game_id, uint64_t expected_turn_id, uint64_t cards_mask,
    const char *request_id, void *out_game_ack, size_t capacity, size_t *out_bytes);

/* Public view has only revealed cards/turn/counts. Private member-only hand
 * requires a separately verified session principal before calling. */
mygames_ddz_owner_status mygames_ddz_game_owner_public_view(
    const mygames_ddz_game_owner *owner, mygames_ddz_match_public *out_view);
mygames_ddz_owner_status mygames_ddz_game_owner_private_hand(
    const mygames_ddz_game_owner *owner,
    mygames_player_id authenticated_player, uint64_t *out_hand);

/* Use only after the one Owner executor is quiescent; this borrows no
 * socket, TLS, CHttp or Flowie object and requires no hidden scheduler. */
void mygames_ddz_game_owner_reset(mygames_ddz_game_owner *owner);

#ifdef __cplusplus
}
#endif
#endif
