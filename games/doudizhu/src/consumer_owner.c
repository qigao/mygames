#include "mygames/consumer_owner.h"
#include "mygames/outbox_dispatch.h"
#include "mygames/recovery_wire.h"

#include <cmeta_crypto.h>
#include <salts/error_codes.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SLOT_FREE = 0, SLOT_QUEUED = 1, SLOT_TAKEN = 2 };
enum { SOURCE_MQTT = 1, SOURCE_HISTORY = 2 };
typedef struct owner_event {
    uint64_t generation;
    uint64_t order;
    uint64_t epoch;
    uint64_t sequence;
    size_t payload_size;
    uint8_t payload[MYGAMES_STORE_EVENT_MAX];
    unsigned char state;
    unsigned char source;
} owner_event;
struct mygames_consumer_owner {
    cmeta_mutex_t lock;
    cmeta_thread_affine_state thread;
    mygames_consumer_projection *ledger; /* never owned */
    char source_namespace[MYGAMES_CONSUMER_IDENTIFIER_MAX + 1u];
    char consumer_id[MYGAMES_CONSUMER_IDENTIFIER_MAX + 1u];
    size_t count, mqtt_count;
    uint64_t next_order;
    bool processing;
    bool closed;
    bool quarantined;
    bool fetch_active;
    uint64_t fetch_generation;
    uint64_t fetch_after;
#ifdef MYGAMES_CONSUMER_OWNER_TESTING
    mygames_owner_test_taken_hook test_taken_hook;
    void *test_taken_user;
#endif
    owner_event slots[MYGAMES_OWNER_TOTAL_SLOTS];
};

static bool owner_name_valid(const char *name) {
    size_t i;
    if (!name) return false;
    for (i = 0u; i <= MYGAMES_CONSUMER_IDENTIFIER_MAX; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == 0u) return i > 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-'))
            return false;
    }
    return false;
}
static void owner_clear_event(mygames_consumer_owner *owner,
                              owner_event *record) {
    if (record->state == SLOT_FREE) return;
    if (record->source == SOURCE_MQTT && owner->mqtt_count)
        --owner->mqtt_count;
    if (owner->count) --owner->count;
    memset(record->payload, 0, sizeof(record->payload));
    record->payload_size = 0u;
    record->epoch = 0u;
    record->sequence = 0u;
    record->order = 0u;
    record->source = 0u;
    record->state = SLOT_FREE;
    /* generation deliberately retained for stale-ticket fencing */
}
static bool owner_ready_to_admit(const mygames_consumer_owner *o,
                                  size_t count, size_t new_mqtt) {
    return !o->closed && !o->quarantined &&
           count <= MYGAMES_OWNER_TOTAL_SLOTS - o->count &&
           new_mqtt <= MYGAMES_OWNER_MQTT_SLOTS - o->mqtt_count;
}
static bool owner_free_index(const mygames_consumer_owner *o, size_t *out) {
    size_t i;
    for (i = 0u; i < MYGAMES_OWNER_TOTAL_SLOTS; ++i) {
        if (o->slots[i].state == SLOT_FREE &&
            o->slots[i].generation != UINT64_MAX) {
            *out = i;
            return true;
        }
    }
    return false;
}
static void owner_fill_slot(mygames_consumer_owner *owner, size_t slot,
                            unsigned char source, uint64_t epoch,
                            uint64_t seq, const void *bytes, size_t n,
                            mygames_owner_ticket *ticket) {
    owner_event *e = &owner->slots[slot];
    ++e->generation;
    ++owner->next_order;
    e->state = SLOT_QUEUED;
    e->source = source;
    e->order = owner->next_order;
    e->epoch = epoch;
    e->sequence = seq;
    e->payload_size = n;
    memcpy(e->payload, bytes, n);
    ++owner->count;
    if (source == SOURCE_MQTT) ++owner->mqtt_count;
    if (ticket) {
        ticket->slot = (uint32_t)slot;
        ticket->generation = e->generation;
    }
}

