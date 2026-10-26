#ifndef MYGAMES_CONSUMER_OWNER_H
#define MYGAMES_CONSUMER_OWNER_H

#include "mygames/consumer_projection.h"
#include "mygames/room_store.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* First M4.4 in-process admission slice: one Owner thread is the ONLY writer
 * of ConsumerProjection. MPMC callback ingress copies bounded event bytes.
 *
 * The Flowie native MQTT QoS1 callback can offer a copied public event, but
 * its return/queue acceptance is NOT a durable ACK! Flowie #22 must implement
 * genuine deferred inbound disposition before any production no-loss claim.
 *
 * 8 MQTT slots + enough reserved capacity for a complete 16-event history
 * page, all static and finite. No borrowed CHttp or Flowie buffers survive.
 */
#define MYGAMES_OWNER_TOTAL_SLOTS 24u
#define MYGAMES_OWNER_MQTT_SLOTS 8u

typedef struct mygames_consumer_owner mygames_consumer_owner;

typedef struct mygames_owner_ticket {
    uint32_t slot; /* zero-based; generation is nonzero on valid grants */
    uint64_t generation;
} mygames_owner_ticket;

typedef enum mygames_owner_result {
    MYGAMES_OWNER_OK = 0,       /* ingress accepted, NOT persisted */
    MYGAMES_OWNER_APPLIED,      /* owner saw positive durable COMMIT */
    MYGAMES_OWNER_REPLAY,       /* same prior durable digest, no new effect */
    MYGAMES_OWNER_EMPTY,        /* no ready work */
    MYGAMES_OWNER_GAP,          /* cannot skip missing committed sequence */
    MYGAMES_OWNER_FULL,         /* bounded admission; never implicit retry */
    MYGAMES_OWNER_CLOSED,       /* closed/quiescing */
    MYGAMES_OWNER_STALE,        /* stale or already retired/cancelled ticket */
    MYGAMES_OWNER_BUSY,         /* item TAKEN or Owner already processing */
    MYGAMES_OWNER_INVALID,      /* bad argument/topic/Owner thread */
    MYGAMES_OWNER_BAD_EVENT,    /* invalid canonical event or page */
    MYGAMES_OWNER_QUARANTINED   /* ledger conflict, COMMIT_UNKNOWN, failure */
} mygames_owner_result;

/* Ledger remains caller-owned and must outlive the Owner. Open/destroy/pump
 * must be called from the same Owner thread; concurrent ingress/cancel/close
 * are protected by Salts::Platform mutex. This is a local logical stream
 * Owner, not a cross-host generation fence or durable lease. */
mygames_owner_result mygames_consumer_owner_open(
    mygames_consumer_projection *ledger,
    const char *source_namespace, const char *consumer_id,
    mygames_consumer_owner **out);

/* Intended Flowie callback adapter: exact internal topic, canonical public
 * epoch/seq, copied bytes. A positive result is QUEUE ADMISSION ONLY.
 * The caller must not interpret it as ACK-after-TurboDB-COMMIT. */
mygames_owner_result mygames_consumer_owner_admit_mqtt(
    mygames_consumer_owner *owner, const char *topic,
    const void *payload, size_t payload_size,
    uint64_t source_epoch, uint64_t global_sequence,
    mygames_owner_ticket *out_ticket);

/* Only call after a verified CHttp/TLS HTTP 200. The response is strictly
 * parsed using fixed namespace/consumer IDs and the *durable* GET-after
 * cursor the caller originally used; atomic all-or-nothing batch admission.
 * The Owner processes each event only from the main Owner pump.
 * out_tickets must hold MYGAMES_STORE_ARCHIVE_PAGE_MAX entries if non-NULL.
 * No durable cursor is advanced by this method. */
mygames_owner_result mygames_consumer_owner_admit_history(
    mygames_consumer_owner *owner, const void *http_body, size_t http_size,
    uint64_t requested_after_committed_seq,
    mygames_owner_ticket *out_tickets, size_t ticket_capacity,
    size_t *out_admitted);

/* Cancel affects QUEUED only. TAKEN might already be committing; return
 * BUSY and let the sole Owner observe terminal state. Stale tickets cannot
 * cancel a newer occupant reusing a slot. */
mygames_owner_result mygames_consumer_owner_cancel(
    mygames_consumer_owner *owner, mygames_owner_ticket ticket);

/* Sole Owner-thread pump. Picks the next contiguous event (or a prior
 * duplicate for receipt verification) irrespective of arrival order/source.
 * Only this call touches the borrowed ConsumerProjection. A future seq
 * remains buffered and reports GAP, NEVER auto-skipped. */
mygames_owner_result mygames_consumer_owner_pump_one(
    mygames_consumer_owner *owner, uint64_t *out_committed_cursor);

/* Concurrent close cancels queued slots. In-flight COMMIT may finish but
 * never use freed storage. Destroy returns BUSY until pump is quiescent and
 * must run on the original Owner thread. The application must stop/join
 * producers before final destroy; it never frees active callback users. */
mygames_owner_result mygames_consumer_owner_begin_close(
    mygames_consumer_owner *owner);
mygames_owner_result mygames_consumer_owner_destroy(
    mygames_consumer_owner *owner);

#ifdef MYGAMES_CONSUMER_OWNER_TESTING
/* Test-only Owner-thread hook after SLOT_TAKEN has been published under
 * lock, and BEFORE the borrowed ledger is accessed for SHA256/COMMIT.
 * Must NOT be compiled into production/installed ConsumerOwner ABI.
 * Allows a separate callback thread to overlap cancel+begin_close with
 * a real TurboDB commit without sleeps/racy readiness guesses. */
typedef void (*mygames_owner_test_taken_hook)(void *user);
mygames_owner_result mygames_consumer_owner_test_set_taken_hook(
    mygames_consumer_owner *owner,
    mygames_owner_test_taken_hook hook, void *user);
#endif

#ifdef __cplusplus
}
#endif
#endif
