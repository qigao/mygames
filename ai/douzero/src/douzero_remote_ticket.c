#include "mygames/douzero_remote_ticket.h"

#include <math.h>
#include <string.h>

enum ticket_phase {
    PHASE_EMPTY = 0,
    PHASE_COLLECTING = 1,
    PHASE_READY = 2,
    PHASE_CONSUMED = 3,
    PHASE_CANCELLED = 4
};

static uint32_t chunks_needed(uint32_t total) {
    return (total + (MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS - 1u)) /
           MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS;
}

static bool has_digest(const uint8_t digest[MYGAMES_DOUZERO_REMOTE_SHA256_BYTES]) {
    unsigned i;
    uint8_t any = 0u;
    for (i = 0u; i < MYGAMES_DOUZERO_REMOTE_SHA256_BYTES; ++i)
        any |= digest[i];
    return any != 0u;
}

static bool valid_identity(const mygames_douzero_remote_identity *id) {
    unsigned offset, role;
    uint32_t chunks;
    if (!id || id->protocol_version != MYGAMES_DOUZERO_REMOTE_PROTOCOL_V1 ||
        id->rules_profile != MYGAMES_DOUZERO_REMOTE_PROFILE_ORIGINAL_WP_V1 ||
        !id->ticket_id || !id->wire_request_id_base ||
        !id->room_generation || !id->owner_epoch || !id->transport_generation ||
        !id->game_id || !id->turn_id || !id->model_version ||
        id->legal_action_count < 1u ||
        id->legal_action_count > MYGAMES_DOUZERO_REMOTE_MAX_ACTIONS ||
        id->seat >= 3u || id->landlord_seat >= 3u || id->role > 2u ||
        !has_digest(id->model_sha256) ||
        !has_digest(id->legal_list_sha256))
        return false;
    offset = ((unsigned)id->seat + 3u - (unsigned)id->landlord_seat) % 3u;
    role = offset == 0u ? 0u : (offset == 1u ? 2u : 1u);
    if (id->role != role) return false;
    chunks = chunks_needed(id->legal_action_count);
    return id->wire_request_id_base <= UINT64_MAX - (uint64_t)(chunks - 1u);
}

static bool same_identity(const mygames_douzero_remote_identity *a,
                          const mygames_douzero_remote_identity *b) {
    return a && b &&
        a->protocol_version == b->protocol_version &&
        a->rules_profile == b->rules_profile &&
        a->ticket_id == b->ticket_id &&
        a->wire_request_id_base == b->wire_request_id_base &&
        a->room_generation == b->room_generation &&
        a->owner_epoch == b->owner_epoch &&
        a->transport_generation == b->transport_generation &&
        a->game_id == b->game_id && a->turn_id == b->turn_id &&
        a->model_version == b->model_version &&
        a->legal_action_count == b->legal_action_count &&
        a->seat == b->seat && a->landlord_seat == b->landlord_seat &&
        a->role == b->role &&
        memcmp(a->model_sha256, b->model_sha256,
               MYGAMES_DOUZERO_REMOTE_SHA256_BYTES) == 0 &&
        memcmp(a->legal_list_sha256, b->legal_list_sha256,
               MYGAMES_DOUZERO_REMOTE_SHA256_BYTES) == 0;
}

