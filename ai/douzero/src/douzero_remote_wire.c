#include "mygames/douzero_remote_wire.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "wire Q requires IEEE-754 binary32");

enum { WIRE_REQUEST = 1, WIRE_REPLY = 2, WIRE_ORIGINAL_WP = 1 };

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static void put64(uint8_t *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < 4; ++i) v |= ((uint32_t)p[i]) << (8u * i);
    return v;
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) v |= ((uint64_t)p[i]) << (8u * i);
    return v;
}
static int any_nonzero(const uint8_t *p, size_t n) {
    size_t i;
    uint8_t acc = 0;
    for (i = 0; i < n; ++i) acc |= p[i];
    return acc != 0;
}
static int binary(const int8_t *v, size_t n) {
    size_t i;
    if (!v) return 0;
    for (i = 0; i < n; ++i)
        if (v[i] != 0 && v[i] != 1) return 0;
    return 1;
}
static int valid_ticket(const mygames_douzero_ticket *t) {
    if (!t || !t->request_id || !t->room_epoch || !t->game_id ||
        !t->turn_id || !t->owner_generation || !t->model_version ||
        t->role > 2u || t->total_actions == 0u ||
        t->total_actions > MYGAMES_DOUZERO_REMOTE_TOTAL_ACTIONS_MAX ||
        t->action_count == 0u ||
        t->action_count > MYGAMES_DOUZERO_REMOTE_BATCH_MAX ||
        t->first_action >= t->total_actions ||
        (uint64_t)t->first_action + t->action_count > t->total_actions)
        return 0;
    return any_nonzero(t->legal_actions_sha256, 32u) &&
           any_nonzero(t->model_artifact_sha256, 32u);
}
static size_t state_width(uint8_t role) {
    return role == 0u ? MYGAMES_DOUZERO_REMOTE_STATE_LANDLORD :
                        MYGAMES_DOUZERO_REMOTE_STATE_FARMER;
}
static void write_header(uint8_t *p, const mygames_douzero_ticket *t,
                         uint8_t kind) {
    memset(p, 0, MYGAMES_DOUZERO_REMOTE_HEADER_BYTES);
    p[0] = 'D'; p[1] = 'Z'; p[2] = 'Q'; p[3] = 'I';
    p[4] = MYGAMES_DOUZERO_REMOTE_ABI;
    p[5] = kind;
    p[6] = t->role;
    p[7] = WIRE_ORIGINAL_WP;
    put64(p + 8, t->request_id);
    put64(p + 16, t->room_epoch);
    put64(p + 24, t->game_id);
    put64(p + 32, t->turn_id);
    put64(p + 40, t->owner_generation);
    put64(p + 48, t->model_version);
    put32(p + 56, t->first_action);
    put32(p + 60, t->total_actions);
    put16(p + 64, t->action_count);
    put16(p + 66, (uint16_t)state_width(t->role));
    put16(p + 68, MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES);
    put16(p + 70, MYGAMES_DOUZERO_REMOTE_ACTION_BYTES);
    memcpy(p + 72, t->legal_actions_sha256, 32u);
    memcpy(p + 104, t->model_artifact_sha256, 32u);
}
static int read_header(const uint8_t *p, size_t bytes, uint8_t kind,
                       mygames_douzero_ticket *out) {
    mygames_douzero_ticket t;
    if (!p || bytes < MYGAMES_DOUZERO_REMOTE_HEADER_BYTES ||
        p[0] != 'D' || p[1] != 'Z' || p[2] != 'Q' || p[3] != 'I' ||
        p[4] != MYGAMES_DOUZERO_REMOTE_ABI || p[5] != kind ||
        p[7] != WIRE_ORIGINAL_WP || p[6] > 2u ||
        get16(p + 66) != state_width(p[6]) ||
        get16(p + 68) != MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES ||
        get16(p + 70) != MYGAMES_DOUZERO_REMOTE_ACTION_BYTES)
        return 0;
    memset(&t, 0, sizeof(t));
    t.role = p[6];
    t.request_id = get64(p + 8);
    t.room_epoch = get64(p + 16);
    t.game_id = get64(p + 24);
    t.turn_id = get64(p + 32);
    t.owner_generation = get64(p + 40);
    t.model_version = get64(p + 48);
    t.first_action = get32(p + 56);
    t.total_actions = get32(p + 60);
    t.action_count = get16(p + 64);
    memcpy(t.legal_actions_sha256, p + 72, 32u);
    memcpy(t.model_artifact_sha256, p + 104, 32u);
    if (!valid_ticket(&t)) return 0;
    *out = t;
    return 1;
}
static int ticket_equal(const mygames_douzero_ticket *a,
                        const mygames_douzero_ticket *b) {
    return a->request_id == b->request_id &&
           a->room_epoch == b->room_epoch &&
           a->game_id == b->game_id &&
           a->turn_id == b->turn_id &&
           a->owner_generation == b->owner_generation &&
           a->model_version == b->model_version &&
           a->first_action == b->first_action &&
           a->total_actions == b->total_actions &&
           a->action_count == b->action_count && a->role == b->role &&
           memcmp(a->legal_actions_sha256, b->legal_actions_sha256, 32u) == 0 &&
           memcmp(a->model_artifact_sha256, b->model_artifact_sha256, 32u) == 0;
}

