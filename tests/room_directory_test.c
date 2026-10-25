#include "mygames/room_directory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_directory(void) {
    mygames_room_directory service;
    const mygames_room *a = NULL;
    const mygames_room *b = NULL;
    unsigned int seat = 99u;
    char id[MYGAMES_ROOM_ID_CAPACITY];
    uint64_t rev;
    mygames_room_directory_init(&service);
    assert(mygames_room_directory_create(&service, 101u, &a) == MYGAMES_ROOM_OK);
    assert(a && strcmp(a->id, "room-1") == 0);
    memcpy(id, a->id, strlen(a->id) + 1u);
    rev = a->revision;
    assert(mygames_room_directory_create(&service, 101u, &b) ==
           MYGAMES_ROOM_ALREADY_PRESENT);
    assert(b == a && a->revision == rev);
    assert(mygames_room_directory_get_player(&service, 101u) == a);
    assert(mygames_room_directory_join(&service, id, 202u, &seat) ==
           MYGAMES_ROOM_OK && seat == 1u);
    assert(mygames_room_directory_join(&service, id, 202u, &seat) ==
           MYGAMES_ROOM_ALREADY_PRESENT && seat == 1u);
    assert(mygames_room_directory_create(&service, 303u, &b) == MYGAMES_ROOM_OK);
    assert(strcmp(b->id, "room-2") == 0);
    assert(mygames_room_directory_join(&service, id, 303u, &seat) ==
           MYGAMES_ROOM_IN_OTHER_ROOM);
    assert(mygames_room_directory_ready(&service, 202u, true) == MYGAMES_ROOM_OK);
    rev = a->revision;
    assert(mygames_room_directory_ready(&service, 202u, true) ==
           MYGAMES_ROOM_ALREADY_PRESENT && a->revision == rev);
    assert(mygames_room_directory_leave(&service, 101u) == MYGAMES_ROOM_OK);
    assert(a->owner == 202u);
    assert(mygames_room_directory_leave(&service, 202u) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_get(&service, id) == NULL);
    assert(mygames_room_directory_join(&service, "does-not-exist", 404u, &seat) ==
           MYGAMES_ROOM_NOT_FOUND);
    assert(mygames_room_directory_leave(&service, 202u) == MYGAMES_ROOM_NOT_FOUND);
}

static void test_capacity_and_recycle(void) {
    mygames_room_directory service;
    const mygames_room *room;
    unsigned int i;
    char first[MYGAMES_ROOM_ID_CAPACITY];
    mygames_room_directory_init(&service);
    for (i = 1u; i <= MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i)
        assert(mygames_room_directory_create(&service, (mygames_player_id)i, &room) ==
               MYGAMES_ROOM_OK);
    assert(mygames_room_directory_create(&service, 100u, &room) == MYGAMES_ROOM_FULL);
    assert(mygames_room_directory_get_player(&service, 100u) == NULL);
    assert(mygames_room_directory_get_player(&service, 1u) != NULL);
    (void)snprintf(first, sizeof(first), "room-1");
    assert(mygames_room_directory_leave(&service, 1u) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_get(&service, first) == NULL);
    assert(mygames_room_directory_create(&service, 100u, &room) == MYGAMES_ROOM_OK);
    assert(room && strcmp(room->id, "room-33") == 0);
}

int main(void) {
    test_directory();
    test_capacity_and_recycle();
    puts("mygames.room_directory: PASS");
    return 0;
}