mygames_owner_result mygames_consumer_owner_open(
    mygames_consumer_projection *ledger,
    const char *source_namespace, const char *consumer_id,
    mygames_consumer_owner **out) {
    uint64_t last_seq = 0u, applied_count = 0u;
    mygames_consumer_owner *owner;
    if (!out || !ledger || !owner_name_valid(source_namespace) ||
        !owner_name_valid(consumer_id))
        return MYGAMES_OWNER_INVALID;
    *out = NULL;
    if (mygames_consumer_projection_snapshot(
            ledger, source_namespace, consumer_id, &last_seq,
            &applied_count) != MYGAMES_CONSUMER_APPLIED ||
        last_seq != applied_count)
        return MYGAMES_OWNER_QUARANTINED;
    owner = (mygames_consumer_owner *)calloc(1u, sizeof(*owner));
    if (!owner) return MYGAMES_OWNER_FULL;
    cmeta_mutex_init(&owner->lock);
    if (!owner->lock ||
        cmeta_thread_affine_init(&owner->thread, owner) != SALTS_OK) {
        if (owner->lock) cmeta_mutex_destroy(&owner->lock);
        free(owner);
        return MYGAMES_OWNER_FULL;
    }
    owner->ledger = ledger;
    memcpy(owner->source_namespace, source_namespace,
           strlen(source_namespace) + 1u);
    memcpy(owner->consumer_id, consumer_id, strlen(consumer_id) + 1u);
    *out = owner;
    return MYGAMES_OWNER_OK;
}

mygames_owner_result mygames_consumer_owner_admit_mqtt(
    mygames_consumer_owner *owner, const char *topic,
    const void *payload, size_t payload_size,
    uint64_t source_epoch, uint64_t global_sequence,
    mygames_owner_ticket *out_ticket) {
    size_t slot = 0u;
    mygames_owner_result rc;
    if (out_ticket) memset(out_ticket, 0, sizeof(*out_ticket));
    if (!owner || !topic ||
        strcmp(topic, MYGAMES_OUTBOX_MQTT_TOPIC) != 0 ||
        !payload || !out_ticket || !source_epoch || !global_sequence ||
        global_sequence > (uint64_t)INT64_MAX ||
        payload_size == 0u || payload_size > MYGAMES_STORE_EVENT_MAX)
        return MYGAMES_OWNER_INVALID;
    if (!mygames_outbox_public_event_validate(
            payload, payload_size, source_epoch, global_sequence))
        return MYGAMES_OWNER_BAD_EVENT;
    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) rc = MYGAMES_OWNER_CLOSED;
    else if (owner->quarantined) rc = MYGAMES_OWNER_QUARANTINED;
    else if (!owner_ready_to_admit(owner, 1u, 1u) ||
             !owner_free_index(owner, &slot) ||
             owner->next_order == UINT64_MAX) rc = MYGAMES_OWNER_FULL;
    else {
        owner_fill_slot(owner, slot, SOURCE_MQTT, source_epoch,
                        global_sequence, payload, payload_size, out_ticket);
        rc = MYGAMES_OWNER_OK;
    }
    cmeta_mutex_unlock(&owner->lock);
    return rc;
}

mygames_owner_result mygames_consumer_owner_admit_history(
    mygames_consumer_owner *owner, const void *http_body, size_t http_size,
    uint64_t requested_after_committed_seq,
    mygames_owner_ticket *out_tickets, size_t ticket_capacity,
    size_t *out_admitted) {
    mygames_store_archive_page *page;
    mygames_owner_result rc = MYGAMES_OWNER_INVALID;
    size_t slots[MYGAMES_STORE_ARCHIVE_PAGE_MAX], i, found;
    if (!owner || !http_body || !out_admitted ||
        (out_tickets && ticket_capacity < MYGAMES_STORE_ARCHIVE_PAGE_MAX))
        return MYGAMES_OWNER_INVALID;
    *out_admitted = 0u;
    page = (mygames_store_archive_page *)calloc(1u, sizeof(*page));
    if (!page) return MYGAMES_OWNER_FULL;
    if (mygames_recovery_decode_page(
            http_body, http_size, owner->source_namespace, owner->consumer_id,
            requested_after_committed_seq,
            mygames_outbox_public_event_validate,
            page) != MYGAMES_RECOVERY_WIRE_OK) {
        rc = MYGAMES_OWNER_BAD_EVENT;
        goto done;
    }
    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) rc = MYGAMES_OWNER_CLOSED;
    else if (owner->quarantined) rc = MYGAMES_OWNER_QUARANTINED;
    else if (!owner_ready_to_admit(owner, page->count, 0u) ||
             page->count > MYGAMES_OWNER_TOTAL_SLOTS ||
             page->count > UINT64_MAX - owner->next_order)
        rc = MYGAMES_OWNER_FULL;
    else {
        found = 0u;
        for (i = 0u; i < MYGAMES_OWNER_TOTAL_SLOTS &&
                     found < page->count; ++i)
            if (owner->slots[i].state == SLOT_FREE &&
                owner->slots[i].generation != UINT64_MAX)
                slots[found++] = i;
        if (found != page->count) rc = MYGAMES_OWNER_FULL;
        else {
            /* Atomic with respect to all other ingress and close. */
            for (i = 0u; i < page->count; ++i) {
                const mygames_store_archive_item *item = &page->items[i];
                owner_fill_slot(owner, slots[i], SOURCE_HISTORY, page->epoch,
                    item->sequence, item->payload, item->size,
                    out_tickets ? &out_tickets[i] : NULL);
            }
            *out_admitted = page->count;
            rc = MYGAMES_OWNER_OK;
        }
    }
    cmeta_mutex_unlock(&owner->lock);