mygames_douzero_wire_status mygames_douzero_wire_encode_request(
    const mygames_douzero_ticket *t, const int8_t *x, size_t x_count,
    const int8_t *z, size_t z_count,
    const int8_t *actions, size_t action_bytes,
    void *output, size_t capacity, size_t *out_size) {
    size_t bytes;
    uint8_t *p = (uint8_t *)output;
    if (!valid_ticket(t) || !output || !out_size ||
        x_count != state_width(t->role) ||
        z_count != MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES ||
        action_bytes != (size_t)t->action_count *
                        MYGAMES_DOUZERO_REMOTE_ACTION_BYTES ||
        !binary(x, x_count) || !binary(z, z_count) ||
        !binary(actions, action_bytes))
        return MYGAMES_DOUZERO_WIRE_INVALID;
    bytes = MYGAMES_DOUZERO_REMOTE_HEADER_BYTES +
            x_count + z_count + action_bytes;
    if (capacity < bytes) return MYGAMES_DOUZERO_WIRE_CAPACITY;
    write_header(p, t, WIRE_REQUEST);
    memcpy(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES, x, x_count);
    memcpy(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + x_count, z, z_count);
    memcpy(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + x_count + z_count,
           actions, action_bytes);
    *out_size = bytes;
    return MYGAMES_DOUZERO_WIRE_OK;
}
mygames_douzero_wire_status mygames_douzero_wire_decode_request(
    const void *frame, size_t size, mygames_douzero_request_view *out) {
    mygames_douzero_ticket ticket;
    mygames_douzero_request_view view;
    const uint8_t *p = (const uint8_t *)frame;
    size_t width, action_bytes;
    if (!out || !read_header(p, size, WIRE_REQUEST, &ticket))
        return MYGAMES_DOUZERO_WIRE_INVALID;
    width = state_width(ticket.role);
    action_bytes = (size_t)ticket.action_count *
                   MYGAMES_DOUZERO_REMOTE_ACTION_BYTES;
    if (size != MYGAMES_DOUZERO_REMOTE_HEADER_BYTES +
                width + MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES + action_bytes)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    view.ticket = ticket;
    view.x = (const int8_t *)(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES);
    view.z = view.x + width;
    view.actions = view.z + MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES;
    if (!binary(view.x, width) ||
        !binary(view.z, MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES) ||
        !binary(view.actions, action_bytes))
        return MYGAMES_DOUZERO_WIRE_INVALID;
    *out = view;
    return MYGAMES_DOUZERO_WIRE_OK;
}
mygames_douzero_wire_status mygames_douzero_wire_encode_reply(
    const mygames_douzero_ticket *ticket, const float *scores,
    size_t score_count, void *out, size_t capacity, size_t *out_size) {
    uint8_t *p = (uint8_t *)out;
    size_t i, size;
    if (!valid_ticket(ticket) || !scores || !out || !out_size ||
        score_count != ticket->action_count)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    for (i = 0; i < score_count; ++i)
        if (!isfinite(scores[i])) return MYGAMES_DOUZERO_WIRE_NONFINITE;
    size = MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + score_count * 4u;
    if (capacity < size) return MYGAMES_DOUZERO_WIRE_CAPACITY;
    write_header(p, ticket, WIRE_REPLY);
    for (i = 0; i < score_count; ++i) {
        uint32_t bits = 0;
        memcpy(&bits, scores + i, sizeof(bits));
        put32(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + i * 4u, bits);
    }
    *out_size = size;
    return MYGAMES_DOUZERO_WIRE_OK;
}
mygames_douzero_wire_status mygames_douzero_wire_decode_reply(
    const void *frame, size_t size, const mygames_douzero_ticket *expected,
    float *scores, size_t capacity) {
    const uint8_t *p = (const uint8_t *)frame;
    mygames_douzero_ticket observed;
    size_t i;
    if (!valid_ticket(expected) || !scores || capacity < expected->action_count)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    if (!read_header(p, size, WIRE_REPLY, &observed) ||
        size != MYGAMES_DOUZERO_REMOTE_HEADER_BYTES +
                (size_t)expected->action_count * 4u)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    if (!ticket_equal(&observed, expected))
        return MYGAMES_DOUZERO_WIRE_STALE;
    /* First pass checks EVERY score; never partially publish to the Owner. */
    for (i = 0; i < expected->action_count; ++i) {
        uint32_t bits = get32(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + i * 4u);
        float value;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) return MYGAMES_DOUZERO_WIRE_NONFINITE;
    }
    for (i = 0; i < expected->action_count; ++i) {
        uint32_t bits = get32(p + MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + i * 4u);
        memcpy(scores + i, &bits, sizeof(bits));
    }
    return MYGAMES_DOUZERO_WIRE_OK;
}


