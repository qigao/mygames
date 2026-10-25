#include "mygames/outbox_dispatch.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* Hard payload envelope gate: the only v1 producers generate room-prefixed
 * canonical JSON. Do not turn this into a generic MQTT JSON relay.
 * Broker ACL must still enforce service-only consumption. */
static bool allowable_public_room_event(const uint8_t *payload, size_t len) {
    static const char kind[] = "\"type\":\"room.";
    static const char room[] = "\"room_id\":\"";
    static const char sequence[] = "\"seq\":";
    size_t i;
    bool type_found = false, room_found = false, seq_found = false;
    if (!payload || len < 45u || len > MYGAMES_STORE_EVENT_MAX ||
        payload[0] != '{' || payload[len - 1u] != '}')
        return false;
    for (i = 0u; i < len; ++i)
        if (payload[i] == 0u || payload[i] < 0x20u || payload[i] > 0x7eu)
            return false;
    for (i = 0u; i + sizeof(kind) - 1u <= len; ++i)
        if (memcmp(payload + i, kind, sizeof(kind) - 1u) == 0)
            type_found = true;
    for (i = 0u; i + sizeof(room) - 1u <= len; ++i)
        if (memcmp(payload + i, room, sizeof(room) - 1u) == 0)
            room_found = true;
    for (i = 0u; i + sizeof(sequence) - 1u <= len; ++i)
        if (memcmp(payload + i, sequence, sizeof(sequence) - 1u) == 0)
            seq_found = true;
    return type_found && room_found && seq_found;
}

mygames_outbox_result mygames_outbox_dispatch_one(
    mygames_room_store *store, mygames_outbox_publish_fn publish,
    void *context, uint64_t *out_sequence) {
    mygames_room_directory rooms;
    uint8_t message[MYGAMES_STORE_EVENT_MAX];
    size_t message_size = 0u;
    uint64_t epoch = 0u, current_sequence = 0u, event_sequence = 0u;
    mygames_store_status status;
    mygames_outbox_delivery delivered;
    if (!store || !publish) return MYGAMES_OUTBOX_FATAL;
    status = mygames_room_store_load(store, &rooms, &epoch, &current_sequence);
    if (status != MYGAMES_STORE_OK || !epoch) return MYGAMES_OUTBOX_FATAL;
    status = mygames_room_store_next_event(
        store, 0u, message, sizeof(message), &message_size, &event_sequence);
    if (status == MYGAMES_STORE_NOT_FOUND) return MYGAMES_OUTBOX_EMPTY;
    if (status != MYGAMES_STORE_OK || event_sequence == 0u ||
        event_sequence > current_sequence ||
        !allowable_public_room_event(message, message_size))
        return MYGAMES_OUTBOX_FATAL;
    delivered = publish(context, MYGAMES_OUTBOX_MQTT_TOPIC,
                        message, message_size, epoch, event_sequence);
    if (delivered == MYGAMES_OUTBOX_DELIVERY_RETRY)
        return MYGAMES_OUTBOX_RETRY;
    if (delivered != MYGAMES_OUTBOX_DELIVERY_SETTLED)
        return MYGAMES_OUTBOX_FATAL;
    /* Broker QoS1 PUBACK is a positive delivery settlement. Outbox ACK is
     * a separate DB transaction, never a Game ACK. DB commit unknown is fatal.
     * If this ACK fails, next publisher invocation may duplicate the MQTT
     * event; consumers MUST deduplicate by its event ID/sequence. */
    status = mygames_room_store_ack_event(store, epoch, event_sequence);
    if (status != MYGAMES_STORE_OK) return MYGAMES_OUTBOX_FATAL;
    if (out_sequence) *out_sequence = event_sequence;
    return MYGAMES_OUTBOX_SENT;
}
