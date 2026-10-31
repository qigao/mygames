#ifndef MYGAMES_DDZ_DMC_REPLAY_H
#define MYGAMES_DDZ_DMC_REPLAY_H

#include "mygames/ddz_dmc_episode.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Private offline training artifact only, not a CFlow/MQTT/WebSocket event.
 * Versioned canonical LITTLE-ENDIAN bytes; never memcpy C structs to disk.
 * Header = 64; every accepted play/pass = 1326; trailer = CRC32(IEEE).
 * CRC32 detects accidental corruption; it is NOT authentication/encryption.
 * Writer/reader must enforce their own ACL, provenance and file ownership.
 * Contains the acting player's own private-hand features, so it MUST NOT
 * be published as a CI artifact or exposed to competing players. */
#define MYGAMES_DDZ_DMC_REPLAY_VERSION 1u
#define MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES 64u
#define MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES 1326u
#define MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES 4u
#define MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES \
    (MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES + \
     MYGAMES_DDZ_DMC_MAX_STEPS * MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES + \
     MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES)

typedef enum mygames_ddz_dmc_replay_status {
    MYGAMES_DDZ_REPLAY_OK = 0,
    MYGAMES_DDZ_REPLAY_INVALID,
    MYGAMES_DDZ_REPLAY_CORRUPT,
    MYGAMES_DDZ_REPLAY_CAPACITY
} mygames_ddz_dmc_replay_status;

/* Caller-owned in-memory serialization; zero file/network I/O.
 * Query size by passing out_bytes=NULL with capacity=0. A failed call
 * leaves caller output intact. Only complete, sealed, self-consistent
 * full-episode reward labels qualify as training samples. */
mygames_ddz_dmc_replay_status mygames_ddz_dmc_replay_encode(
    const mygames_ddz_dmc_episode *episode, void *out_bytes,
    size_t capacity, size_t *out_size);

/* Query required sample count by passing out_episode=NULL, samples=NULL
 * and sample_capacity=0. Decode otherwise requires both and a
 * caller-owned sample buffer. CRC, full record structure, role/turn
 * ordering and objective targets are checked BEFORE publishing any sample.
 * Outputs remain unchanged on invalid/corrupt/capacity failure. */
mygames_ddz_dmc_replay_status mygames_ddz_dmc_replay_decode(
    const void *bytes, size_t byte_count,
    mygames_ddz_dmc_episode *out_episode,
    mygames_ddz_dmc_sample *out_samples, size_t sample_capacity,
    size_t *out_required_samples);

#ifdef __cplusplus
}
#endif
#endif
