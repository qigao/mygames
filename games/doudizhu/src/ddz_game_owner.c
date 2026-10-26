#include "mygames/ddz_game_owner.h"

#include <cmeta_crypto.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool request_valid(const char *request_id) {
    size_t i;
    if (!request_id) return false;
    for (i = 0u; i <= MYGAMES_STORE_REQUEST_ID_MAX; ++i) {
        const unsigned char c = (unsigned char)request_id[i];
        if (!c) return i > 0u;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ':')) return false;
    }
    return false;
}

static bool owner_available(const mygames_ddz_game_owner *owner) {
    return owner && !owner->halted && owner->store && owner->rooms &&
           owner->sequence && owner->epoch != 0u &&
           owner->room_id[0] != '\0';
}

/* Canonical normalized action SHA256: request ID is the per-player journal
 * key and deliberately NOT part of the semantic digest; identical retry
 * bytes can be resolved after game progression or Owner restart.
 * This never hashes or logs private hands/deck/kitty. */
static bool digest_command(
    const char *operation, uint64_t epoch, uint64_t game_id,
    uint64_t turn, mygames_player_id player, uint64_t value,
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES]) {
    char normalized[218];
    int n;
    /* game.start has a SERVER-minted game_id. Excluding it from the
     * normalized start digest makes a retry after confirmed COMMIT replay
     * the ORIGINAL Game ACK, rather than conflict against a newly minted
     * id based on the now-incremented global sequence. */
    if (!operation || !digest || !player || !epoch || !turn ||
        (!game_id && strcmp(operation, "start") != 0))
        return false;
    n = snprintf(normalized, sizeof(normalized),
        "mygames.ddz.v1|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64
        "|%" PRIu64 "|%" PRIu64,
        operation, epoch, game_id, turn, player, value);
    return n > 0 && (size_t)n < sizeof(normalized) &&
           cmeta_sha256(normalized, (size_t)n, digest) == SALTS_OK;
}

static mygames_ddz_owner_status lookup_replay(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    const char *request_id,
    const uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    void *out_ack, size_t capacity, size_t *out_bytes) {
    unsigned char prior[MYGAMES_STORE_RESULT_MAX];
    size_t size = 0u;
    uint64_t committed_seq = 0u;
    const mygames_store_status status =
        mygames_room_store_get_outcome(owner->store, player,
                request_id, digest, prior, sizeof(prior), &size,
                &committed_seq);
    if (status == MYGAMES_STORE_NOT_FOUND) return MYGAMES_DDZ_OWNER_OK;
    if (status == MYGAMES_STORE_REQUEST_CONFLICT)
        return MYGAMES_DDZ_OWNER_STALE;
    if (status != MYGAMES_STORE_OK || !size || !committed_seq) {
        owner->halted = true;
        return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    }
    if (capacity < size) return MYGAMES_DDZ_OWNER_CAPACITY;
    memcpy(out_ack, prior, size);
    *out_bytes = size;
    return MYGAMES_DDZ_OWNER_REPLAY;
}

static bool player_seated(const mygames_ddz_game_owner *owner,
                          mygames_player_id player) {
    const mygames_room *room;
    unsigned int i;
    if (!owner || !player) return false;
    room = mygames_room_directory_get(owner->rooms, owner->room_id);
    if (!room || room->phase != MYGAMES_ROOM_STARTING) return false;
    for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i)
        if (room->seats[i].player == player) return true;
    return false;
}

static mygames_ddz_owner_status preflight(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    uint64_t game_id, uint64_t turn, uint64_t value,
    const char *operation, const char *request_id,
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    void *out_ack, size_t capacity, size_t *out_bytes) {
    if (!owner || !out_ack || !out_bytes || !capacity ||
        !request_valid(request_id) || !game_id || !turn)
        return MYGAMES_DDZ_OWNER_INVALID;
    if (!owner_available(owner)) return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    if (!player_seated(owner, player)) return MYGAMES_DDZ_OWNER_UNAUTHORIZED;
    if (!digest_command(operation, owner->epoch,
                        strcmp(operation, "start") == 0 ? UINT64_C(0) : game_id,
                        turn, player, value, digest))
        return MYGAMES_DDZ_OWNER_INVALID;
    return lookup_replay(owner, player, request_id,
                         digest, out_ack, capacity, out_bytes);
}