static mygames_douzero_remote_status live(
    const mygames_douzero_remote_ticket *ticket, uint64_t now_ms) {
    if (!ticket) return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (ticket->phase == PHASE_CANCELLED)
        return MYGAMES_DOUZERO_REMOTE_CANCELLED;
    if (ticket->phase == PHASE_CONSUMED)
        return MYGAMES_DOUZERO_REMOTE_REPLAY;
    if (ticket->phase != PHASE_COLLECTING && ticket->phase != PHASE_READY)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (now_ms >= ticket->absolute_deadline_ms)
        return MYGAMES_DOUZERO_REMOTE_DEADLINE;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_begin(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_identity *identity,
    float *score_scratch, size_t score_capacity,
    uint64_t absolute_deadline_ms, uint64_t now_ms) {
    mygames_douzero_remote_ticket next = {0};
    if (!ticket || !identity || !score_scratch ||
        !valid_identity(identity) || !absolute_deadline_ms ||
        absolute_deadline_ms <= now_ms)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (ticket->phase != PHASE_EMPTY) return MYGAMES_DOUZERO_REMOTE_BUSY;
    if (score_capacity < identity->legal_action_count)
        return MYGAMES_DOUZERO_REMOTE_CAPACITY;
    next.identity = *identity;
    next.score_scratch = score_scratch;
    next.score_capacity = score_capacity;
    next.absolute_deadline_ms = absolute_deadline_ms;
    next.phase = PHASE_COLLECTING;
    *ticket = next;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_expected_chunk(
    const mygames_douzero_remote_ticket *ticket, uint64_t now_ms,
    uint64_t *out_wire_request_id, uint32_t *out_chunk_index,
    uint32_t *out_chunk_count, size_t *out_score_count) {
    uint32_t count;
    size_t offset, remaining;
    mygames_douzero_remote_status status = live(ticket, now_ms);
    if (status != MYGAMES_DOUZERO_REMOTE_OK) return status;
    if (!out_wire_request_id || !out_chunk_index ||
        !out_chunk_count || !out_score_count)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (ticket->phase != PHASE_COLLECTING)
        return MYGAMES_DOUZERO_REMOTE_REPLAY;
    count = chunks_needed(ticket->identity.legal_action_count);
    if (ticket->next_chunk >= count) return MYGAMES_DOUZERO_REMOTE_REPLAY;
    offset = (size_t)ticket->next_chunk *
             MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS;
    remaining = (size_t)ticket->identity.legal_action_count - offset;
    *out_wire_request_id = ticket->identity.wire_request_id_base +
                           ticket->next_chunk;
    *out_chunk_index = ticket->next_chunk;
    *out_chunk_count = count;
    *out_score_count = remaining < MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS ?
                       remaining : MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_accept(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_reply_chunk *reply, uint64_t now_ms) {
    uint64_t wire;
    uint32_t index, count;
    size_t needed, offset, i;
    mygames_douzero_remote_status status = live(ticket, now_ms);
    if (status != MYGAMES_DOUZERO_REMOTE_OK) return status;
    if (!reply || !reply->scores || ticket->phase != PHASE_COLLECTING)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    status = mygames_douzero_remote_expected_chunk(
        ticket, now_ms, &wire, &index, &count, &needed);
    if (status != MYGAMES_DOUZERO_REMOTE_OK) return status;
    if (!same_identity(&ticket->identity, &reply->identity) ||
        reply->wire_request_id != wire || reply->chunk_index != index ||
        reply->chunk_count != count || reply->score_count != needed)
        return MYGAMES_DOUZERO_REMOTE_STALE;
    offset = (size_t)index * MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS;
    if (offset + needed > ticket->score_capacity)
        return MYGAMES_DOUZERO_REMOTE_CAPACITY;
    /* Check ALL scores before writing ANY caller output. */
    for (i = 0u; i < needed; ++i)
        if (!isfinite(reply->scores[i]))
            return MYGAMES_DOUZERO_REMOTE_NONFINITE;
    memcpy(ticket->score_scratch + offset, reply->scores,
           needed * sizeof(float));
    ++ticket->next_chunk;
    if (ticket->next_chunk == count)
        ticket->phase = PHASE_READY;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_best_index(
    mygames_douzero_remote_ticket *ticket,
    const mygames_douzero_remote_identity *current_owner,
    uint64_t now_ms, size_t *out_index) {
    size_t i, best = 0u;
    mygames_douzero_remote_status status = live(ticket, now_ms);
    if (status != MYGAMES_DOUZERO_REMOTE_OK) return status;
    if (!out_index || !current_owner)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (!same_identity(&ticket->identity, current_owner))
        return MYGAMES_DOUZERO_REMOTE_STALE;
    if (ticket->phase != PHASE_READY)
        return MYGAMES_DOUZERO_REMOTE_INCOMPLETE;
    /* Scratch is a borrowed Owner buffer, no other thread may alter it.
     * Recheck finiteness at consumption: no speculative response applies. */
    for (i = 0u; i < ticket->identity.legal_action_count; ++i)
        if (!isfinite(ticket->score_scratch[i]))
            return MYGAMES_DOUZERO_REMOTE_NONFINITE;
    for (i = 1u; i < ticket->identity.legal_action_count; ++i)
        if (ticket->score_scratch[i] > ticket->score_scratch[best])
            best = i;
    *out_index = best;
    ticket->phase = PHASE_CONSUMED;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_cancel(
    mygames_douzero_remote_ticket *ticket) {
    size_t i, filled;
    if (!ticket || ticket->phase == PHASE_EMPTY)
        return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (ticket->phase == PHASE_CANCELLED)
        return MYGAMES_DOUZERO_REMOTE_CANCELLED;
    if (ticket->phase == PHASE_CONSUMED)
        return MYGAMES_DOUZERO_REMOTE_REPLAY;
    filled = (size_t)ticket->next_chunk *
             MYGAMES_DOUZERO_REMOTE_CHUNK_ACTIONS;
    if (filled > ticket->identity.legal_action_count)
        filled = ticket->identity.legal_action_count;
    for (i = 0u; i < filled; ++i)
        ticket->score_scratch[i] = 0.0f;
    ticket->phase = PHASE_CANCELLED;
    return MYGAMES_DOUZERO_REMOTE_OK;
}

mygames_douzero_remote_status mygames_douzero_remote_reset(
    mygames_douzero_remote_ticket *ticket) {
    if (!ticket) return MYGAMES_DOUZERO_REMOTE_INVALID;
    if (ticket->phase != PHASE_CANCELLED &&
        ticket->phase != PHASE_CONSUMED)
        return MYGAMES_DOUZERO_REMOTE_BUSY;
    memset(ticket, 0, sizeof(*ticket));
    return MYGAMES_DOUZERO_REMOTE_OK;
}
