#ifndef MYGAMES_SESSION_REGISTRY_H
#define MYGAMES_SESSION_REGISTRY_H
#include "mygames/room.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYGAMES_SESSION_CAPACITY 64u
#define MYGAMES_SESSION_TICKET_BYTES 32u
#define MYGAMES_SESSION_TICKET_HEX 64u

typedef enum mygames_session_status {
    MYGAMES_SESSION_OK = 0,
    MYGAMES_SESSION_INVALID,
    MYGAMES_SESSION_FULL,
    MYGAMES_SESSION_NOT_FOUND,
    MYGAMES_SESSION_ACTIVE,
    MYGAMES_SESSION_EXPIRED,
    MYGAMES_SESSION_ENTROPY_COLLISION,
    MYGAMES_SESSION_OVERFLOW
} mygames_session_status;

typedef struct mygames_session_record {
    bool used;
    bool attached;
    mygames_player_id player;
    uint64_t generation;
    uint64_t expires_at_ms; /* meaningful only while detached */
    char ticket[MYGAMES_SESSION_TICKET_HEX + 1u];
} mygames_session_record;

typedef struct mygames_session_registry {
    uint64_t next_player;
    mygames_session_record entries[MYGAMES_SESSION_CAPACITY];
} mygames_session_registry;

/**
 * Single-owner, fixed-capacity, in-memory recovery capabilities.
 *
 * A ticket is 256 bits supplied by a trusted CSPRNG caller; the core never
 * fabricates random bytes. Tokens MUST be transported only over authenticated
 * WSS in a future production service. This developmental registry is NOT user
 * authentication or durable session storage.
 */
void mygames_sessions_init(mygames_session_registry *registry);
const mygames_session_record *mygames_sessions_at(
    const mygames_session_registry *registry, size_t slot);

mygames_session_status mygames_sessions_issue(
    mygames_session_registry *registry,
    const uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES],
    size_t *out_slot);

mygames_session_status mygames_sessions_detach(
    mygames_session_registry *registry, size_t slot, uint64_t generation,
    uint64_t now_ms, uint64_t grace_ms);

mygames_session_status mygames_sessions_resume(
    mygames_session_registry *registry,
    const char *presented_ticket,
    const uint8_t rotated_entropy[MYGAMES_SESSION_TICKET_BYTES],
    uint64_t now_ms,
    size_t *out_slot);

/* Discard the caller's newly issued unused session on successful resume.
 * The caller must verify that this player is not seated in a room. */
/* Import a DB-verified recovered identity into a fresh, still in-memory
 * connection registry. The server first verifies and rotates the credential
 * in the durable authority; the caller supplies only a fresh CSPRNG token.
 * Refuses duplicate player IDs or ticket collisions, and leaves registry
 * unchanged on failure. This is not an external user authentication API.
 */
mygames_session_status mygames_sessions_import_active(
    mygames_session_registry *registry, mygames_player_id player,
    uint64_t generation,
    const uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES],
    size_t *out_slot);

mygames_session_status mygames_sessions_discard(
    mygames_session_registry *registry, size_t slot);

/**
 * Reap timed-out detached sessions (not active sessions). Returns the former
 * player identities so the SAME Room Owner can remove them from game state.
 * This is one atomic owner-only call, not an autonomous timer.
 */
size_t mygames_sessions_collect_expired(
    mygames_session_registry *registry, uint64_t now_ms,
    mygames_player_id out_players[MYGAMES_SESSION_CAPACITY]);

#ifdef __cplusplus
}
#endif
#endif
