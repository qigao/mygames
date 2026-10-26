#include "mygames/ddz_game_owner.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_store(mygames_store_status actual, mygames_store_status wanted) {
    if (actual != wanted) {
        fprintf(stderr, "Game Owner disk status %d wanted %d\n",
                (int)actual, (int)wanted);
        abort();
    }
}
static void require_game(mygames_ddz_owner_status actual,
                         mygames_ddz_owner_status wanted) {
    if (actual != wanted) {
        fprintf(stderr, "Game Owner status %d wanted %d\n",
                (int)actual, (int)wanted);
        abort();
    }
}
static void cleanup_db(const char *path) {
    char sidecar[1024];
    int n;
    (void)remove(path);
    n = snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    if (n > 0 && (size_t)n < sizeof(sidecar)) (void)remove(sidecar);
    n = snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    if (n > 0 && (size_t)n < sizeof(sidecar)) (void)remove(sidecar);
}
static void create_started_room(mygames_room_directory *rooms) {
    const mygames_room *room = NULL;
    unsigned int seat = 99u;
    mygames_room_directory_init(rooms);
    assert(mygames_room_directory_create(rooms, 101u, &room) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(rooms, "room-1", 202u, &seat) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(rooms, "room-1", 303u, &seat) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms, 101u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms, 202u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms, 303u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_start(&rooms->entries[0].room, 101u) == MYGAMES_ROOM_OK);
}

