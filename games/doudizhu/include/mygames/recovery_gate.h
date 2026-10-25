#ifndef MYGAMES_RECOVERY_GATE_H
#define MYGAMES_RECOVERY_GATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Pure C11, SDK-independent admission policy for the future recovery service.
 * A caller MUST pass a fingerprint from a verified CHttp/CNet mTLS peer.
 * This module does not authenticate raw TLS, own a database, or serve HTTP. */
#define MYGAMES_RECOVERY_MAX_PRINCIPALS 8u
#define MYGAMES_RECOVERY_NAME_CAPACITY 65u
#define MYGAMES_RECOVERY_CERT_SHA256_CAPACITY 65u
#define MYGAMES_RECOVERY_MAX_PAGE 16u
#define MYGAMES_RECOVERY_MAX_RATE 64u
#define MYGAMES_RECOVERY_MAX_BURST 64u

typedef enum mygames_recovery_gate_status {
    MYGAMES_RECOVERY_GATE_OK = 0,
    MYGAMES_RECOVERY_GATE_INVALID,
    MYGAMES_RECOVERY_GATE_UNAUTHENTICATED,
    MYGAMES_RECOVERY_GATE_FORBIDDEN,
    MYGAMES_RECOVERY_GATE_RATE_LIMITED,
    MYGAMES_RECOVERY_GATE_BUSY,
    MYGAMES_RECOVERY_GATE_STALE
} mygames_recovery_gate_status;

typedef struct mygames_recovery_principal {
    char peer_certificate_sha256[MYGAMES_RECOVERY_CERT_SHA256_CAPACITY];
    char source_namespace[MYGAMES_RECOVERY_NAME_CAPACITY];
    char consumer_id[MYGAMES_RECOVERY_NAME_CAPACITY];
    bool allow_read;
    uint32_t rate_per_second;  /* in [1,64], per process */
    uint32_t burst;            /* in [1,64], per process */
} mygames_recovery_principal;

typedef struct mygames_recovery_grant {
    uint32_t slot; /* private admission identity, not user supplied */
    uint64_t generation;
    uint64_t after_committed_seq; /* request hint, NOT server ACK */
    uint32_t page_limit;
    char source_namespace[MYGAMES_RECOVERY_NAME_CAPACITY];
    char consumer_id[MYGAMES_RECOVERY_NAME_CAPACITY];
} mygames_recovery_grant;

typedef struct mygames_recovery_slot {
    mygames_recovery_principal policy;
    uint64_t remaining_milli_credits;
    uint64_t last_monotonic_ms;
    uint64_t generation;
    bool clock_initialized;
    bool in_flight;
} mygames_recovery_slot;

/* Initialize exactly once on a zeroed/stopped object.
 * Policy is copied; no TLS credentials or file paths are loaded implicitly.
 * Callbacks sharing this gate must run on one owner or provide synchronization.
 * For multi-instance deployments a separate shared rate limit is required. */
typedef struct mygames_recovery_gate {
    mygames_recovery_slot slots[MYGAMES_RECOVERY_MAX_PRINCIPALS];
    size_t count;
    bool initialized;
} mygames_recovery_gate;

mygames_recovery_gate_status mygames_recovery_gate_init(
    mygames_recovery_gate *gate, const mygames_recovery_principal *principals,
    size_t count);

/* Reserve one bounded GET recovery read. Caller MUST authenticate the TLS
 * connection and pass only the framework's verified peer fingerprint.
 * On any failure out_grant is cleared, no reservation is acquired.
 * A successful grant is NOT an event delivery or consumer settlement. */
mygames_recovery_gate_status mygames_recovery_gate_admit_read(
    mygames_recovery_gate *gate, const char *verified_peer_sha256,
    uint64_t after_committed_seq, uint32_t page_limit,
    uint64_t now_monotonic_ms, mygames_recovery_grant *out_grant);

/* Release once on every handler terminal path. Stale/double releases never
 * release a later grant, nor refund consumed rate credits. */
mygames_recovery_gate_status mygames_recovery_gate_release(
    mygames_recovery_gate *gate, const mygames_recovery_grant *grant);
#ifdef __cplusplus
}
#endif
#endif
