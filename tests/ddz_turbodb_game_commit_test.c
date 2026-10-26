#include "mygames/room_store.h"
#include <orm.h>
#include <orm_runtime.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_store(mygames_store_status got, mygames_store_status expected) {
    if (got != expected) {
        fprintf(stderr, "MyGames GameStore status %d expected %d\n",
                (int)got, (int)expected);
        abort();
    }
}

static void remove_sqlite(const char *path) {
    char extra[1024];
    int n;
    (void)remove(path);
    n = snprintf(extra, sizeof(extra), "%s-wal", path);
    if (n > 0 && (size_t)n < sizeof(extra)) (void)remove(extra);
    n = snprintf(extra, sizeof(extra), "%s-shm", path);
    if (n > 0 && (size_t)n < sizeof(extra)) (void)remove(extra);
}

static void create_started_room(mygames_room_directory *rooms) {
    const mygames_room *room = NULL;
    mygames_room *authoritative = NULL;
    unsigned int seat = 99u;
    unsigned int i;
    mygames_room_directory_init(rooms);
    assert(mygames_room_directory_create(rooms, 101u, &room) == MYGAMES_ROOM_OK);
    assert(room && strcmp(room->id, "room-1") == 0);
    assert(mygames_room_directory_join(rooms, "room-1", 202u, &seat) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(rooms, "room-1", 303u, &seat) == MYGAMES_ROOM_OK);
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        if (rooms->entries[i].used &&
            strcmp(rooms->entries[i].room.id, "room-1") == 0) {
            authoritative = &rooms->entries[i].room;
            break;
        }
    }
    assert(authoritative);
    assert(mygames_room_set_ready(authoritative, 101u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_set_ready(authoritative, 202u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_set_ready(authoritative, 303u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_start(authoritative, 101u) == MYGAMES_ROOM_OK);
    assert(authoritative->phase == MYGAMES_ROOM_STARTING);
}

static void public_only_event(mygames_room_store *store, uint64_t after,
                              uint64_t expected_seq) {
    char text[MYGAMES_STORE_EVENT_MAX + 1u] = {0};
    size_t size = 0u;
    uint64_t seq = 0u;
    require_store(mygames_room_store_next_event(
        store, after, text, sizeof(text) - 1u, &size, &seq), MYGAMES_STORE_OK);
    assert(seq == expected_seq && size > 0u && size <= MYGAMES_STORE_EVENT_MAX);
    text[size] = '\0';
    assert(strstr(text, "\"type\":\"game.state_committed\"") != NULL);
    assert(strstr(text, "\"room_id\":\"room-1\"") != NULL);
    assert(strstr(text, "secret") == NULL && strstr(text, "hand") == NULL);
    assert(strstr(text, "kitty") == NULL && strstr(text, "deck") == NULL);
}

static void same_match(const mygames_ddz_match *left,
                       const mygames_ddz_match *right) {
    uint8_t a[MYGAMES_DDZ_CHECKPOINT_V1_BYTES];
    uint8_t b[MYGAMES_DDZ_CHECKPOINT_V1_BYTES];
    size_t alen = 0u, blen = 0u;
    assert(mygames_ddz_checkpoint_encode(left, a, sizeof(a), &alen) ==
           MYGAMES_DDZ_CHECKPOINT_OK);
    assert(mygames_ddz_checkpoint_encode(right, b, sizeof(b), &blen) ==
           MYGAMES_DDZ_CHECKPOINT_OK);
    assert(alen == blen && memcmp(a, b, alen) == 0);
}

static void corrupt_private_image_with_real_orm(const char *plugin,
                                                 const char *dbpath) {
    orm_runtime_config_t config;
    orm_driver_load_config_t load = {0};
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_config_t conn;
    orm_option_t option;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    orm_error_t error;
    uint64_t affected = 0u;
    orm_runtime_config_init(&config);
    orm_error_init(&error);
    assert(orm_runtime_create(&config, &runtime, &error) == ORM_STATUS_OK);
    load.struct_size = (uint32_t)sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(plugin);
    load.expected_driver_id = orm_view("sqlite");
    assert(orm_runtime_load_driver(runtime, &load, &error) == ORM_STATUS_OK);
    orm_config(&conn);
    option.keyword = orm_view("filename");
    option.value = orm_view(dbpath);
    conn.driver = orm_view("sqlite");
    conn.options = &option;
    conn.option_count = 1u;
    assert(orm_runtime_connect(runtime, &conn, &connection, &error) == ORM_STATUS_OK);
    assert(orm_raw(connection,
        orm_view("update mg_game_state set private_image=zeroblob(216) "
                 "where room_id='room-1'"), &query, &error) == ORM_STATUS_OK);
    assert(orm_query_execute(query, &result, &error) == ORM_STATUS_OK);
    assert(orm_result_affected_rows(result, &affected, &error) == ORM_STATUS_OK);
    assert(affected == 1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    assert(orm_connection_close(connection, &error) == ORM_STATUS_OK);
    orm_connection_release(connection);
    assert(orm_runtime_close(runtime, &error) == ORM_STATUS_OK);
    orm_runtime_release(runtime);
}

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory rooms, recovered;
    const mygames_room *room;
    mygames_ddz_match initial = {0}, bid_next = {0}, bid_committed = {0};
    mygames_ddz_match played = {0}, recovered_game = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result action = {0};
    unsigned char original_reply[MYGAMES_STORE_RESULT_MAX];
    static const char room_ack[] = "{\"v\":1,\"type\":\"room.ack\"}";
    static const char room_event[] = "{\"v\":1,\"type\":\"room.command_accepted\"}";
    static const char start_ack[] = "{\"v\":1,\"type\":\"game.ack\",\"step\":\"start\"}";
    static const char bid_ack[] = "{\"v\":1,\"type\":\"game.ack\",\"step\":\"bid\"}";
    static const char play_ack[] = "{\"v\":1,\"type\":\"game.ack\",\"step\":\"play\"}";
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES], conflict[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    uint64_t sequence = UINT64_C(777), game_seq = 0u, loaded_epoch = 0u, loaded_seq = 0u;
    uint64_t new_epoch = 0u, hand = 0u, card = 0u;
    size_t reply_bytes = 0u;
    if (argc != 3) {
        fprintf(stderr, "usage: game_store_test /absolute/turbodb_driver /test/sqlite-file\n");
        return 2;
    }
    remove_sqlite(argv[2]);
    memset(digest, 0x35, sizeof(digest));
    memset(conflict, 0xc5, sizeof(conflict));
    create_started_room(&rooms);
    room = mygames_room_directory_get(&rooms, "room-1");
    assert(room && room->phase == MYGAMES_ROOM_STARTING);
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_NOT_FOUND);
    require_store(mygames_room_store_commit(store, 101u, &rooms, 1u, 0u,
            "room-start", digest, room_ack, sizeof(room_ack) - 1u,
            room_event, sizeof(room_event) - 1u, &sequence), MYGAMES_STORE_OK);
    assert(sequence == 1u);
    assert(mygames_ddz_match_start(&initial, room, 1u, 8u) == MYGAMES_DDZ_MATCH_OK);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            NULL, &initial, 1u, 1u, "game-start", digest,
            start_ack, sizeof(start_ack) - 1u, &sequence), MYGAMES_STORE_OK);
    assert(sequence == 2u);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_OK);
    assert(game_seq == 2u);
    same_match(&initial, &recovered_game);
    public_only_event(store, 1u, 2u);
    require_store(mygames_room_store_get_outcome(store, 101u, "game-start",
            digest, original_reply, sizeof(original_reply), &reply_bytes,
            &sequence), MYGAMES_STORE_OK);
    assert(reply_bytes == sizeof(start_ack) - 1u);
    assert(memcmp(original_reply, start_ack, reply_bytes) == 0);

    assert(mygames_ddz_match_bid(&initial, 1u, 8u, 101u, 1u, 3u,
            &bid_next, &bid) == MYGAMES_DDZ_MATCH_OK);
    mygames_room_store_fail_after_checkpoint_once(store);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &initial, &bid_next, 1u, 2u, "game-bid", digest,
            bid_ack, sizeof(bid_ack) - 1u, &sequence),
            MYGAMES_STORE_BACKEND_ERROR);
    assert(sequence == 2u); /* No success sequence published. */
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_OK);
    same_match(&initial, &recovered_game);
    require_store(mygames_room_store_get_outcome(store, 101u, "game-bid",
            digest, original_reply, sizeof(original_reply), &reply_bytes,
            &sequence), MYGAMES_STORE_NOT_FOUND);
    require_store(mygames_room_store_next_event(store, 2u, original_reply,
            sizeof(original_reply), &reply_bytes, &sequence), MYGAMES_STORE_NOT_FOUND);

    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &initial, &bid_next, 1u, 2u, "game-bid", digest,
            bid_ack, sizeof(bid_ack) - 1u, &sequence), MYGAMES_STORE_OK);
    assert(sequence == 3u);
    bid_committed = bid_next;
    public_only_event(store, 2u, 3u);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &initial, &bid_next, 1u, 2u, "game-bid", digest,
            bid_ack, sizeof(bid_ack) - 1u, &sequence), MYGAMES_STORE_REPLAY);
    assert(sequence == 3u);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &initial, &bid_next, 1u, 2u, "game-bid", conflict,
            bid_ack, sizeof(bid_ack) - 1u, &sequence),
            MYGAMES_STORE_REQUEST_CONFLICT);

    require_store(mygames_ddz_match_private_hand(&bid_committed, 1u, 8u, 101u,
            &hand) == MYGAMES_DDZ_MATCH_OK ? MYGAMES_STORE_OK : MYGAMES_STORE_INVALID,
            MYGAMES_STORE_OK);
    card = hand & (~hand + UINT64_C(1));
    assert(mygames_ddz_match_play(&bid_committed, 1u, 8u, 101u, 1u, card,
            &played, &action) == MYGAMES_DDZ_MATCH_OK);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &bid_committed, &played, 1u, 3u, "game-play", digest,
            play_ack, sizeof(play_ack) - 1u, &sequence), MYGAMES_STORE_OK);
    assert(sequence == 4u);
    public_only_event(store, 3u, 4u);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_OK);
    assert(game_seq == 4u);
    same_match(&played, &recovered_game);

    mygames_room_store_close(store);
    store = NULL;
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    require_store(mygames_room_store_load(store, &recovered,
            &loaded_epoch, &loaded_seq), MYGAMES_STORE_OK);
    assert(loaded_epoch == 1u && loaded_seq == 4u);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_OK);
    assert(game_seq == 4u);
    same_match(&played, &recovered_game);
    require_store(mygames_room_store_get_outcome(store, 101u, "game-play",
            digest, original_reply, sizeof(original_reply), &reply_bytes,
            &sequence), MYGAMES_STORE_OK);
    assert(sequence == 4u && reply_bytes == sizeof(play_ack) - 1u);
    require_store(mygames_room_store_claim_epoch(store, 1u, &new_epoch),
                  MYGAMES_STORE_OK);
    assert(new_epoch == 2u);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_OK);
    assert(recovered_game.room_epoch == 2u && recovered_game.game_id == 8u);
    assert(recovered_game.round.turn_id == 2u);
    assert(recovered_game.round.played_cards == played.round.played_cards);
    require_store(mygames_room_store_commit_game(store, 101u, &rooms, "room-1",
            &bid_committed, &played, 1u, 4u, "stale-epoch", digest,
            play_ack, sizeof(play_ack) - 1u, &sequence), MYGAMES_STORE_STALE_EPOCH);
    mygames_room_store_close(store);
    store = NULL;

    corrupt_private_image_with_real_orm(argv[1], argv[2]);
    require_store(mygames_room_store_open(argv[1], argv[2], &store), MYGAMES_STORE_OK);
    require_store(mygames_room_store_load_game(store, "room-1",
            &recovered_game, &game_seq), MYGAMES_STORE_CORRUPT);
    require_store(mygames_room_store_claim_epoch(store, 2u, &new_epoch),
                  MYGAMES_STORE_CORRUPT);
    require_store(mygames_room_store_load(store, &recovered,
            &loaded_epoch, &loaded_seq), MYGAMES_STORE_OK);
    assert(loaded_epoch == 2u && loaded_seq == 4u);
    mygames_room_store_close(store);
    puts("mygames.game_store: PASS");
    return 0;
}
