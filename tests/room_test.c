#include "mygames/room.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_room_lifecycle(void) {
    mygames_room r;
    unsigned int seat = 99u;
    uint64_t revision;

    assert(mygames_room_init(&r, "r-1", 11u) == MYGAMES_ROOM_OK);
    assert(!strcmp(r.id, "r-1"));
    assert(r.players == 1u && r.owner == 11u && r.revision == 1u);
    assert(mygames_room_find_seat(&r, 11u, &seat) == MYGAMES_ROOM_OK && seat == 0u);
    assert(mygames_room_join(&r, 11u, &seat) == MYGAMES_ROOM_ALREADY_PRESENT);
    assert(r.revision == 1u);
    assert(mygames_room_join(&r, 22u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    assert(mygames_room_join(&r, 33u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    revision = r.revision;
    assert(mygames_room_join(&r, 44u, &seat) == MYGAMES_ROOM_FULL);
    assert(r.revision == revision);
    assert(mygames_room_start(&r, 22u) == MYGAMES_ROOM_NOT_OWNER);
    assert(mygames_room_start(&r, 11u) == MYGAMES_ROOM_NOT_READY);
    assert(mygames_room_set_ready(&r, 11u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_set_ready(&r, 22u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_set_ready(&r, 33u, true) == MYGAMES_ROOM_OK);
    revision = r.revision;
    assert(mygames_room_set_ready(&r, 33u, true) == MYGAMES_ROOM_ALREADY_PRESENT);
    assert(r.revision == revision);
    assert(mygames_room_start(&r, 11u) == MYGAMES_ROOM_OK);
    assert(r.phase == MYGAMES_ROOM_STARTING);
    assert(mygames_room_join(&r, 44u, &seat) == MYGAMES_ROOM_WRONG_PHASE);
    assert(mygames_room_leave(&r, 22u) == MYGAMES_ROOM_WRONG_PHASE);
}

static void test_handover(void) {
    mygames_room r;
    unsigned int seat;
    assert(mygames_room_init(&r, "r2", 100u) == MYGAMES_ROOM_OK);
    assert(mygames_room_join(&r, 200u, &seat) == MYGAMES_ROOM_OK);
    assert(mygames_room_join(&r, 300u, &seat) == MYGAMES_ROOM_OK);
    assert(mygames_room_leave(&r, 100u) == MYGAMES_ROOM_OK);
    assert(r.owner == 200u && r.players == 2u);
    assert(mygames_room_join(&r, 400u, &seat) == MYGAMES_ROOM_OK && seat == 0u);
    assert(mygames_room_leave(&r, 200u) == MYGAMES_ROOM_OK);
    assert(r.owner == 400u);
    assert(mygames_room_leave(&r, 400u) == MYGAMES_ROOM_OK);
    assert(r.owner == 300u);
    assert(mygames_room_leave(&r, 300u) == MYGAMES_ROOM_OK);
    assert(r.phase == MYGAMES_ROOM_CLOSED && r.players == 0u && r.owner == 0u);
    assert(mygames_room_join(&r, 500u, &seat) == MYGAMES_ROOM_WRONG_PHASE);
}

static void test_invalid(void) {
    mygames_room r;
    unsigned int seat = 99u;
    const char *long_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    assert(mygames_room_init(NULL, "abc", 1u) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_init(&r, "", 1u) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_init(&r, "r/1", 1u) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_init(&r, long_id, 1u) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_init(&r, "abc", 0u) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_init(&r, "abc", 1u) == MYGAMES_ROOM_OK);
    assert(mygames_room_find_seat(&r, 22u, &seat) == MYGAMES_ROOM_NOT_FOUND);
    assert(seat == 99u);
    assert(mygames_room_join(&r, 0u, &seat) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_join(&r, 2u, NULL) == MYGAMES_ROOM_INVALID_ARGUMENT);
    assert(mygames_room_set_ready(&r, 2u, true) == MYGAMES_ROOM_NOT_FOUND);
    assert(mygames_room_leave(&r, 2u) == MYGAMES_ROOM_NOT_FOUND);
    assert(mygames_room_start(&r, 2u) == MYGAMES_ROOM_NOT_OWNER);
}

int main(void) {
    test_room_lifecycle();
    test_handover();
    test_invalid();
    puts("mygames.room: PASS");
    return 0;
}
