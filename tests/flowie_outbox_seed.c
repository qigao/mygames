/* Real TurboDB writer fixture: two committed sanitized room events and
 * an optional third event for the broker-outage recovery test. */
#include "mygames/room_store.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void commit_event(mygames_room_store *store,
                         mygames_room_directory *room, uint64_t seq,
                         const char *request_id, uint8_t fingerprint,
                         const char *event) {
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    uint64_t settled = 0u;
    const char reply[] = "{\"v\":1,\"type\":\"room.ack\"}";
    memset(digest, fingerprint, sizeof(digest));
    assert(mygames_room_store_commit(
        store, 101u, room, 1u, seq - 1u, request_id, digest,
        reply, sizeof(reply) - 1u, event, strlen(event), &settled)
        == MYGAMES_STORE_OK);
    assert(settled == seq);
}

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory room;
    const mygames_room *created = NULL;
    uint64_t epoch = 0u, seq = 0u;
    if (argc != 4) {
        fprintf(stderr, "usage: seed sqlite-plugin db --seed|--append\n");
        return 2;
    }
    assert(mygames_room_store_open(argv[1], argv[2], &store) == MYGAMES_STORE_OK);
    assert(mygames_room_store_load(store, &room, &epoch, &seq) == MYGAMES_STORE_OK);
    assert(epoch == 1u);
    if (strcmp(argv[3], "--seed") == 0) {
        assert(seq == 0u);
        assert(mygames_room_directory_create(&room, 101u, &created) ==
               MYGAMES_ROOM_OK);
        commit_event(store, &room, 1u, "event1", 0x11u,
            "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
            "\"room_epoch\":1,\"revision\":1,\"seq\":1}");
        assert(mygames_room_directory_ready(&room, 101u, true) ==
               MYGAMES_ROOM_OK);
        commit_event(store, &room, 2u, "event2", 0x22u,
            "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
            "\"room_epoch\":1,\"revision\":2,\"seq\":2}");
    } else if (strcmp(argv[3], "--append") == 0) {
        assert(seq == 2u);
        assert(mygames_room_directory_ready(&room, 101u, false) ==
               MYGAMES_ROOM_OK);
        commit_event(store, &room, 3u, "event3", 0x33u,
            "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
            "\"room_epoch\":1,\"revision\":3,\"seq\":3}");
    } else return 2;
    mygames_room_store_close(store);
    puts("mygames.flowie_seed: PASS");
    return 0;
}