static mygames_ddz_owner_status commit_candidate(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    const char *operation, const char *request_id,
    const uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const mygames_ddz_match *candidate,
    void *out_ack, size_t capacity, size_t *out_bytes) {
    char ack[MYGAMES_DDZ_GAME_ACK_MAX];
    size_t ack_size;
    uint64_t sequence = 0u;
    uint64_t public_turn;
    int n;
    mygames_store_status status;
    if (!owner_available(owner) || !candidate ||
        *owner->sequence >= (uint64_t)INT64_MAX)
        return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    public_turn = (candidate->phase == MYGAMES_DDZ_MATCH_BIDDING ||
                   candidate->phase == MYGAMES_DDZ_MATCH_REDEAL_REQUIRED) ?
                   candidate->bid_turn_id : candidate->round.turn_id;
    n = snprintf(ack, sizeof(ack),
        "{\"v\":1,\"type\":\"game.ack\",\"action\":\"%s\","
        "\"request_id\":\"%s\",\"room_id\":\"%s\","
        "\"room_epoch\":%" PRIu64 ",\"game_id\":%" PRIu64 ","
        "\"phase\":%u,\"turn_id\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        operation, request_id, owner->room_id, owner->epoch,
        candidate->game_id, (unsigned int)candidate->phase,
        public_turn, *owner->sequence + 1u);
    if (n <= 0 || (size_t)n >= sizeof(ack))
        return MYGAMES_DDZ_OWNER_INVALID;
    ack_size = (size_t)n;
    if (capacity < ack_size) return MYGAMES_DDZ_OWNER_CAPACITY;
    status = mygames_room_store_commit_game(
        owner->store, player, owner->rooms, owner->room_id,
        owner->has_match ? &owner->private_match : NULL,
        candidate, owner->epoch, *owner->sequence,
        request_id, digest, ack, ack_size, &sequence);
    if (status == MYGAMES_STORE_REPLAY) {
        return lookup_replay(owner, player, request_id,
                             digest, out_ack, capacity, out_bytes);
    }
    if (status == MYGAMES_STORE_REQUEST_CONFLICT ||
        status == MYGAMES_STORE_STALE_GAME ||
        status == MYGAMES_STORE_STALE_SEQUENCE ||
        status == MYGAMES_STORE_STALE_EPOCH)
        return MYGAMES_DDZ_OWNER_STALE;
    if (status != MYGAMES_STORE_OK ||
        sequence != *owner->sequence + 1u) {
        /* Even a dispatched unknown COMMIT may have landed. Refuse all
         * subsequent mutations until a fresh trusted physical DB reopen. */
        owner->halted = true;
        return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    }
    owner->private_match = *candidate;
    owner->has_match = true;
    *owner->sequence = sequence;
    memcpy(out_ack, ack, ack_size);
    *out_bytes = ack_size;
    return MYGAMES_DDZ_OWNER_OK;
}

