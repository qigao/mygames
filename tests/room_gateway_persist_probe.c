#include "mygames/room_store.h"
#include "mygames/wire.h"
#include <cmeta_crypto.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory rooms;
    const mygames_room *room;
    mygames_player_id player_max = 0u;
    uint64_t epoch = 0u, sequence = 0u, old_seq = 0u;
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    uint8_t event[MYGAMES_STORE_EVENT_MAX];
    char ack[MYGAMES_STORE_RESULT_MAX + 1u];
    size_t ack_len = 0u, event_len = 0u;
    static const char canonical[] = "v1|1||0";
    char *end = NULL;
    unsigned long long creator_id = 0u;
    const int expect_empty = argc == 4 &&
                             strcmp(argv[3], "--expect-empty") == 0;
    if (argc != 4) {
        fprintf(stderr, "usage: gateway_probe /absolute/sqlite-driver /database /creator-id\n");
        return 2;
    }
    if (!expect_empty) {
        errno = 0;
        creator_id = strtoull(argv[3], &end, 10);
        if (errno != 0 || !end || *end || creator_id == 0u ||
            creator_id > (unsigned long long)INT64_MAX) return 2;
    }
    assert(mygames_room_store_open(argv[1], argv[2], &store) ==
           MYGAMES_STORE_OK);
    assert(store != NULL);
    assert(mygames_room_store_load(store, &rooms, &epoch, &sequence) ==
           MYGAMES_STORE_OK);
    assert(epoch == 2u);
    if (expect_empty) {
        unsigned int i;
        assert(sequence == 0u && rooms.next_serial == 0u);
        for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i)
            assert(!rooms.entries[i].used);
        assert(mygames_room_store_player_high_watermark(store, &player_max) ==
               MYGAMES_STORE_OK && player_max == 0u);
        assert(mygames_room_store_next_event(
            store, 0u, event, sizeof(event), &event_len, &old_seq) ==
               MYGAMES_STORE_NOT_FOUND);
        mygames_room_store_close(store);
        puts("mygames.gateway_fault_probe: PASS (no state, no ACK, no event)");
        return 0;
    }
    assert(sequence >= 7u);
    room = mygames_room_directory_get(&rooms, "room-1");
    assert(room != NULL);
    assert(room->players == 3u);
    assert(room->owner != 0u);
    assert(room->revision >= 7u);
    assert(rooms.next_serial == 1u);
    assert(mygames_room_store_player_high_watermark(store, &player_max) ==
           MYGAMES_STORE_OK);
    assert(player_max >= 5u);

    assert(cmeta_sha256(canonical, sizeof(canonical) - 1u, digest) == SALTS_OK);
    {
        const mygames_store_status status = mygames_room_store_get_outcome(
            store, (mygames_player_id)creator_id, "create-1", digest,
            ack, sizeof(ack) - 1u, &ack_len, &old_seq);
        if (status != MYGAMES_STORE_OK) {
            fprintf(stderr, "persist probe failed: status=%d player_max=%llu "
                    "epoch=%llu sequence=%llu error=%s\n",
                    (int)status, (unsigned long long)player_max,
                    (unsigned long long)epoch, (unsigned long long)sequence,
                    mygames_room_store_last_error(store));
        }
        assert(status == MYGAMES_STORE_OK);
    }
    assert(old_seq == 1u && ack_len > 0u && ack_len < sizeof(ack));
    ack[ack_len] = '\0';
    assert(strstr(ack, "\"type\":\"room.ack\"") != NULL);
    assert(strstr(ack, "\"request_id\":\"create-1\"") != NULL);
    assert(strstr(ack, "\"room_id\":\"room-1\"") != NULL);

    assert(mygames_room_store_next_event(
        store, 0u, event, sizeof(event), &event_len, &old_seq) ==
           MYGAMES_STORE_OK);
    assert(old_seq == 1u);
    assert(event_len > 0u);
    assert(event_len < sizeof(event));
    event[event_len] = '\0';
    assert(strstr((const char *)event, "room.command_accepted") != NULL);
    mygames_room_store_close(store);
    puts("mygames.gateway_durable_probe: PASS");
    return 0;
}
