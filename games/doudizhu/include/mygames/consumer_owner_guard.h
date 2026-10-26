#ifndef MYGAMES_CONSUMER_OWNER_GUARD_H
#define MYGAMES_CONSUMER_OWNER_GUARD_H
#include <stdbool.h>
#include <cmeta_fs.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Single-host, cooperating-PROCESS admission only. This is a bounded test
 * protection for the remote recovery process, not a distributed lease, not
 * a thread mutex, not a durable owner epoch and NOT Flowie ACK authority.
 *
 * The process holds one advisory exclusive lock on ledger.db.m43-owner
 * throughout TLS GET, validation, transactional effects and shutdown.
 * On SIGKILL, the operating system releases the lock automatically.
 * Only use a trusted, privately owned ledger directory: OS advisory locks
 * cannot protect against an attacker swapping/deleting the sidecar path.
 *
 * Future production Owner arbitration must also include live MQTT ingress
 * and durable generation fencing for multi-host failover (MyGames #8).
 */
typedef struct mygames_consumer_owner_guard {
    cmeta_file_t fd;
    bool acquired;
} mygames_consumer_owner_guard;

typedef enum mygames_consumer_guard_status {
    MYGAMES_CONSUMER_GUARD_ACQUIRED = 0,
    MYGAMES_CONSUMER_GUARD_INVALID,
    MYGAMES_CONSUMER_GUARD_UNAVAILABLE
} mygames_consumer_guard_status;

mygames_consumer_guard_status mygames_consumer_owner_guard_acquire(
    const char *consumer_database_path, mygames_consumer_owner_guard *out);

void mygames_consumer_owner_guard_release(
    mygames_consumer_owner_guard *guard);
#ifdef __cplusplus
}
#endif
#endif