done:
    memset(page, 0, sizeof(*page));
    free(page);
    return rc;
}

mygames_owner_result mygames_consumer_owner_begin_fetch(
    mygames_consumer_owner *owner,
    mygames_owner_fetch_ticket *out_ticket) {
    uint64_t cursor = 0u, effects = 0u;
    mygames_consumer_status status;
    mygames_owner_result rc = MYGAMES_OWNER_INVALID;
    if (out_ticket) memset(out_ticket, 0, sizeof(*out_ticket));
    if (!owner || !out_ticket ||
        cmeta_thread_affine_check(&owner->thread, owner) != SALTS_OK)
        return MYGAMES_OWNER_INVALID;

    /* The Owner-creating thread alone reads the durable cursor; the
     * asynchronous HTTP worker only receives the copied bounded ticket. */
    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) rc = MYGAMES_OWNER_CLOSED;
    else if (owner->quarantined) rc = MYGAMES_OWNER_QUARANTINED;
    else if (owner->processing || owner->fetch_active ||
             owner->fetch_generation == UINT64_MAX)
        rc = MYGAMES_OWNER_BUSY;
    else rc = MYGAMES_OWNER_OK;
    cmeta_mutex_unlock(&owner->lock);
    if (rc != MYGAMES_OWNER_OK) return rc;

    status = mygames_consumer_projection_snapshot(
        owner->ledger, owner->source_namespace, owner->consumer_id,
        &cursor, &effects);
    if (status != MYGAMES_CONSUMER_APPLIED ||
        effects != cursor || cursor > (uint64_t)INT64_MAX) {
        cmeta_mutex_lock(&owner->lock);
        owner->quarantined = true;
        cmeta_mutex_unlock(&owner->lock);
        return MYGAMES_OWNER_QUARANTINED;
    }

    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) rc = MYGAMES_OWNER_CLOSED;
    else if (owner->quarantined) rc = MYGAMES_OWNER_QUARANTINED;
    else if (owner->processing || owner->fetch_active ||
             owner->fetch_generation == UINT64_MAX)
        rc = MYGAMES_OWNER_BUSY;
    else {
        ++owner->fetch_generation;
        owner->fetch_active = true;
        owner->fetch_after = cursor;
        out_ticket->generation = owner->fetch_generation;
        out_ticket->committed_after = cursor;
        rc = MYGAMES_OWNER_OK;
    }
    cmeta_mutex_unlock(&owner->lock);
    return rc;
}

mygames_owner_result mygames_consumer_owner_finish_fetch(
    mygames_consumer_owner *owner, mygames_owner_fetch_ticket ticket) {
    mygames_owner_result result;
    if (!owner || !ticket.generation ||
        ticket.committed_after > (uint64_t)INT64_MAX)
        return MYGAMES_OWNER_INVALID;
    cmeta_mutex_lock(&owner->lock);
    if (!owner->fetch_active ||
        ticket.generation != owner->fetch_generation ||
        ticket.committed_after != owner->fetch_after)
        result = MYGAMES_OWNER_STALE;
    else {
        owner->fetch_active = false;
        owner->fetch_after = 0u;
        result = MYGAMES_OWNER_OK;
    }
    cmeta_mutex_unlock(&owner->lock);
    return result;
}

