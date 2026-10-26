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
    MYGAMES_RECOVERY_WIRE_CAPACITY,
    MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL
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

/* Candidate M4.3 response decoder. The only accepted representation is the
 * exact versioned, ordered and unescaped JSON emitted by encode_page().
 * It strictly binds SOURCE NAMESPACE and CONSUMER ID to the caller's fixed,
 * already-authenticated service policy; payloads are canonical public events.
 *
 * after_committed_seq MUST come from the consumer's durable local projection
 * snapshot (never from HTTP resume_after). No writes/ACK/cursor modification
 * occur here. out_page is assigned atomically only after complete validation.
 * Max 16 events / 32 KiB input; malformed names, gaps, retention, partial
 * responses, unexpected fields, numeric overflow and forged identity reject.
 *
 * Caller MUST use an independent verified TLS client and check HTTP status,
 * Content-Type/body size, then feed only HTTP 200 bytes to this parser.
 */
mygames_recovery_wire_status mygames_recovery_decode_page(
    const void *response_bytes, size_t response_size,
    const char *expected_source_namespace, const char *expected_consumer_id,
    uint64_t after_committed_seq,
    mygames_recovery_public_event_validate_fn validate_event,
    mygames_store_archive_page *out_page);
#ifdef __cplusplus
}
#endif
#endif
