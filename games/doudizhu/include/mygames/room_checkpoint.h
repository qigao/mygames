#ifndef MYGAMES_ROOM_CHECKPOINT_H
#define MYGAMES_ROOM_CHECKPOINT_H
#include "mygames/room_directory.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MYGAMES_CHECKPOINT_HEADER_BYTES 32u
#define MYGAMES_CHECKPOINT_ROOM_BYTES 88u
#define MYGAMES_CHECKPOINT_MAX_BYTES \
    (MYGAMES_CHECKPOINT_HEADER_BYTES + MYGAMES_ROOM_DIRECTORY_CAPACITY * MYGAMES_CHECKPOINT_ROOM_BYTES)

typedef enum mygames_checkpoint_status {
    MYGAMES_CHECKPOINT_OK = 0,
    MYGAMES_CHECKPOINT_INVALID,
    MYGAMES_CHECKPOINT_CAPACITY,
    MYGAMES_CHECKPOINT_UNSUPPORTED,
    MYGAMES_CHECKPOINT_CORRUPT
} mygames_checkpoint_status;

/* Versioned, canonical, cross-platform C11 room checkpoint codec.
 * No filesystem or persistence layer is implied. A transactional TurboDB
 * adapter must own durable storage, room_epoch fencing and journaling.
 * FNV-1a detects accidental damage; it is NOT a cryptographic MAC.
 * On any failure the destination remains unchanged. */
mygames_checkpoint_status mygames_room_checkpoint_encode(
    const mygames_room_directory *source, uint64_t room_epoch,
    void *output, size_t capacity, size_t *out_bytes);
mygames_checkpoint_status mygames_room_checkpoint_decode(
    const void *data, size_t size,
    mygames_room_directory *destination, uint64_t *out_epoch);
#ifdef __cplusplus
}
#endif
#endif
