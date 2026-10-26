#include "mygames/room_store.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory rooms;
    uint8_t payload[MYGAMES_STORE_EVENT_MAX];
    size_t length = 0u;
    uint64_t epoch = 0u, committed = 0u, next_seq = 0u;
    char *end = NULL;
    unsigned long expected;
    mygames_store_status result;
    if (argc != 4) return 2;
    expected = strtoul(argv[3], &end, 10);
    if (!end || *end || expected > 3ul) return 2;
    assert(mygames_room_store_open(argv[1], argv[2], &store) == MYGAMES_STORE_OK);
    assert(mygames_room_store_load(store, &rooms, &epoch, &committed) ==
           MYGAMES_STORE_OK);
    assert(epoch == 1u && committed >= 2u);
    result = mygames_room_store_next_event(
        store, 0u, payload, sizeof(payload), &length, &next_seq);
    if (expected == 0ul) assert(result == MYGAMES_STORE_NOT_FOUND);
    else {
        assert(result == MYGAMES_STORE_OK);
        assert(next_seq == expected && length > 0u);
    }
    mygames_room_store_close(store);
    puts("mygames.flowie_outbox_audit: PASS");
    return 0;
}
