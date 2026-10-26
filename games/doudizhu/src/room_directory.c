#include "mygames/room_directory.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void mygames_room_directory_init(mygames_room_directory *directory) {
    if (directory) memset(directory, 0, sizeof(*directory));
}

static mygames_room_directory_entry *find_room(mygames_room_directory *directory,
                                                const char *room_id) {
    size_t i;
    if (!directory || !room_id) return NULL;
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        mygames_room_directory_entry *entry = &directory->entries[i];
        if (entry->used && strcmp(entry->room.id, room_id) == 0) return entry;
    }
    return NULL;
}

static mygames_room_directory_entry *find_player(mygames_room_directory *directory,
                                                  mygames_player_id player) {
    size_t i;
    unsigned int seat;
    if (!directory || !player) return NULL;
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        mygames_room_directory_entry *entry = &directory->entries[i];
        if (entry->used && mygames_room_find_seat(&entry->room, player, &seat) ==
            MYGAMES_ROOM_OK) return entry;
    }
    return NULL;
}

const mygames_room *mygames_room_directory_get(const mygames_room_directory *directory,
                                                const char *room_id) {
    mygames_room_directory_entry *entry =
        find_room((mygames_room_directory *)directory, room_id);
    return entry ? &entry->room : NULL;
}

const mygames_room *mygames_room_directory_get_player(
    const mygames_room_directory *directory, mygames_player_id player) {
    mygames_room_directory_entry *entry =
        find_player((mygames_room_directory *)directory, player);
    return entry ? &entry->room : NULL;
}

mygames_room_status mygames_room_directory_create(mygames_room_directory *directory,
                                                   mygames_player_id player,
                                                   const mygames_room **out_room) {
    size_t i;
    char room_id[MYGAMES_ROOM_ID_CAPACITY];
    mygames_room_directory_entry *existing;
    if (!directory || !player || !out_room) return MYGAMES_ROOM_INVALID_ARGUMENT;
    *out_room = NULL;
    existing = find_player(directory, player);
    if (existing) {
        *out_room = &existing->room;
        return MYGAMES_ROOM_ALREADY_PRESENT;
    }
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        mygames_room_directory_entry *entry = &directory->entries[i];
        int len;
        mygames_room_status status;
        if (entry->used) continue;
        if (directory->next_serial == UINT64_MAX) return MYGAMES_ROOM_FULL;
        len = snprintf(room_id, sizeof(room_id), "room-%" PRIu64,
                       ++directory->next_serial);
        if (len <= 0 || (size_t)len >= sizeof(room_id))
            return MYGAMES_ROOM_INVALID_ARGUMENT;
        status = mygames_room_init(&entry->room, room_id, player);
        if (status != MYGAMES_ROOM_OK) return status;
        entry->used = true;
        *out_room = &entry->room;
        return MYGAMES_ROOM_OK;
    }
    return MYGAMES_ROOM_FULL;
}

mygames_room_status mygames_room_directory_join(mygames_room_directory *directory,
                                                 const char *room_id,
                                                 mygames_player_id player,
                                                 unsigned int *out_seat) {
    mygames_room_directory_entry *entry, *prior;
    if (!directory || !room_id || !player || !out_seat)
        return MYGAMES_ROOM_INVALID_ARGUMENT;
    entry = find_room(directory, room_id);
    if (!entry) return MYGAMES_ROOM_NOT_FOUND;
    prior = find_player(directory, player);
    if (prior && prior != entry) return MYGAMES_ROOM_IN_OTHER_ROOM;
    return mygames_room_join(&entry->room, player, out_seat);
}

mygames_room_status mygames_room_directory_ready(mygames_room_directory *directory,
                                                  mygames_player_id player, bool ready) {
    mygames_room_directory_entry *entry = find_player(directory, player);
    if (!directory || !player) return MYGAMES_ROOM_INVALID_ARGUMENT;
    if (!entry) return MYGAMES_ROOM_NOT_FOUND;
    return mygames_room_set_ready(&entry->room, player, ready);
}

mygames_room_status mygames_room_directory_start(mygames_room_directory *directory,
                                                 mygames_player_id requester) {
    mygames_room_directory_entry *entry;
    if (!directory || !requester) return MYGAMES_ROOM_INVALID_ARGUMENT;
    entry = find_player(directory, requester);
    if (!entry) return MYGAMES_ROOM_NOT_FOUND;
    return mygames_room_start(&entry->room, requester);
}

mygames_room_status mygames_room_directory_leave(mygames_room_directory *directory,
                                                  mygames_player_id player) {
    mygames_room_directory_entry *entry = find_player(directory, player);
    mygames_room_status status;
    if (!directory || !player) return MYGAMES_ROOM_INVALID_ARGUMENT;
    if (!entry) return MYGAMES_ROOM_NOT_FOUND;
    status = mygames_room_leave(&entry->room, player);
    if (status == MYGAMES_ROOM_OK && entry->room.phase == MYGAMES_ROOM_CLOSED)
        memset(entry, 0, sizeof(*entry));
    return status;
}
