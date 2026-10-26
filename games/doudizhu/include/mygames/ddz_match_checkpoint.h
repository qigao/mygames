#ifndef MYGAMES_DDZ_MATCH_CHECKPOINT_H
#define MYGAMES_DDZ_MATCH_CHECKPOINT_H

#include "mygames/ddz_match.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version 2, canonical little-endian, fixed-size private game image.
 * This format contains ALL players' SECRET physical cards and the exact
 * CSPRNG shuffle permutation. It must ONLY be written through a trusted
 * TurboDB Owner transaction. Never expose it to public WebSocket, MQTT,
 * Cocos, analytics or logs. No direct file I/O or persistent C runtime.
 *
 * Checksum is FNV-1a for accidental corruption, NOT a signature, MAC or
 * encryption. Protect database/media at rest and authenticate access.
 */
#define MYGAMES_DDZ_CHECKPOINT_V2_BYTES 224u

typedef enum mygames_ddz_checkpoint_status {
    MYGAMES_DDZ_CHECKPOINT_OK = 0,
    MYGAMES_DDZ_CHECKPOINT_INVALID,
    MYGAMES_DDZ_CHECKPOINT_CAPACITY,
    MYGAMES_DDZ_CHECKPOINT_UNSUPPORTED,
    MYGAMES_DDZ_CHECKPOINT_CORRUPT
} mygames_ddz_checkpoint_status;

/* Encode or decode only after a complete state validation.
 * On EVERY failure, output bytes, size and destination match are unchanged.
 * Decoder rejects unknown versions, truncated/trailing/noncanonical data,
 * bad checksum, impossible phases, forged bids, shuffled duplicates, card
 * overlap or premature winner/turn corruption. */
mygames_ddz_checkpoint_status mygames_ddz_checkpoint_encode(
    const mygames_ddz_match *source,
    void *out_bytes, size_t capacity, size_t *out_size);
mygames_ddz_checkpoint_status mygames_ddz_checkpoint_decode(
    const void *bytes, size_t size, mygames_ddz_match *out_match);

#ifdef __cplusplus
}
#endif
#endif