mygames_owner_result mygames_consumer_owner_cancel(
    mygames_consumer_owner *owner, mygames_owner_ticket ticket) {
    mygames_owner_result rc;
    owner_event *event;
    if (!owner || !ticket.generation ||
        ticket.slot >= MYGAMES_OWNER_TOTAL_SLOTS)
        return MYGAMES_OWNER_INVALID;
    cmeta_mutex_lock(&owner->lock);
    event = &owner->slots[ticket.slot];
    if (event->generation != ticket.generation ||
        event->state == SLOT_FREE) rc = MYGAMES_OWNER_STALE;
    else if (event->state == SLOT_TAKEN) rc = MYGAMES_OWNER_BUSY;
    else {
        owner_clear_event(owner, event);
        rc = MYGAMES_OWNER_OK;
    }
    cmeta_mutex_unlock(&owner->lock);
    return rc;
}

mygames_owner_result mygames_consumer_owner_pump_one(
    mygames_consumer_owner *owner, uint64_t *out_committed_cursor) {
    uint64_t current = 0u, count = 0u;
    owner_event selected = {0};
    size_t index = MYGAMES_OWNER_TOTAL_SLOTS, i;
    uint64_t oldest_order = UINT64_MAX;
    bool future_only = false;
    mygames_owner_result rc;
    uint8_t digest[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES] = {0};
    uint64_t committed_effects = 0u;
    mygames_consumer_status st;

    if (!owner || !out_committed_cursor) return MYGAMES_OWNER_INVALID;
    if (cmeta_thread_affine_check(&owner->thread, owner) != SALTS_OK)
        return MYGAMES_OWNER_INVALID;
    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) rc = MYGAMES_OWNER_CLOSED;
    else if (owner->quarantined) rc = MYGAMES_OWNER_QUARANTINED;
    else if (owner->processing) rc = MYGAMES_OWNER_BUSY;
    else {
        owner->processing = true;
        rc = MYGAMES_OWNER_OK;
    }
    cmeta_mutex_unlock(&owner->lock);
    if (rc != MYGAMES_OWNER_OK) return rc;

    st = mygames_consumer_projection_snapshot(owner->ledger,
         owner->source_namespace, owner->consumer_id, &current, &count);
    if (st != MYGAMES_CONSUMER_APPLIED || current != count ||
        current > (uint64_t)INT64_MAX) {
        rc = MYGAMES_OWNER_QUARANTINED;
        goto complete;
    }
    cmeta_mutex_lock(&owner->lock);
    /* Prefer the exact next contiguous seq over any duplicate. Among
     * matching seqs, first admission wins. Future gaps remain buffered. */
    for (i = 0u; i < MYGAMES_OWNER_TOTAL_SLOTS; ++i) {
        owner_event *e = &owner->slots[i];
        if (e->state != SLOT_QUEUED) continue;
        if (e->sequence > current + 1u) {
            future_only = true;
            continue;
        }
        if (e->sequence == current + 1u &&
            (index == MYGAMES_OWNER_TOTAL_SLOTS ||
             selected.sequence != current + 1u ||
             e->order < oldest_order)) {
            index = i;
            oldest_order = e->order;
            selected = *e;
        }
    }
    if (index == MYGAMES_OWNER_TOTAL_SLOTS) {
        oldest_order = UINT64_MAX;
        for (i = 0u; i < MYGAMES_OWNER_TOTAL_SLOTS; ++i) {
            owner_event *e = &owner->slots[i];
            if (e->state == SLOT_QUEUED && e->sequence <= current &&
                e->order < oldest_order) {
                index = i;
                oldest_order = e->order;
                selected = *e;
            }
        }
    }
    if (index == MYGAMES_OWNER_TOTAL_SLOTS) {
        rc = future_only ? MYGAMES_OWNER_GAP : MYGAMES_OWNER_EMPTY;
        owner->processing = false;
        cmeta_mutex_unlock(&owner->lock);
        *out_committed_cursor = current;
        return rc;
    }
    owner->slots[index].state = SLOT_TAKEN;
    cmeta_mutex_unlock(&owner->lock);

#ifdef MYGAMES_CONSUMER_OWNER_TESTING
    /* Slot remains TAKEN and process-level Owner remains live while
     * another thread tests cancel and begin_close. Its hook MUST finish
     * before this sole Owner accesses its borrowed ledger. */
    if (owner->test_taken_hook)
        owner->test_taken_hook(owner->test_taken_user);
