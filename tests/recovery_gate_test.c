#include "mygames/recovery_gate.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static mygames_recovery_principal principal(char digit,
                                            const char *ns,
                                            const char *consumer,
                                            bool can_read,
                                            uint32_t rate,
                                            uint32_t burst) {
    mygames_recovery_principal p = {0};
    size_t i;
    for (i = 0u; i < 64u; ++i) p.peer_certificate_sha256[i] = digit;
    p.peer_certificate_sha256[64] = '\0';
    (void)snprintf(p.source_namespace, sizeof(p.source_namespace), "%s", ns);
    (void)snprintf(p.consumer_id, sizeof(p.consumer_id), "%s", consumer);
    p.allow_read = can_read;
    p.rate_per_second = rate;
    p.burst = burst;
    return p;
}

int main(void) {
    mygames_recovery_gate gate = {0};
    mygames_recovery_principal policies[2] = {
        principal('a', "trusted-source-v1", "backend-v1", true, 8u, 2u),
        principal('b', "trusted-source-v2", "audit-v1", false, 8u, 1u)
    };
    mygames_recovery_grant first = {0}, second = {0}, denied = {0};
    const char *good = policies[0].peer_certificate_sha256;
    const char *forbidden = policies[1].peer_certificate_sha256;
    const char bad_sha[] =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

    /* Registration is explicit and default deny, no duplicate identity or
     * duplicate source+consumer streams, and no wildcard namespace. */
    assert(mygames_recovery_gate_init(&gate, policies, 2u) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(mygames_recovery_gate_init(&gate, policies, 2u) ==
           MYGAMES_RECOVERY_GATE_INVALID);
    assert(mygames_recovery_gate_admit_read(
               &gate, NULL, 0u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_UNAUTHENTICATED);
    assert(mygames_recovery_gate_admit_read(
               &gate, bad_sha, 0u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_UNAUTHENTICATED);
    assert(mygames_recovery_gate_admit_read(
               &gate, forbidden, 0u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_FORBIDDEN);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 0u, 0u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_INVALID);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 0u, MYGAMES_RECOVERY_MAX_PAGE + 1u,
               1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_INVALID);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, (uint64_t)INT64_MAX + 1u, 1u,
               1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_INVALID);
    assert(denied.generation == 0u); /* no partial admission */

    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 16u, 1000u, &first) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(first.after_committed_seq == 123u && first.page_limit == 16u);
    assert(!strcmp(first.source_namespace, "trusted-source-v1"));
    assert(!strcmp(first.consumer_id, "backend-v1"));
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_BUSY);
    assert(mygames_recovery_gate_release(&gate, &first) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(mygames_recovery_gate_release(&gate, &first) ==
           MYGAMES_RECOVERY_GATE_STALE);

    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1000u, &second) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(second.generation != first.generation);
    assert(mygames_recovery_gate_release(&gate, &first) ==
           MYGAMES_RECOVERY_GATE_STALE);
    /* Stale/double release cannot refund or release second reservation. */
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_BUSY);
    assert(mygames_recovery_gate_release(&gate, &second) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1000u, &denied) ==
           MYGAMES_RECOVERY_GATE_RATE_LIMITED);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1124u, &denied) ==
           MYGAMES_RECOVERY_GATE_RATE_LIMITED);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1125u, &second) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(mygames_recovery_gate_release(&gate, &second) ==
           MYGAMES_RECOVERY_GATE_OK);
    /* A backwards monotonic clock fails closed; long leaps cap the burst. */
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, 1124u, &denied) ==
           MYGAMES_RECOVERY_GATE_INVALID);
    assert(mygames_recovery_gate_admit_read(
               &gate, good, 123u, 1u, UINT64_MAX, &second) ==
           MYGAMES_RECOVERY_GATE_OK);
    assert(mygames_recovery_gate_release(&gate, &second) ==
           MYGAMES_RECOVERY_GATE_OK);

    {
        mygames_recovery_gate bad_gate = {0};
        mygames_recovery_principal bad[2];
        bad[0] = policies[0];
        bad[1] = policies[0];
        assert(mygames_recovery_gate_init(&bad_gate, bad, 2u) ==
               MYGAMES_RECOVERY_GATE_INVALID);
        bad[1].peer_certificate_sha256[0] = 'c';
        assert(mygames_recovery_gate_init(&bad_gate, bad, 2u) ==
               MYGAMES_RECOVERY_GATE_INVALID); /* same stream */
        bad[1].source_namespace[0] = '*';
        assert(mygames_recovery_gate_init(&bad_gate, bad, 2u) ==
               MYGAMES_RECOVERY_GATE_INVALID); /* wildcard */
        bad[1] = policies[1];
        bad[1].burst = 0u;
        assert(mygames_recovery_gate_init(&bad_gate, bad, 2u) ==
               MYGAMES_RECOVERY_GATE_INVALID);
    }
    puts("mygames.recovery_gate: PASS");
    return 0;
}
