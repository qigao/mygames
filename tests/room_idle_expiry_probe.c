#include "mygames/room_store.h"
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
    uint64_t epoch = 0u, seq = 0u, outbox_seq = 0u;
    unsigned long long id;
    char *end = NULL;
    uint8_t event[MYGAMES_STORE_EVENT_MAX + 1u];
    size_t length = 0u;
    if (argc != 4) return 2;
    errno = 0;
    id = strtoull(argv[3], &end, 10);
    if (errno || !end || *end || id == 0u ||
        id > (unsigned long long)INT64_MAX) return 2;
    assert(mygames_room_store_open(argv[1], argv[2], &store) == MYGAMES_STORE_OK);
    assert(mygames_room_store_load(store, &rooms, &epoch, &seq) == MYGAMES_STORE_OK);
    assert(epoch == 2u && seq == 3u);
    room = mygames_room_directory_get(&rooms, "room-1");
    assert(room != NULL);
    assert(room->players == 1u && room->revision == 3u);
    assert(mygames_room_directory_get_player(&rooms, (mygames_player_id)id) == NULL);
    assert(mygames_room_store_has_session(store, (mygames_player_id)id) ==
           MYGAMES_STORE_NOT_FOUND);
    assert(mygames_room_store_next_event(
        store, 2u, event, sizeof(event) - 1u, &length, &outbox_seq) ==
        MYGAMES_STORE_OK);
    assert(outbox_seq == 3u && length > 0u && length < sizeof(event));
    event[length] = '\0';
    assert(strstr((char *)event, "room.session_expired") != NULL);
    assert(strstr((char *)event, "\"seq\":3") != NULL);
    mygames_room_store_close(store);
    puts("mygames.idle_expiry_store: PASS (journal + Outbox + lease)");
    return 0;
}
