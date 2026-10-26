#include "mygames/room_checkpoint.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static void store64(uint8_t *p, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8u; ++i) p[i] = (uint8_t)(value >> (i * 8u));
}
static uint64_t load64(const uint8_t *p) {
    uint64_t v = 0u;
    unsigned i;
    for (i = 0; i < 8u; ++i) v |= (uint64_t)p[i] << (i * 8u);
    return v;
}
static void store32(uint8_t *p, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4u; ++i) p[i] = (uint8_t)(value >> (i * 8u));
}
static uint32_t load32(const uint8_t *p) {
    uint32_t v = 0u;
    unsigned i;
    for (i = 0; i < 4u; ++i) v |= (uint32_t)p[i] << (i * 8u);
    return v;
}
static uint32_t crc_weak(const uint8_t *p, size_t length) {
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0u; i < length; ++i)
        h = (h ^ p[i]) * UINT32_C(16777619);
    return h;
}
static bool id_valid(const char *id, uint64_t serial_ceiling) {
    uint64_t number = 0u;
    size_t i;
    if (memcmp(id, "room-", 5u) != 0 || id[5] < '1' || id[5] > '9')
        return false;
    for (i = 5u; i < MYGAMES_ROOM_ID_CAPACITY; ++i) {
        unsigned char ch = (unsigned char)id[i];
        if (!ch) {
            size_t j;
            for (j = i + 1u; j < MYGAMES_ROOM_ID_CAPACITY; ++j)
                if (id[j]) return false;
            return number != 0u && number <= serial_ceiling;
        }
        if (ch < '0' || ch > '9' ||
            number > (UINT64_MAX - (uint64_t)(ch - '0')) / 10u)
            return false;
        number = 10u * number + (uint64_t)(ch - '0');
    }
    return false;
}
static bool validate(const mygames_room_directory *d, unsigned int *count) {
    unsigned int i, j, k, num = 0u;
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        const mygames_room_directory_entry *e = &d->entries[i];
        const mygames_room *r = &e->room;
        bool owner_seen = false;
        unsigned int actual = 0u;
        if (!e->used) continue;
        ++num;
        if (!id_valid(r->id, d->next_serial) ||
            (r->phase != MYGAMES_ROOM_WAITING &&
             r->phase != MYGAMES_ROOM_STARTING) ||
            r->players < 1u || r->players > MYGAMES_ROOM_SEATS ||
            r->revision == 0u || r->owner == 0u) return false;
        for (j = 0u; j < MYGAMES_ROOM_SEATS; ++j) {
            const mygames_room_seat *seat = &r->seats[j];
            unsigned int t;
            if (!seat->player) { if (seat->ready) return false; continue; }
            ++actual;
            if (seat->player == r->owner) owner_seen = true;
            for (t = 0u; t < j; ++t)
                if (r->seats[t].player == seat->player) return false;
            for (k = 0u; k < i; ++k) {
                const mygames_room_directory_entry *prev = &d->entries[k];
                if (!prev->used) continue;
                for (t = 0u; t < MYGAMES_ROOM_SEATS; ++t)
                    if (prev->room.seats[t].player == seat->player) return false;
            }
        }
        for (k = 0u; k < i; ++k)
            if (d->entries[k].used && strcmp(d->entries[k].room.id, r->id) == 0)
                return false;
        if (actual != r->players || !owner_seen) return false;
    }
    *count = num;
    return true;
}
mygames_checkpoint_status mygames_room_checkpoint_encode(
    const mygames_room_directory *source, uint64_t room_epoch,
    void *output, size_t capacity, size_t *out_bytes) {
    uint8_t scratch[MYGAMES_CHECKPOINT_MAX_BYTES] = {0};
    unsigned int count, i, j;
    size_t required, position = MYGAMES_CHECKPOINT_HEADER_BYTES;
    if (!source || !out_bytes || !room_epoch ||
        !validate(source, &count)) return MYGAMES_CHECKPOINT_INVALID;
    required = MYGAMES_CHECKPOINT_HEADER_BYTES +
               (size_t)count * MYGAMES_CHECKPOINT_ROOM_BYTES;
    if (capacity < required) {
        *out_bytes = required;
        return MYGAMES_CHECKPOINT_CAPACITY;
    }
    if (!output) return MYGAMES_CHECKPOINT_INVALID;
    memcpy(scratch, "MGR1", 4u);
    scratch[4] = 1u;
    scratch[5] = (uint8_t)count;
    store64(scratch + 8u, source->next_serial);
    store64(scratch + 16u, room_epoch);
    for (i = 0u; i < MYGAMES_ROOM_DIRECTORY_CAPACITY; ++i) {
        const mygames_room_directory_entry *e = &source->entries[i];
        const mygames_room *r = &e->room;
        uint8_t *p;
        unsigned ready = 0u;
        if (!e->used) continue;
        p = scratch + position;
        memcpy(p, r->id, strlen(r->id));
        p[40] = (uint8_t)r->phase;
        p[41] = (uint8_t)r->players;
        store64(p + 44u, r->owner);
        store64(p + 52u, r->revision);
        for (j = 0u; j < MYGAMES_ROOM_SEATS; ++j) {
            store64(p + 60u + 8u * j, r->seats[j].player);
            if (r->seats[j].ready) ready |= 1u << j;
        }
        p[84] = (uint8_t)ready;
        position += MYGAMES_CHECKPOINT_ROOM_BYTES;
    }
    if (position != required) return MYGAMES_CHECKPOINT_INVALID;
    store32(scratch + 24u, crc_weak(scratch + MYGAMES_CHECKPOINT_HEADER_BYTES,
                                   required - MYGAMES_CHECKPOINT_HEADER_BYTES));
    memcpy(output, scratch, required);
    *out_bytes = required;
    return MYGAMES_CHECKPOINT_OK;
}
mygames_checkpoint_status mygames_room_checkpoint_decode(
    const void *data, size_t size,
    mygames_room_directory *destination, uint64_t *out_epoch) {
    mygames_room_directory temp = {0};
    const uint8_t *p = (const uint8_t *)data;
    uint64_t epoch;
    unsigned int i, j, count, verified;
    if (!p || !destination || !out_epoch ||
        size < MYGAMES_CHECKPOINT_HEADER_BYTES) return MYGAMES_CHECKPOINT_INVALID;
    if (memcmp(p, "MGR1", 4u) || p[4] != 1u)
        return MYGAMES_CHECKPOINT_UNSUPPORTED;
    count = p[5];
    if (p[6] || p[7] || load32(p + 28u) ||
        count > MYGAMES_ROOM_DIRECTORY_CAPACITY ||
        size != MYGAMES_CHECKPOINT_HEADER_BYTES +
                (size_t)count * MYGAMES_CHECKPOINT_ROOM_BYTES)
        return MYGAMES_CHECKPOINT_CORRUPT;
    epoch = load64(p + 16u);
    if (!epoch || load32(p + 24u) != crc_weak(
        p + MYGAMES_CHECKPOINT_HEADER_BYTES, size - MYGAMES_CHECKPOINT_HEADER_BYTES))
        return MYGAMES_CHECKPOINT_CORRUPT;
    temp.next_serial = load64(p + 8u);
    for (i = 0u; i < count; ++i) {
        const uint8_t *row = p + MYGAMES_CHECKPOINT_HEADER_BYTES +
                             (size_t)i * MYGAMES_CHECKPOINT_ROOM_BYTES;
        mygames_room_directory_entry *e = &temp.entries[i];
        mygames_room *r = &e->room;
        if (row[42] || row[43] || row[85] || row[86] || row[87] ||
            (row[84] & 0xf8u)) return MYGAMES_CHECKPOINT_CORRUPT;
        memcpy(r->id, row, MYGAMES_ROOM_ID_CAPACITY);
        r->phase = (mygames_room_phase)row[40];
        r->players = row[41];
        r->owner = load64(row + 44u);
        r->revision = load64(row + 52u);
        for (j = 0u; j < MYGAMES_ROOM_SEATS; ++j) {
            r->seats[j].player = load64(row + 60u + 8u * j);
            r->seats[j].ready = (row[84] & (1u << j)) != 0u;
        }
        e->used = true;
    }
    if (!validate(&temp, &verified) || verified != count)
        return MYGAMES_CHECKPOINT_CORRUPT;
    *destination = temp;
    *out_epoch = epoch;
    return MYGAMES_CHECKPOINT_OK;
}
