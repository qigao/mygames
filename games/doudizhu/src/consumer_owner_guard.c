#include "mygames/consumer_owner_guard.h"
#include <stddef.h>
#include <string.h>

/* The pathname is a durable NAME for an OS-held advisory lock, not state:
 * no cursor, generation, credential, or event is written to it. Never unlink
 * this file on release: unlink/recreate could admit another inode while an
 * older Owner still holds the previous inode's lock. */
mygames_consumer_guard_status mygames_consumer_owner_guard_acquire(
    const char *consumer_database_path, mygames_consumer_owner_guard *out) {
    static const char suffix[] = ".m43-owner";
    char lock_path[SALTS_FS_MAX_PATH];
    size_t n;
    cmeta_file_t fd;
    int result;

    if (!out) return MYGAMES_CONSUMER_GUARD_INVALID;
    out->fd = SALTS_INVALID_FILE;
    out->acquired = false;
    if (!consumer_database_path || !consumer_database_path[0] ||
        strcmp(consumer_database_path, ":memory:") == 0)
        return MYGAMES_CONSUMER_GUARD_INVALID;

    n = strlen(consumer_database_path);
    if (n + sizeof(suffix) > sizeof(lock_path))
        return MYGAMES_CONSUMER_GUARD_INVALID;
    memcpy(lock_path, consumer_database_path, n);
    memcpy(lock_path + n, suffix, sizeof(suffix));

    /* An owner must reside on a trusted local filesystem with cooperative
     * advisory lock semantics. Failure to obtain exclusive admission never
     * falls back to an unlocked projection. */
    fd = cmeta_fs_open(lock_path, SALTS_FS_O_RDWR | SALTS_FS_O_CREAT, 0600);
    if (fd == SALTS_INVALID_FILE)
        return MYGAMES_CONSUMER_GUARD_UNAVAILABLE;
    result = cmeta_fs_lock(fd, SALTS_FS_LOCK_EXCLUSIVE |
                           SALTS_FS_LOCK_NONBLOCK, 0, 1u);
    if (result != 0) {
        (void)cmeta_fs_close(fd);
        return MYGAMES_CONSUMER_GUARD_UNAVAILABLE;
    }
    out->fd = fd;
    out->acquired = true;
    return MYGAMES_CONSUMER_GUARD_ACQUIRED;
}

void mygames_consumer_owner_guard_release(mygames_consumer_owner_guard *guard) {
    if (!guard || !guard->acquired) return;
    /* Closing also releases the advisory lock if the process is healthy.
     * SIGKILL bypasses this function, which is the real OS-release test. */
    (void)cmeta_fs_unlock(guard->fd, 0, 1u);
    (void)cmeta_fs_close(guard->fd);
    guard->fd = SALTS_INVALID_FILE;
    guard->acquired = false;
}
