#include "mygames/session_registry.h"
#include <string.h>

static void erase_session(mygames_session_record *session) {
    if (!session) return;
    volatile unsigned char *p = (volatile unsigned char *)session->ticket;
    size_t i;
    for (i = 0; i < sizeof(session->ticket); ++i) p[i] = 0;
    memset(session, 0, sizeof(*session));
}

static bool valid_entropy(const uint8_t bytes[MYGAMES_SESSION_TICKET_BYTES]) {
    size_t i;
    uint8_t bits = 0u;
    if (!bytes) return false;
    for (i = 0u; i < MYGAMES_SESSION_TICKET_BYTES; ++i) bits |= bytes[i];
    return bits != 0u;
}

static void hex_encode(char output[MYGAMES_SESSION_TICKET_HEX + 1u],
                       const uint8_t bytes[MYGAMES_SESSION_TICKET_BYTES]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0u; i < MYGAMES_SESSION_TICKET_BYTES; ++i) {
        output[2u * i] = digits[bytes[i] >> 4u];
        output[2u * i + 1u] = digits[bytes[i] & 0x0fu];
    }
    output[MYGAMES_SESSION_TICKET_HEX] = '\0';
}

static bool equals_constant_time(const char *a, const char *b) {
    size_t i;
    unsigned char diff = 0u;
    if (!a || !b) return false;
    for (i = 0; i < MYGAMES_SESSION_TICKET_HEX; ++i)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0u;
}

static bool valid_token(const char *s) {
    size_t i;
    if (!s) return false;
    /* Require canonical 64 lowercase hex bytes before bounded lookup. */
    for (i = 0u; i < MYGAMES_SESSION_TICKET_HEX; ++i) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return s[MYGAMES_SESSION_TICKET_HEX] == '\0';
}

static bool ticket_conflict(const mygames_session_registry *registry,
                            const char *token) {
    size_t i;
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        const mygames_session_record *record = &registry->entries[i];
        if (record->used && equals_constant_time(record->ticket, token))
            return true;
    }
    return false;
}

void mygames_sessions_init(mygames_session_registry *registry) {
    if (registry) memset(registry, 0, sizeof(*registry));
}

const mygames_session_record *mygames_sessions_at(
    const mygames_session_registry *registry, size_t slot) {
    if (!registry || slot >= MYGAMES_SESSION_CAPACITY ||
        !registry->entries[slot].used) return NULL;
    return &registry->entries[slot];
}

mygames_session_status mygames_sessions_issue(
    mygames_session_registry *registry,
    const uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES],
    size_t *out_slot) {
    size_t i;
    char token[MYGAMES_SESSION_TICKET_HEX + 1u];
    if (!registry || !out_slot || !valid_entropy(entropy))
        return MYGAMES_SESSION_INVALID;
    *out_slot = MYGAMES_SESSION_CAPACITY;
    if (registry->next_player == UINT64_MAX) return MYGAMES_SESSION_OVERFLOW;
    hex_encode(token, entropy);
    if (ticket_conflict(registry, token)) return MYGAMES_SESSION_ENTROPY_COLLISION;
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        mygames_session_record *record = &registry->entries[i];
        if (record->used) continue;
        *record = (mygames_session_record){0};
        record->used = true;
        record->attached = true;
        record->player = ++registry->next_player;
        record->generation = 1u;
        memcpy(record->ticket, token, sizeof(token));
        *out_slot = i;
        return MYGAMES_SESSION_OK;
    }
    return MYGAMES_SESSION_FULL;
}

mygames_session_status mygames_sessions_detach(
    mygames_session_registry *registry, size_t slot, uint64_t generation,
    uint64_t now_ms, uint64_t grace_ms) {
    mygames_session_record *record;
    if (!registry || slot >= MYGAMES_SESSION_CAPACITY || !grace_ms)
        return MYGAMES_SESSION_INVALID;
    record = &registry->entries[slot];
    if (!record->used || !record->attached || record->generation != generation)
        return MYGAMES_SESSION_NOT_FOUND;
    if (UINT64_MAX - now_ms < grace_ms) return MYGAMES_SESSION_OVERFLOW;
    record->attached = false;
    record->expires_at_ms = now_ms + grace_ms;
    return MYGAMES_SESSION_OK;
}

