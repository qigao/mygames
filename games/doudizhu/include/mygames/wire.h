#ifndef MYGAMES_WIRE_H
#define MYGAMES_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_WIRE_MAX_BYTES 512u
#define MYGAMES_REQUEST_ID_CAPACITY 49u
#define MYGAMES_WIRE_ROOM_ID_CAPACITY 40u

typedef enum mygames_wire_status {
    MYGAMES_WIRE_OK = 0,
    MYGAMES_WIRE_INVALID,
    MYGAMES_WIRE_TOO_LARGE,
    MYGAMES_WIRE_UNSUPPORTED_VERSION,
    MYGAMES_WIRE_UNSUPPORTED_TYPE
} mygames_wire_status;

typedef enum mygames_command_kind {
    MYGAMES_COMMAND_PING = 0,
    MYGAMES_COMMAND_CREATE_ROOM,
    MYGAMES_COMMAND_JOIN_ROOM,
    MYGAMES_COMMAND_READY,
    MYGAMES_COMMAND_LEAVE_ROOM,
    MYGAMES_COMMAND_GET_SNAPSHOT
} mygames_command_kind;

typedef struct mygames_wire_command {
    mygames_command_kind kind;
    char request_id[MYGAMES_REQUEST_ID_CAPACITY];
    char room_id[MYGAMES_WIRE_ROOM_ID_CAPACITY];
    bool ready;
} mygames_wire_command;

/* Strict, bounded subset of JSON objects with ASCII string values, boolean
 * and integer protocol version. Unknown/duplicate members, string escapes,
 * Unicode, nested JSON and trailing bytes are rejected by design.
 * This is a development protocol and MUST NOT be considered authentication.
 */
mygames_wire_status mygames_wire_decode_command(const void *bytes, size_t size,
                                                 mygames_wire_command *out);

#ifdef __cplusplus
}
#endif
#endif