mygames_ddz_owner_status mygames_ddz_game_owner_init(
    mygames_ddz_game_owner *owner, mygames_room_store *store,
    const mygames_room_directory *rooms, uint64_t epoch,
    uint64_t *sequence, const char *room_id) {
    mygames_ddz_game_owner state = {0};
    mygames_room_directory durable_rooms;
    mygames_ddz_match restored = {0};
    unsigned char durable_image[MYGAMES_CHECKPOINT_MAX_BYTES];
    unsigned char current_image[MYGAMES_CHECKPOINT_MAX_BYTES];
    size_t durable_bytes = 0u, current_bytes = 0u;
    uint64_t persisted_epoch = 0u, persisted_seq = 0u, game_seq = 0u;
    mygames_store_status status;
    size_t length;
    if (!owner || !store || !rooms || !sequence || !room_id ||
        !epoch) return MYGAMES_DDZ_OWNER_INVALID;
    for (length = 0u; length < MYGAMES_ROOM_ID_CAPACITY; ++length) {
        if (!room_id[length]) break;
        if (!((room_id[length] >= 'a' && room_id[length] <= 'z') ||
              (room_id[length] >= 'A' && room_id[length] <= 'Z') ||
              (room_id[length] >= '0' && room_id[length] <= '9') ||
              room_id[length] == '-' || room_id[length] == '_'))
            return MYGAMES_DDZ_OWNER_INVALID;
    }
    if (!length || length >= MYGAMES_ROOM_ID_CAPACITY)
        return MYGAMES_DDZ_OWNER_INVALID;
    status = mygames_room_store_load(store, &durable_rooms,
                                    &persisted_epoch, &persisted_seq);
    if (status != MYGAMES_STORE_OK)
        return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    if (persisted_epoch != epoch || persisted_seq != *sequence)
        return MYGAMES_DDZ_OWNER_STALE;
    if (mygames_room_checkpoint_encode(&durable_rooms, epoch, durable_image,
            sizeof(durable_image), &durable_bytes) != MYGAMES_CHECKPOINT_OK ||
        mygames_room_checkpoint_encode(rooms, epoch, current_image,
            sizeof(current_image), &current_bytes) != MYGAMES_CHECKPOINT_OK ||
        durable_bytes != current_bytes ||
        memcmp(durable_image, current_image, current_bytes) != 0)
        return MYGAMES_DDZ_OWNER_STALE;
    state.store = store;
    state.rooms = rooms;
    state.sequence = sequence;
    state.epoch = epoch;
    memcpy(state.room_id, room_id, length);
    state.room_id[length] = '\0';
    if (!mygames_room_directory_get(rooms, state.room_id))
        return MYGAMES_DDZ_OWNER_INVALID;
    status = mygames_room_store_load_game(store, room_id, &restored, &game_seq);
    if (status == MYGAMES_STORE_OK) {
        if (restored.room_epoch != epoch || game_seq > *sequence)
            return MYGAMES_DDZ_OWNER_STALE;
        state.private_match = restored;
        state.has_match = true;
    } else if (status != MYGAMES_STORE_NOT_FOUND) {
        return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    }
    *owner = state;
    return MYGAMES_DDZ_OWNER_OK;
}