mygames_session_status mygames_sessions_resume(
    mygames_session_registry *registry, const char *presented_ticket,
    const uint8_t rotated_entropy[MYGAMES_SESSION_TICKET_BYTES],
    uint64_t now_ms, size_t *out_slot) {
    char new_token[MYGAMES_SESSION_TICKET_HEX + 1u];
    mygames_session_record *match = NULL;
    size_t i, match_slot = MYGAMES_SESSION_CAPACITY;
    if (!registry || !out_slot || !valid_token(presented_ticket) ||
        !valid_entropy(rotated_entropy)) return MYGAMES_SESSION_INVALID;
    *out_slot = MYGAMES_SESSION_CAPACITY;
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        mygames_session_record *record = &registry->entries[i];
        if (record->used && equals_constant_time(record->ticket, presented_ticket)) {
            match = record;
            match_slot = i;
        }
    }
    if (!match) return MYGAMES_SESSION_NOT_FOUND;
    if (match->attached) return MYGAMES_SESSION_ACTIVE;
    if (now_ms >= match->expires_at_ms) return MYGAMES_SESSION_EXPIRED;
    if (match->generation == UINT64_MAX) return MYGAMES_SESSION_OVERFLOW;
    hex_encode(new_token, rotated_entropy);
    if (ticket_conflict(registry, new_token)) return MYGAMES_SESSION_ENTROPY_COLLISION;
    memcpy(match->ticket, new_token, sizeof(new_token));
    match->generation++;
    match->attached = true;
    match->expires_at_ms = 0u;
    *out_slot = match_slot;
    return MYGAMES_SESSION_OK;
}

mygames_session_status mygames_sessions_import_active(
    mygames_session_registry *registry, mygames_player_id player,
    uint64_t generation,
    const uint8_t entropy[MYGAMES_SESSION_TICKET_BYTES],
    size_t *out_slot) {
    char token[MYGAMES_SESSION_TICKET_HEX + 1u];
    size_t i;
    size_t free_slot = MYGAMES_SESSION_CAPACITY;
    if (!registry || !out_slot || player == 0u || generation == 0u ||
        !valid_entropy(entropy)) return MYGAMES_SESSION_INVALID;
    *out_slot = MYGAMES_SESSION_CAPACITY;
    hex_encode(token, entropy);
    if (ticket_conflict(registry, token)) return MYGAMES_SESSION_ENTROPY_COLLISION;
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        if (registry->entries[i].used) {
            if (registry->entries[i].player == player)
                return MYGAMES_SESSION_ACTIVE;
        } else if (free_slot == MYGAMES_SESSION_CAPACITY) free_slot = i;
    }
    if (free_slot == MYGAMES_SESSION_CAPACITY) return MYGAMES_SESSION_FULL;
    {
        mygames_session_record *record = &registry->entries[free_slot];
        *record = (mygames_session_record){0};
        record->used = true;
        record->attached = true;
        record->player = player;
        record->generation = generation;
        memcpy(record->ticket, token, sizeof(token));
    }
    if (registry->next_player < player) registry->next_player = player;
    *out_slot = free_slot;
    return MYGAMES_SESSION_OK;
}

mygames_session_status mygames_sessions_discard(
    mygames_session_registry *registry, size_t slot) {
    if (!registry || slot >= MYGAMES_SESSION_CAPACITY)
        return MYGAMES_SESSION_INVALID;
    if (!registry->entries[slot].used) return MYGAMES_SESSION_NOT_FOUND;
    erase_session(&registry->entries[slot]);
    return MYGAMES_SESSION_OK;
}

size_t mygames_sessions_collect_expired(
    mygames_session_registry *registry, uint64_t now_ms,
    mygames_player_id out_players[MYGAMES_SESSION_CAPACITY]) {
    size_t i, count = 0u;
    if (!registry || !out_players) return 0u;
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        mygames_session_record *record = &registry->entries[i];
        if (!record->used || record->attached || now_ms < record->expires_at_ms)
            continue;
        out_players[count++] = record->player;
        erase_session(record);
    }
    return count;
}
