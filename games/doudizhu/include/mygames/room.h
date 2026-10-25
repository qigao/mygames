#ifndef MYGAMES_DOUDIZHU_ROOM_H
#define MYGAMES_DOUDIZHU_ROOM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_ROOM_SEATS 3u
#define MYGAMES_ROOM_ID_CAPACITY 40u

typedef uint64_t mygames_player_id;

typedef enum mygames_room_phase {
    MYGAMES_ROOM_WAITING = 0,
    MYGAMES_ROOM_STARTING = 1,
    MYGAMES_ROOM_CLOSED = 2
} mygames_room_phase;

typedef enum mygames_room_status {
    MYGAMES_ROOM_OK = 0,
    MYGAMES_ROOM_INVALID_ARGUMENT,
    MYGAMES_ROOM_ALREADY_PRESENT,
    MYGAMES_ROOM_FULL,
    MYGAMES_ROOM_NOT_FOUND,
    MYGAMES_ROOM_WRONG_PHASE,
    MYGAMES_ROOM_NOT_OWNER,
    MYGAMES_ROOM_NOT_READY,
    MYGAMES_ROOM_IN_OTHER_ROOM
} mygames_room_status;

typedef struct mygames_room_seat {
    mygames_player_id player;
    bool ready;
} mygames_room_seat;

typedef struct mygames_room {
    char id[MYGAMES_ROOM_ID_CAPACITY];
    mygames_room_phase phase;
    mygames_player_id owner;
    mygames_room_seat seats[MYGAMES_ROOM_SEATS];
    uint64_t revision;
    unsigned int players;
} mygames_room;

/* Mutate only on the unique authoritative Room Owner. This POD is not thread-safe.
 * Room id: ASCII [A-Za-z0-9_-], length 1..39. Player id: nonzero. */
mygames_room_status mygames_room_init(mygames_room *room, const char *id,
                                      mygames_player_id creator);
mygames_room_status mygames_room_join(mygames_room *room, mygames_player_id player,
                                      unsigned int *out_seat);
mygames_room_status mygames_room_set_ready(mygames_room *room,
                                           mygames_player_id player, bool ready);
mygames_room_status mygames_room_leave(mygames_room *room, mygames_player_id player);
mygames_room_status mygames_room_start(mygames_room *room, mygames_player_id requester);
mygames_room_status mygames_room_find_seat(const mygames_room *room,
                                           mygames_player_id player, unsigned int *out_seat);
#ifdef __cplusplus
}
#endif
#endif