static void assert_public_ack(const void *payload, size_t bytes, uint64_t seq,
                              const char *operation) {
    char decoded[MYGAMES_DDZ_GAME_ACK_MAX + 1u];
    char expected_sequence[40], expected_action[60];
    int n;
    assert(bytes > 0u && bytes <= MYGAMES_DDZ_GAME_ACK_MAX);
    memcpy(decoded, payload, bytes);
    decoded[bytes] = '\0';
    n = snprintf(expected_sequence, sizeof(expected_sequence),
                 "\"seq\":%" PRIu64, seq);
    assert(n > 0 && (size_t)n < sizeof(expected_sequence));
    n = snprintf(expected_action, sizeof(expected_action),
                 "\"action\":\"%s\"", operation);
    assert(n > 0 && (size_t)n < sizeof(expected_action));
    assert(strstr(decoded, "\"type\":\"game.ack\""));
    assert(strstr(decoded, expected_sequence));
    assert(strstr(decoded, expected_action));
    /* None of the encrypted-at-rest/private game state is emitted. */
    assert(!strstr(decoded, "secret") && !strstr(decoded, "kitty") &&
           !strstr(decoded, "hands") && !strstr(decoded, "deck"));
}

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory rooms, persisted;
    mygames_ddz_game_owner owner = {0};
    mygames_ddz_match_public view = {0};
    char ack[MYGAMES_DDZ_GAME_ACK_MAX];
    char initial_ack[MYGAMES_DDZ_GAME_ACK_MAX];
    char bid_ack[MYGAMES_DDZ_GAME_ACK_MAX];
    char play_ack[MYGAMES_DDZ_GAME_ACK_MAX];
    char tiny[8] = {'X','X','X','X','X','X','X','X'};
    size_t bytes = 0u, initial_bytes = 0u, bid_bytes = 0u, play_bytes = 0u;
    uint64_t seq = 0u, stored_epoch = 0u, stored_seq = 0u;
    uint64_t hand = UINT64_C(0), other = UINT64_C(0), first = 0u, claimed = 0u;
    uint8_t initial_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    static const char room_ack[] = "{\"v\":1,\"type\":\"room.ack\"}";
    static const char room_event[] = "{\"v\":1,\"type\":\"room.command_accepted\"}";
    if (argc != 3) {
        fprintf(stderr, "usage: game_owner_test /absolute/driver /path/game.db\n");
        return 2;
    }
    cleanup_db(argv[2]);
    memset(initial_digest, 0x43, sizeof(initial_digest));
    create_started_room(&rooms);
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    require_store(mygames_room_store_commit(store, 101u, &rooms, 1u, 0u,
        "room-setup", initial_digest, room_ack, sizeof(room_ack) - 1u,
        room_event, sizeof(room_event) - 1u, &seq), MYGAMES_STORE_OK);
    assert(seq == 1u);
    require_game(mygames_ddz_game_owner_init(&owner, store, &rooms, 1u, &seq, "room-1"),
                 MYGAMES_DDZ_OWNER_OK);
    require_game(mygames_ddz_game_owner_private_hand(&owner, 101u, &hand),
                 MYGAMES_DDZ_OWNER_WRONG_PHASE);
    require_game(mygames_ddz_game_owner_start(&owner, 202u, 99u, "game-start",
        ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_UNAUTHORIZED);
    require_game(mygames_ddz_game_owner_start(&owner, 101u, 99u, "game-start",
        tiny, sizeof(tiny), &bytes), MYGAMES_DDZ_OWNER_CAPACITY);
    assert(seq == 1u && bytes == 0u);
    require_game(mygames_ddz_game_owner_start(&owner, 101u, 99u, "game-start",
        ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_OK);
    assert(seq == 2u);
    assert_public_ack(ack, bytes, 2u, "start");
    initial_bytes = bytes;
    memcpy(initial_ack, ack, bytes);
    require_game(mygames_ddz_game_owner_private_hand(&owner, 101u, &hand),
                 MYGAMES_DDZ_OWNER_OK);
    require_game(mygames_ddz_game_owner_private_hand(&owner, 202u, &other),
                 MYGAMES_DDZ_OWNER_OK);
    assert(hand != other && (hand & other) == 0u);
    require_game(mygames_ddz_game_owner_private_hand(&owner, 404u, &hand),
                 MYGAMES_DDZ_OWNER_UNAUTHORIZED);
    require_game(mygames_ddz_game_owner_public_view(&owner, &view),
                 MYGAMES_DDZ_OWNER_OK);
    assert(view.phase == MYGAMES_DDZ_MATCH_BIDDING &&
           view.revealed_kitty == 0u && !view.has_round);

    bytes = 0u;
    require_game(mygames_ddz_game_owner_start(&owner, 101u, 99u, "game-start",
        ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(seq == 2u && bytes == initial_bytes &&
           memcmp(ack, initial_ack, bytes) == 0);
    /* game_id is minted by CFlow, not a client semantic parameter:
     * repeat of this same request_id must replay the original durable
     * Game ACK even if a caller proposes the next available minted id. */
    require_game(mygames_ddz_game_owner_start(&owner, 101u, 100u, "game-start",
        ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(seq == 2u && bytes == initial_bytes &&
           memcmp(ack, initial_ack, bytes) == 0);

    /* Emulate a failed true TurboDB transaction; no owner/Game ACK is
     * emitted, and a fresh ORM connection must recover the prior epoch. */
    mygames_room_store_fail_after_checkpoint_once(store);
    bytes = 0u;
    require_game(mygames_ddz_game_owner_bid(&owner, 101u, 99u, 1u, 3u,
        "bid-three", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_STORAGE_HALTED);
    assert(seq == 2u && bytes == 0u && owner.halted);
    require_game(mygames_ddz_game_owner_bid(&owner, 101u, 99u, 1u, 3u,
        "bid-three", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_STORAGE_HALTED);
    mygames_room_store_close(store);
    store = NULL;
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    mygames_ddz_game_owner_reset(&owner);
    require_game(mygames_ddz_game_owner_init(&owner, store, &rooms, 1u, &seq, "room-1"),
                 MYGAMES_DDZ_OWNER_OK);
    require_game(mygames_ddz_game_owner_public_view(&owner, &view),
                 MYGAMES_DDZ_OWNER_OK);
    assert(!view.has_round && view.phase == MYGAMES_DDZ_MATCH_BIDDING);
    require_game(mygames_ddz_game_owner_bid(&owner, 101u, 99u, 1u, 3u,
        "bid-three", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_OK);
    assert(seq == 3u);
    assert_public_ack(ack, bytes, 3u, "bid");
    bid_bytes = bytes;
    memcpy(bid_ack, ack, bytes);
    require_game(mygames_ddz_game_owner_public_view(&owner, &view),
                 MYGAMES_DDZ_OWNER_OK);
    assert(view.phase == MYGAMES_DDZ_MATCH_PLAYING &&
           view.has_round && view.revealed_kitty != 0u);
    require_game(mygames_ddz_game_owner_bid(&owner, 101u, 99u, 1u, 3u,
        "bid-three", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(bytes == bid_bytes && memcmp(ack, bid_ack, bytes) == 0 && seq == 3u);
    require_game(mygames_ddz_game_owner_play(&owner, 202u, 99u, 1u,
        UINT64_C(1) << 0u, "wrong-seat", ack, sizeof(ack), &bytes),
        MYGAMES_DDZ_OWNER_STALE);
    require_game(mygames_ddz_game_owner_private_hand(&owner, 101u, &hand),
                 MYGAMES_DDZ_OWNER_OK);
    first = hand & (~hand + UINT64_C(1));
    require_game(mygames_ddz_game_owner_play(&owner, 101u, 99u, 1u,
        first, "play-first", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_OK);
    assert(seq == 4u);
    assert_public_ack(ack, bytes, 4u, "play");
    play_bytes = bytes;
    memcpy(play_ack, ack, bytes);
    require_game(mygames_ddz_game_owner_public_view(&owner, &view),
                 MYGAMES_DDZ_OWNER_OK);
    assert(view.round.last_cards == first && view.round.turn_id == 2u);
    require_game(mygames_ddz_game_owner_play(&owner, 101u, 99u, 1u,
        first, "play-first", ack, sizeof(ack), &bytes),
        MYGAMES_DDZ_OWNER_REPLAY);
    assert(seq == 4u && bytes == play_bytes &&
           memcmp(ack, play_ack, bytes) == 0);
    mygames_room_store_close(store);
    store = NULL;

    /* The next actual process/connection loads the original committed
     * outcome before re-applying any stale model/network request. */
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    require_store(mygames_room_store_load(store, &persisted, &stored_epoch, &stored_seq),
                  MYGAMES_STORE_OK);
    assert(stored_epoch == 1u && stored_seq == seq);
    mygames_ddz_game_owner_reset(&owner);
    require_game(mygames_ddz_game_owner_init(&owner, store, &persisted,
                 stored_epoch, &seq, "room-1"), MYGAMES_DDZ_OWNER_OK);
    require_game(mygames_ddz_game_owner_play(&owner, 101u, 99u, 1u,
        first, "play-first", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(seq == 4u && bytes == play_bytes &&
           memcmp(ack, play_ack, bytes) == 0);
    require_store(mygames_room_store_claim_epoch(store, 1u, &claimed),
                  MYGAMES_STORE_OK);
    assert(claimed == 2u);
    require_store(mygames_room_store_load(store, &persisted, &stored_epoch, &stored_seq),
                  MYGAMES_STORE_OK);
    assert(stored_epoch == 2u && stored_seq == 4u);
    mygames_ddz_game_owner_reset(&owner);
    require_game(mygames_ddz_game_owner_init(&owner, store, &persisted,
                 stored_epoch, &seq, "room-1"), MYGAMES_DDZ_OWNER_OK);
    /* The same semantic receipt survives a REAL durable Owner epoch claim.
     * A retransmitted old game command must return its original ACK,
     * never a new turn or another Outbox event. */
    bytes = 0u;
    require_game(mygames_ddz_game_owner_play(&owner, 101u, 99u, 1u,
        first, "play-first", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(bytes == play_bytes && memcmp(ack, play_ack, bytes) == 0 && seq == 4u);
    bytes = 0u;
    require_game(mygames_ddz_game_owner_start(&owner, 101u, 100u,
        "game-start", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_REPLAY);
    assert(bytes == initial_bytes &&
           memcmp(ack, initial_ack, bytes) == 0 && seq == 4u);
    require_game(mygames_ddz_game_owner_play(&owner, 202u, 99u, 2u,
        0u, "pass-second", ack, sizeof(ack), &bytes), MYGAMES_DDZ_OWNER_OK);
    assert(seq == 5u);
    assert_public_ack(ack, bytes, 5u, "pass");
    require_game(mygames_ddz_game_owner_public_view(&owner, &view),
                 MYGAMES_DDZ_OWNER_OK);
    assert(view.round.turn_id == 3u &&
           view.round.turn_seat == 2u);
    mygames_ddz_game_owner_reset(&owner);
    mygames_room_store_close(store);
    puts("mygames.game_owner: PASS");
    return 0;
}
