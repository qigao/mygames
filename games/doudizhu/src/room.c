#include "mygames/room.h"
#include <string.h>

static bool valid_room_id(const char *id) {
    size_t n = 0u;
    if (!id) return false;
    for (; n < MYGAMES_ROOM_ID_CAPACITY; ++n) {
        const unsigned char ch = (unsigned char)id[n];
        if (!ch) return n != 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_')) return false;
    }
    return false;
}

mygames_room_status mygames_room_find_seat(const mygames_room *room,
                                            mygames_player_id player,
                                            unsigned int *out_seat) {
    unsigned int i;
    if (!room || !out_seat || player == 0u) return MYGAMES_ROOM_INVALID_ARGUMENT;
    for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i) {
        if (room->seats[i].player == player) {
            *out_seat = i;
            return MYGAMES_ROOM_OK;
        }
    }
    return MYGAMES_ROOM_NOT_FOUND;
}

mygames_room_status mygames_room_init(mygames_room *room, const char *id,
                                      mygames_player_id creator) {
    if (!room || !valid_room_id(id) || creator == 0u)
        return MYGAMES_ROOM_INVALID_ARGUMENT;
    memset(room, 0, sizeof(*room));
    memcpy(room->id, id, strlen(id) + 1u);
    room->phase = MYGAMES_ROOM_WAITING;
    room->owner = creator;
    room->seats[0].player = creator;
    room->players = 1u;
    room->revision = 1u;
    return MYGAMES_ROOM_OK;
}

mygames_room_status mygames_room_join(mygames_room *room, mygames_player_id player,
                                      unsigned int *out_seat) {
    unsigned int i, found;
    mygames_room_status result;
    if (!room || !out_seat || player == 0u)
        return MYGAMES_ROOM_INVALID_ARGUMENT;
    result = mygames_room_find_seat(room, player, &found);
    if (result == MYGAMES_ROOM_OK) {
        *out_seat = found;
        return MYGAMES_ROOM_ALREADY_PRESENT;
    }
    if (room->phase != MYGAMES_ROOM_WAITING) return MYGAMES_ROOM_WRONG_PHASE;
    if (room->players == MYGAMES_ROOM_SEATS) return MYGAMES_ROOM_FULL;
    for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i) {
        if (!room->seats[i].player) {
            room->seats[i].player = player;
            room->seats[i].ready = false;
            room->players++;
            room->revision++;
            *out_seat = i;
            return MYGAMES_ROOM_OK;
        }
    }
    return MYGAMES_ROOM_FULL;
}

mygames_room_status mygames_room_set_ready(mygames_room *room,
                                           mygames_player_id player, bool ready) {
    unsigned int seat;
    mygames_room_status status;
    if (!room || player == 0u) return MYGAMES_ROOM_INVALID_ARGUMENT;
    if (room->phase != MYGAMES_ROOM_WAITING) return MYGAMES_ROOM_WRONG_PHASE;
    status = mygames_room_find_seat(room, player, &seat);
    if (status != MYGAMES_ROOM_OK) return status;
    if (room->seats[seat].ready == ready) return MYGAMES_ROOM_ALREADY_PRESENT;
    room->seats[seat].ready = ready;
    room->revision++;
    return MYGAMES_ROOM_OK;
}

mygames_room_status mygames_room_leave(mygames_room *room, mygames_player_id player) {
    unsigned int seat, i;
    mygames_room_status status;
    if (!room || player == 0u) return MYGAMES_ROOM_INVALID_ARGUMENT;
    if (room->phase != MYGAMES_ROOM_WAITING) return MYGAMES_ROOM_WRONG_PHASE;
    status = mygames_room_find_seat(room, player, &seat);
    if (status != MYGAMES_ROOM_OK) return status;
    room->seats[seat] = (mygames_room_seat){0};
    room->players--;
    if (room->owner == player) {
        room->owner = 0u;
        for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i) {
            if (room->seats[i].player != 0u) {
                room->owner = room->seats[i].player;
                break;
            }
        }
    }
    if (room->players == 0u) room->phase = MYGAMES_ROOM_CLOSED;
    room->revision++;
    return MYGAMES_ROOM_OK;
}

mygames_room_status mygames_room_start(mygames_room *room,
                                       mygames_player_id requester) {
    unsigned int i;
    if (!room || requester == 0u) return MYGAMES_ROOM_INVALID_ARGUMENT;
    if (room->phase != MYGAMES_ROOM_WAITING) return MYGAMES_ROOM_WRONG_PHASE;
    if (requester != room->owner) return MYGAMES_ROOM_NOT_OWNER;
    if (room->players != MYGAMES_ROOM_SEATS) return MYGAMES_ROOM_NOT_READY;
    for (i = 0u; i < MYGAMES_ROOM_SEATS; ++i) {
        if (!room->seats[i].player || !room->seats[i].ready)
            return MYGAMES_ROOM_NOT_READY;
    }
    room->phase = MYGAMES_ROOM_STARTING;
    room->revision++;
    return MYGAMES_ROOM_OK;
}
