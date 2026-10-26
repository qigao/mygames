#ifndef MYGAMES_DOUDIZHU_ROOM_DIRECTORY_H
#define MYGAMES_DOUDIZHU_ROOM_DIRECTORY_H

#include "mygames/room.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_ROOM_DIRECTORY_CAPACITY 32u

typedef struct mygames_room_directory_entry {
    bool used;
    mygames_room room;
} mygames_room_directory_entry;

typedef struct mygames_room_directory {
    uint64_t next_serial;
    mygames_room_directory_entry entries[MYGAMES_ROOM_DIRECTORY_CAPACITY];
} mygames_room_directory;

/* Single-writer, non-thread-safe room authority; all results borrowed until
 * next directory mutation. No network, MQTT, or persistence is owned here. */
void mygames_room_directory_init(mygames_room_directory *directory);
const mygames_room *mygames_room_directory_get(const mygames_room_directory *directory,
                                                const char *room_id);
const mygames_room *mygames_room_directory_get_player(
    const mygames_room_directory *directory, mygames_player_id player);

mygames_room_status mygames_room_directory_create(mygames_room_directory *directory,
                                                   mygames_player_id player,
                                                   const mygames_room **out_room);
mygames_room_status mygames_room_directory_join(mygames_room_directory *directory,
                                                 const char *room_id,
                                                 mygames_player_id player,
                                                 unsigned int *out_seat);
mygames_room_status mygames_room_directory_ready(mygames_room_directory *directory,
                                                  mygames_player_id player, bool ready);
mygames_room_status mygames_room_directory_leave(mygames_room_directory *directory,
                                                  mygames_player_id player);

#ifdef __cplusplus
}
#endif
#endif
