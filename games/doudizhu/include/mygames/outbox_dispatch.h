#ifndef MYGAMES_OUTBOX_DISPATCH_H
#define MYGAMES_OUTBOX_DISPATCH_H

#include "mygames/room_store.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* This topic is service-internal. Browser clients MUST NOT be given a
 * general subscription grant; Flowie broker auth/ACL must protect it.
 * Events are sanitized by the authoritative Room Owner before DB COMMIT. */
#define MYGAMES_OUTBOX_MQTT_TOPIC "mygames/internal/v1/room-events"

typedef enum mygames_outbox_delivery {
    MYGAMES_OUTBOX_DELIVERY_SETTLED = 0, /* MQTT QoS1 positive PUBACK */
    MYGAMES_OUTBOX_DELIVERY_RETRY,       /* no definitive settlement */
    MYGAMES_OUTBOX_DELIVERY_FATAL
} mygames_outbox_delivery;

typedef mygames_outbox_delivery (*mygames_outbox_publish_fn)(
    void *context, const char *topic,
    const uint8_t *payload, size_t payload_size,
    uint64_t epoch, uint64_t sequence);

typedef enum mygames_outbox_result {
    MYGAMES_OUTBOX_SENT = 0,
    MYGAMES_OUTBOX_EMPTY,
    MYGAMES_OUTBOX_RETRY,
    MYGAMES_OUTBOX_FATAL
} mygames_outbox_result;

/* One event, one settlement. Never ACK a Flowie command merely admitted
 * into a client queue; transport callback must have positively validated
 * MQTT PUBACK for QoS1 first. A timeout/unknown response leaves the database
 * event unpublished for at-least-once replay. A DB ACK failure is fatal.
 *
 * The dispatcher does not create a thread or retain any payload references.
 * Call only from the single, independent publisher worker; the Room Owner is
 * not re-entered from the MQTT callback.
 */
/* Strict canonical allowlist for **publicly distributable** v1 room events.
 * Rejects unknown event types/keys, nested values, escaped payloads, private
 * cards, session tickets and inconsistent sequence/epoch. Event epoch may be
 * older than current DB Owner epoch following a restart. */
bool mygames_outbox_public_event_validate(
    const uint8_t *payload, size_t size,
    uint64_t current_epoch, uint64_t event_sequence);

mygames_outbox_result mygames_outbox_dispatch_one(
    mygames_room_store *store, mygames_outbox_publish_fn publish,
    void *context, uint64_t *out_sequence);

#ifdef __cplusplus
}
#endif
#endif
