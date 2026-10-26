#include "mygames/recovery_gate.h"
#include <limits.h>
#include <string.h>

/* Deliberately accepts only ASCII names already approved by the service
 * provisioner; never treats a request/query/body as a principal source. */
static bool safe_name(const char *text, size_t cap) {
    size_t i;
    if (!text) return false;
    for (i = 0u; i < cap; ++i) {
        const unsigned char ch = (unsigned char)text[i];
        if (!ch) return i != 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-' || ch == ':'))
            return false;
    }
    return false;
}

static bool fingerprint_valid(const char *text) {
    size_t i;
    if (!text) return false;
    for (i = 0u; i < MYGAMES_RECOVERY_CERT_SHA256_CAPACITY - 1u; ++i) {
        const unsigned char ch = (unsigned char)text[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return text[MYGAMES_RECOVERY_CERT_SHA256_CAPACITY - 1u] == '\0';
}

/* Compare full public fingerprints without leaking first-differing offset. */
static bool same_fingerprint(const char *left, const char *right) {
    unsigned int diff = 0u;
    size_t i;
    for (i = 0u; i < MYGAMES_RECOVERY_CERT_SHA256_CAPACITY - 1u; ++i)
        diff |= (unsigned int)((unsigned char)left[i] ^ (unsigned char)right[i]);
    return diff == 0u;
}

mygames_recovery_gate_status mygames_recovery_gate_init(
    mygames_recovery_gate *gate, const mygames_recovery_principal *principals,
    size_t count) {
    size_t i, j;
    if (!gate || gate->initialized || !principals || !count ||
        count > MYGAMES_RECOVERY_MAX_PRINCIPALS)
        return MYGAMES_RECOVERY_GATE_INVALID;
    for (i = 0u; i < count; ++i) {
        const mygames_recovery_principal *p = &principals[i];
        if (!fingerprint_valid(p->peer_certificate_sha256) ||
            !safe_name(p->source_namespace, sizeof(p->source_namespace)) ||
            !safe_name(p->consumer_id, sizeof(p->consumer_id)) ||
            !p->rate_per_second ||
            p->rate_per_second > MYGAMES_RECOVERY_MAX_RATE ||
            !p->burst || p->burst > MYGAMES_RECOVERY_MAX_BURST)
            return MYGAMES_RECOVERY_GATE_INVALID;
        for (j = 0u; j < i; ++j) {
            const mygames_recovery_principal *prev = &principals[j];
            /* One reader owner per durable consumer stream; no different
             * certs may race the same identity through parallel gates. */
            if (same_fingerprint(p->peer_certificate_sha256,
                                 prev->peer_certificate_sha256) ||
                (strcmp(p->source_namespace, prev->source_namespace) == 0 &&
                 strcmp(p->consumer_id, prev->consumer_id) == 0))
                return MYGAMES_RECOVERY_GATE_INVALID;
        }
    }
    memset(gate, 0, sizeof(*gate));
    for (i = 0u; i < count; ++i) {
        gate->slots[i].policy = principals[i];
        gate->slots[i].remaining_milli_credits =
            (uint64_t)principals[i].burst * UINT64_C(1000);
    }
    gate->count = count;
    gate->initialized = true;
    return MYGAMES_RECOVERY_GATE_OK;
}

mygames_recovery_gate_status mygames_recovery_gate_admit_read(
    mygames_recovery_gate *gate, const char *verified_peer_sha256,
    uint64_t after_committed_seq, uint32_t page_limit,
    uint64_t now_monotonic_ms, mygames_recovery_grant *out_grant) {
    mygames_recovery_slot *slot = NULL;
    uint64_t cap, elapsed, refill;
    size_t i;
    if (!out_grant) return MYGAMES_RECOVERY_GATE_INVALID;
    memset(out_grant, 0, sizeof(*out_grant));
    if (!gate || !gate->initialized || !page_limit ||
        page_limit > MYGAMES_RECOVERY_MAX_PAGE ||
        after_committed_seq > (uint64_t)INT64_MAX)
        return MYGAMES_RECOVERY_GATE_INVALID;
    /* Only a verified CHttp/CNet certificate leaf digest qualifies.
     * Missing/malformed digests fail before the archive is touched. */
    if (!fingerprint_valid(verified_peer_sha256))
        return MYGAMES_RECOVERY_GATE_UNAUTHENTICATED;
    for (i = 0u; i < gate->count; ++i) {
        if (same_fingerprint(
                verified_peer_sha256,
                gate->slots[i].policy.peer_certificate_sha256)) {
            slot = &gate->slots[i];
            break;
        }
    }
    if (!slot) return MYGAMES_RECOVERY_GATE_UNAUTHENTICATED;
    if (!slot->policy.allow_read) return MYGAMES_RECOVERY_GATE_FORBIDDEN;
    if (slot->in_flight) return MYGAMES_RECOVERY_GATE_BUSY;
    if (!slot->clock_initialized) {
        slot->clock_initialized = true;
        slot->last_monotonic_ms = now_monotonic_ms;
    }
    if (now_monotonic_ms < slot->last_monotonic_ms)
        return MYGAMES_RECOVERY_GATE_INVALID;
    elapsed = now_monotonic_ms - slot->last_monotonic_ms;
    cap = (uint64_t)slot->policy.burst * UINT64_C(1000);
    /* Saturate before multiplication to avoid overflow on a distant
     * monotonic time jump (e.g. resumed laptop / test clock). */
    if (elapsed >= (cap + slot->policy.rate_per_second - 1u) /
                    slot->policy.rate_per_second) {
        slot->remaining_milli_credits = cap;
    } else if (elapsed != 0u) {
        refill = elapsed * slot->policy.rate_per_second;
        if (refill > cap - slot->remaining_milli_credits)
            slot->remaining_milli_credits = cap;
        else
            slot->remaining_milli_credits += refill;
    }
    slot->last_monotonic_ms = now_monotonic_ms;
    if (slot->remaining_milli_credits < UINT64_C(1000))
        return MYGAMES_RECOVERY_GATE_RATE_LIMITED;
    if (slot->generation == UINT64_MAX)
        return MYGAMES_RECOVERY_GATE_BUSY;
    slot->remaining_milli_credits -= UINT64_C(1000);
    ++slot->generation;
    slot->in_flight = true;
    out_grant->slot = (uint32_t)i;
    out_grant->generation = slot->generation;
    out_grant->after_committed_seq = after_committed_seq;
    out_grant->page_limit = page_limit;
    memcpy(out_grant->source_namespace, slot->policy.source_namespace,
           sizeof(out_grant->source_namespace));
    memcpy(out_grant->consumer_id, slot->policy.consumer_id,
           sizeof(out_grant->consumer_id));
    return MYGAMES_RECOVERY_GATE_OK;
}

mygames_recovery_gate_status mygames_recovery_gate_release(
    mygames_recovery_gate *gate, const mygames_recovery_grant *grant) {
    mygames_recovery_slot *slot;
    if (!gate || !gate->initialized || !grant || grant->slot >= gate->count)
        return MYGAMES_RECOVERY_GATE_INVALID;
    slot = &gate->slots[grant->slot];
    if (!slot->in_flight || !grant->generation ||
        grant->generation != slot->generation)
        return MYGAMES_RECOVERY_GATE_STALE;
    slot->in_flight = false;
    return MYGAMES_RECOVERY_GATE_OK;
}