#endif

    if (cmeta_sha256(selected.payload, selected.payload_size,
                     digest) != SALTS_OK) {
        rc = MYGAMES_OWNER_QUARANTINED;
        goto complete;
    }
    st = mygames_consumer_projection_apply(
        owner->ledger, owner->source_namespace, owner->consumer_id,
        selected.sequence, digest, &committed_effects);
    if (st == MYGAMES_CONSUMER_APPLIED &&
        committed_effects == selected.sequence) {
        current = selected.sequence;
        rc = MYGAMES_OWNER_APPLIED;
    } else if (st == MYGAMES_CONSUMER_REPLAY &&
               committed_effects == current) {
        rc = MYGAMES_OWNER_REPLAY;
    } else {
        rc = MYGAMES_OWNER_QUARANTINED;
    }
complete:
    memset(digest, 0, sizeof(digest));
    memset(selected.payload, 0, sizeof(selected.payload));
    cmeta_mutex_lock(&owner->lock);
    if (index < MYGAMES_OWNER_TOTAL_SLOTS &&
        owner->slots[index].state == SLOT_TAKEN)
        owner_clear_event(owner, &owner->slots[index]);
    if (rc == MYGAMES_OWNER_QUARANTINED)
        owner->quarantined = true;
    owner->processing = false;
    cmeta_mutex_unlock(&owner->lock);
    if (rc == MYGAMES_OWNER_APPLIED || rc == MYGAMES_OWNER_REPLAY ||
        rc == MYGAMES_OWNER_GAP || rc == MYGAMES_OWNER_EMPTY)
        *out_committed_cursor = current;
    return rc;
}

#ifdef MYGAMES_CONSUMER_OWNER_TESTING
mygames_owner_result mygames_consumer_owner_test_set_taken_hook(
    mygames_consumer_owner *owner, mygames_owner_test_taken_hook hook,
    void *user) {
    if (!owner ||
        cmeta_thread_affine_check(&owner->thread, owner) != SALTS_OK)
        return MYGAMES_OWNER_INVALID;
    cmeta_mutex_lock(&owner->lock);
    if (owner->closed) {
        cmeta_mutex_unlock(&owner->lock);
        return MYGAMES_OWNER_CLOSED;
    }
    if (owner->processing || owner->quarantined) {
        cmeta_mutex_unlock(&owner->lock);
        return MYGAMES_OWNER_BUSY;
    }
    owner->test_taken_hook = hook;
    owner->test_taken_user = user;
    cmeta_mutex_unlock(&owner->lock);
    return MYGAMES_OWNER_OK;
}
#endif

mygames_owner_result mygames_consumer_owner_begin_close(
    mygames_consumer_owner *owner) {
    size_t i;
    bool busy;
    if (!owner) return MYGAMES_OWNER_INVALID;
    cmeta_mutex_lock(&owner->lock);
    owner->closed = true;
    for (i = 0u; i < MYGAMES_OWNER_TOTAL_SLOTS; ++i)
        if (owner->slots[i].state == SLOT_QUEUED)
            owner_clear_event(owner, &owner->slots[i]);
    /* Closing cancels QUEUED work, but never frees the Owner while a
     * native HTTP worker still holds a generation-fenced fetch retain. */
    busy = owner->processing || owner->fetch_active;
    cmeta_mutex_unlock(&owner->lock);
    return busy ? MYGAMES_OWNER_BUSY : MYGAMES_OWNER_OK;
}
mygames_owner_result mygames_consumer_owner_destroy(
    mygames_consumer_owner *owner) {
    mygames_owner_result st;
    if (!owner) return MYGAMES_OWNER_INVALID;
    if (cmeta_thread_affine_check(&owner->thread, owner) != SALTS_OK)
        return MYGAMES_OWNER_INVALID;
    st = mygames_consumer_owner_begin_close(owner);
    if (st != MYGAMES_OWNER_OK) return st;
    (void)cmeta_thread_affine_reset(&owner->thread, owner);
    cmeta_mutex_destroy(&owner->lock);
    memset(owner, 0, sizeof(*owner));
    free(owner);
    return MYGAMES_OWNER_OK;
}
