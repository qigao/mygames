#ifndef MYGAMES_RECOVERY_WIRE_H
#define MYGAMES_RECOVERY_WIRE_H
#include "mygames/recovery_gate.h"
#include "mygames/room_store.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* No network, persistence, JSON library, or policy fallback in this module.
 * The caller passes a generation-fenced grant from RecoveryGate and MUST
 * supply the canonical MyGames::Outbox public-event validator. */
#define MYGAMES_RECOVERY_RESPONSE_MAX 32768u
#define MYGAMES_RECOVERY_PATH "/internal/recovery/v1/events"

typedef enum mygames_recovery_wire_status {
    MYGAMES_RECOVERY_WIRE_OK = 0,
    MYGAMES_RECOVERY_WIRE_BAD_QUERY,
    MYGAMES_RECOVERY_WIRE_INVALID_PAGE,
    MYGAMES_RECOVERY_WIRE_CAPACITY
} mygames_recovery_wire_status;

typedef bool (*mygames_recovery_public_event_validate_fn)(
    const uint8_t *payload, size_t size, uint64_t source_epoch,
    uint64_t event_sequence);

/* Strict origin-form target only. Exact parameter names once each, decimal
 * ASCII only, no percent escape/plus/zero padding/fragment/unknown fields.
 * Rejects after > INT64_MAX, limit outside [1,16], missing/duplicate values. */
mygames_recovery_wire_status mygames_recovery_parse_target(
    const char *target, uint64_t *out_after, uint32_t *out_limit);

/* Encode only canonical validated public events, preserving committed bytes.
 * The output is intended for HTTP 200 after a successfully completed read
 * snapshot; it does NOT persist, ACK, or advance the consumer's cursor. */
mygames_recovery_wire_status mygames_recovery_encode_page(
    const mygames_recovery_grant *grant,
    const mygames_store_archive_page *page,
    mygames_recovery_public_event_validate_fn validate_event,
    char *output, size_t output_capacity, size_t *out_bytes);
#ifdef __cplusplus
}
#endif
#endif