mygames_douzero_wire_status mygames_douzero_score_collector_begin(
    const mygames_douzero_ticket *first, float *scores, size_t capacity,
    mygames_douzero_score_collector *out) {
    mygames_douzero_score_collector tmp;
    uint32_t first_count;
    uint32_t count;
    if (!out || !scores || !valid_ticket(first) ||
        first->first_action != 0u ||
        capacity < (size_t)first->total_actions)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    count = (first->total_actions + 63u) / 64u;
    first_count = first->total_actions < 64u ? first->total_actions : 64u;
    if (count == 0u || count > MYGAMES_DOUZERO_REMOTE_CHUNKS_MAX ||
        first->action_count != first_count ||
        UINT64_MAX - first->request_id < (uint64_t)count - 1u)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    memset(&tmp, 0, sizeof(tmp));
    tmp.initial = *first;
    tmp.scores = scores;
    tmp.score_capacity = capacity;
    tmp.expected_count = (uint16_t)count;
    *out = tmp;
    return MYGAMES_DOUZERO_WIRE_OK;
}

mygames_douzero_wire_status mygames_douzero_score_collector_ticket(
    const mygames_douzero_score_collector *owner, size_t batch,
    mygames_douzero_ticket *out) {
    mygames_douzero_ticket ticket;
    uint32_t remain;
    if (!owner || !out || !owner->scores ||
        !owner->expected_count || batch >= owner->expected_count ||
        owner->score_capacity < owner->initial.total_actions)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    ticket = owner->initial;
    ticket.request_id += (uint64_t)batch;
    ticket.first_action = (uint32_t)batch * 64u;
    remain = ticket.total_actions - ticket.first_action;
    ticket.action_count = (uint16_t)(remain < 64u ? remain : 64u);
    if (!valid_ticket(&ticket))
        return MYGAMES_DOUZERO_WIRE_INVALID;
    *out = ticket;
    return MYGAMES_DOUZERO_WIRE_OK;
}

mygames_douzero_wire_status mygames_douzero_score_collector_accept(
    mygames_douzero_score_collector *owner, const void *reply, size_t n) {
    const uint8_t *p = (const uint8_t *)reply;
    mygames_douzero_ticket expected;
    mygames_douzero_wire_status result;
    float temporary[MYGAMES_DOUZERO_REMOTE_BATCH_MAX];
    uint32_t first;
    size_t batch;
    if (!owner || !owner->scores || !owner->expected_count ||
        !reply || n < MYGAMES_DOUZERO_REMOTE_HEADER_BYTES)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    /* Packet data is UNTRUSTED. Indexing the bitmap or the score storage
     * is forbidden until the offset is checked and the full owner ticket
     * was authenticated against the immutable locally expected batch. */
    first = get32(p + 56u);
    if ((first % 64u) != 0u) return MYGAMES_DOUZERO_WIRE_INVALID;
    batch = first / 64u;
    if (batch >= owner->expected_count)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    if ((owner->received[batch / 8u] & (1u << (batch % 8u))) != 0u)
        return MYGAMES_DOUZERO_WIRE_STALE;
    if (mygames_douzero_score_collector_ticket(owner, batch, &expected)
        != MYGAMES_DOUZERO_WIRE_OK)
        return MYGAMES_DOUZERO_WIRE_INVALID;
    result = mygames_douzero_wire_decode_reply(
        reply, n, &expected, temporary, MYGAMES_DOUZERO_REMOTE_BATCH_MAX);
    if (result != MYGAMES_DOUZERO_WIRE_OK) return result;
    memcpy(owner->scores + first, temporary,
           (size_t)expected.action_count * sizeof(float));
    owner->received[batch / 8u] |= (uint8_t)(1u << (batch % 8u));
    ++owner->received_count;
    return MYGAMES_DOUZERO_WIRE_OK;
}

int mygames_douzero_score_collector_complete(
    const mygames_douzero_score_collector *owner) {
    return owner && owner->scores && owner->expected_count > 0u &&
           owner->received_count == owner->expected_count;
}