mygames_ddz_owner_status mygames_ddz_game_owner_start(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    uint64_t game_id, const char *request_id,
    void *out_ack, size_t capacity, size_t *out_bytes) {
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    mygames_ddz_match candidate = {0};
    const mygames_room *room;
    mygames_ddz_owner_status status = preflight(
        owner, player, game_id, 1u, 0u, "start", request_id,
        digest, out_ack, capacity, out_bytes);
    if (status != MYGAMES_DDZ_OWNER_OK) return status;
    room = mygames_room_directory_get(owner->rooms, owner->room_id);
    if (!room || room->owner != player)
        return MYGAMES_DDZ_OWNER_UNAUTHORIZED;
    if (owner->has_match &&
        owner->private_match.phase != MYGAMES_DDZ_MATCH_FINISHED &&
        owner->private_match.phase != MYGAMES_DDZ_MATCH_REDEAL_REQUIRED)
        return MYGAMES_DDZ_OWNER_WRONG_PHASE;
    if (owner->has_match && game_id <= owner->private_match.game_id)
        return MYGAMES_DDZ_OWNER_STALE;
    if (mygames_ddz_match_start(&candidate, room,
                               owner->epoch, game_id) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_OWNER_INVALID;
    return commit_candidate(owner, player, "start", request_id,
                            digest, &candidate, out_ack, capacity, out_bytes);
}

mygames_ddz_owner_status mygames_ddz_game_owner_bid(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    uint64_t game_id, uint64_t expected_turn, uint8_t score,
    const char *request_id,
    void *out_ack, size_t capacity, size_t *out_bytes) {
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    mygames_ddz_match candidate = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_owner_status status = preflight(
        owner, player, game_id, expected_turn, score,
        "bid", request_id, digest, out_ack, capacity, out_bytes);
    if (status != MYGAMES_DDZ_OWNER_OK) return status;
    if (!owner->has_match) return MYGAMES_DDZ_OWNER_WRONG_PHASE;
    if (game_id != owner->private_match.game_id)
        return MYGAMES_DDZ_OWNER_STALE;
    if (mygames_ddz_match_bid(&owner->private_match, owner->epoch,
            game_id, player, expected_turn, score,
            &candidate, &bid) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_OWNER_STALE;
    return commit_candidate(owner, player, "bid", request_id,
                            digest, &candidate, out_ack, capacity, out_bytes);
}

mygames_ddz_owner_status mygames_ddz_game_owner_play(
    mygames_ddz_game_owner *owner, mygames_player_id player,
    uint64_t game_id, uint64_t expected_turn, uint64_t cards,
    const char *request_id,
    void *out_ack, size_t capacity, size_t *out_bytes) {
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    mygames_ddz_match candidate = {0};
    mygames_ddz_action_result move = {0};
    const char *operation = cards == 0u ? "pass" : "play";
    mygames_ddz_owner_status status = preflight(
        owner, player, game_id, expected_turn, cards,
        operation, request_id, digest, out_ack, capacity, out_bytes);
    if (status != MYGAMES_DDZ_OWNER_OK) return status;
    if (!owner->has_match) return MYGAMES_DDZ_OWNER_WRONG_PHASE;
    if (game_id != owner->private_match.game_id)
        return MYGAMES_DDZ_OWNER_STALE;
    if (mygames_ddz_match_play(&owner->private_match, owner->epoch,
            game_id, player, expected_turn, cards,
            &candidate, &move) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_OWNER_STALE;
    return commit_candidate(owner, player, operation, request_id,
                            digest, &candidate, out_ack, capacity, out_bytes);
}

mygames_ddz_owner_status mygames_ddz_game_owner_public_view(
    const mygames_ddz_game_owner *owner, mygames_ddz_match_public *out_view) {
    if (!owner || !out_view) return MYGAMES_DDZ_OWNER_INVALID;
    if (!owner_available(owner)) return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    if (!owner->has_match) return MYGAMES_DDZ_OWNER_WRONG_PHASE;
    return mygames_ddz_match_public_view(&owner->private_match, out_view) ==
           MYGAMES_DDZ_MATCH_OK ? MYGAMES_DDZ_OWNER_OK :
                                 MYGAMES_DDZ_OWNER_STALE;
}

mygames_ddz_owner_status mygames_ddz_game_owner_private_hand(
    const mygames_ddz_game_owner *owner,
    mygames_player_id player, uint64_t *out_hand) {
    if (!owner || !out_hand) return MYGAMES_DDZ_OWNER_INVALID;
    if (!owner_available(owner)) return MYGAMES_DDZ_OWNER_STORAGE_HALTED;
    if (!player_seated(owner, player)) return MYGAMES_DDZ_OWNER_UNAUTHORIZED;
    if (!owner->has_match) return MYGAMES_DDZ_OWNER_WRONG_PHASE;
    return mygames_ddz_match_private_hand(&owner->private_match,
                owner->epoch, owner->private_match.game_id,
                player, out_hand) == MYGAMES_DDZ_MATCH_OK ?
           MYGAMES_DDZ_OWNER_OK : MYGAMES_DDZ_OWNER_STALE;
}

void mygames_ddz_game_owner_reset(mygames_ddz_game_owner *owner) {
    if (owner) memset(owner, 0, sizeof(*owner));
}
