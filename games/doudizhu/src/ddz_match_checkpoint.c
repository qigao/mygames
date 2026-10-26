#include "mygames/ddz_match_checkpoint.h"
#include <string.h>

enum { DDZ_CKPT_HEADER_BYTES = 32, DDZ_CKPT_PAYLOAD_BYTES = 184 };

static void put_u32(unsigned char *buffer, size_t at, uint32_t value) {
    size_t i;
    for (i = 0u; i < 4u; ++i)
        buffer[at + i] = (unsigned char)(value >> (i * 8u));
}
static uint32_t get_u32(const unsigned char *buffer, size_t at) {
    uint32_t value = 0u;
    size_t i;
    for (i = 0u; i < 4u; ++i)
        value |= (uint32_t)buffer[at + i] << (i * 8u);
    return value;
}
static void put_u64(unsigned char *buffer, size_t *position, uint64_t value) {
    size_t i;
    for (i = 0u; i < 8u; ++i)
        buffer[(*position)++] = (unsigned char)(value >> (i * 8u));
}
static uint64_t get_u64(const unsigned char *buffer, size_t *position) {
    uint64_t value = UINT64_C(0);
    size_t i;
    for (i = 0u; i < 8u; ++i)
        value |= (uint64_t)buffer[(*position)++] << (i * 8u);
    return value;
}

static uint32_t canonical_checksum(const unsigned char *data) {
    uint32_t hash = UINT32_C(2166136261);
    size_t i;
    /* Checksum itself is normalized to four zeros, not skipped. */
    for (i = 0u; i < MYGAMES_DDZ_CHECKPOINT_V1_BYTES; ++i) {
        const unsigned char value = (i >= 28u && i < 32u) ? 0u : data[i];
        hash = (hash ^ (uint32_t)value) * UINT32_C(16777619);
    }
    return hash;
}

mygames_ddz_checkpoint_status mygames_ddz_checkpoint_encode(
    const mygames_ddz_match *source,
    void *out_bytes, size_t capacity, size_t *out_size) {
    unsigned char image[MYGAMES_DDZ_CHECKPOINT_V1_BYTES] = {0};
    size_t offset = DDZ_CKPT_HEADER_BYTES;
    size_t i;
    const mygames_ddz_round *round;
    if (!source || !out_bytes || !out_size)
        return MYGAMES_DDZ_CHECKPOINT_INVALID;
    if (mygames_ddz_match_validate(source) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_CHECKPOINT_INVALID;
    if (capacity < sizeof(image)) return MYGAMES_DDZ_CHECKPOINT_CAPACITY;

    image[0] = 'M'; image[1] = 'G'; image[2] = 'D'; image[3] = 'G';
    image[4] = 1u;
    image[5] = (unsigned char)source->phase;
    put_u64(image, &(size_t){8u}, source->room_epoch);
    put_u64(image, &(size_t){16u}, source->game_id);
    put_u32(image, 24u, DDZ_CKPT_PAYLOAD_BYTES);

    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        put_u64(image, &offset, source->players[i]);
    memcpy(image + offset, source->secret_deck, MYGAMES_DDZ_CARDS);
    offset += MYGAMES_DDZ_CARDS;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        put_u64(image, &offset, source->initial_hands[i]);
    put_u64(image, &offset, source->secret_kitty);

    round = &source->round;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        put_u64(image, &offset, round->hands[i]);
    put_u64(image, &offset, round->played_cards);
    put_u64(image, &offset, round->last_cards);
    put_u64(image, &offset, round->turn_id);
    image[offset++] = (unsigned char)round->last_move.kind;
    image[offset++] = round->last_move.high_rank;
    image[offset++] = round->last_move.chain_length;
    image[offset++] = round->last_move.card_count;
    memcpy(image + offset, round->remaining, MYGAMES_DDZ_SEATS);
    offset += MYGAMES_DDZ_SEATS;
    image[offset++] = round->landlord_seat;
    image[offset++] = round->turn_seat;
    image[offset++] = round->last_seat;
    image[offset++] = round->passes;
    image[offset++] = round->winner_seat;
    image[offset++] = round->finished ? 1u : 0u;

    put_u64(image, &offset, source->bid_turn_id);
    image[offset++] = source->bid_start_seat;
    image[offset++] = source->bid_turn_seat;
    image[offset++] = source->bid_count;
    image[offset++] = source->highest_bid;
    image[offset++] = source->highest_seat;
    if (offset != sizeof(image)) return MYGAMES_DDZ_CHECKPOINT_INVALID;
    put_u32(image, 28u, canonical_checksum(image));
    memcpy(out_bytes, image, sizeof(image));
    *out_size = sizeof(image);
    return MYGAMES_DDZ_CHECKPOINT_OK;
}

mygames_ddz_checkpoint_status mygames_ddz_checkpoint_decode(
    const void *bytes, size_t size, mygames_ddz_match *out_match) {
    mygames_ddz_match next = {0};
    const unsigned char *image = (const unsigned char *)bytes;
    mygames_ddz_round *round = &next.round;
    size_t offset = DDZ_CKPT_HEADER_BYTES, i;
    if (!bytes || !out_match) return MYGAMES_DDZ_CHECKPOINT_INVALID;
    if (size != MYGAMES_DDZ_CHECKPOINT_V1_BYTES)
        return MYGAMES_DDZ_CHECKPOINT_CORRUPT;
    if (memcmp(image, "MGDG", 4u) != 0)
        return MYGAMES_DDZ_CHECKPOINT_UNSUPPORTED;
    if (image[4] != 1u)
        return MYGAMES_DDZ_CHECKPOINT_UNSUPPORTED;
    if (image[6] != 0u || image[7] != 0u ||
        get_u32(image, 24u) != DDZ_CKPT_PAYLOAD_BYTES ||
        get_u32(image, 28u) != canonical_checksum(image))
        return MYGAMES_DDZ_CHECKPOINT_CORRUPT;
    next.phase = (mygames_ddz_match_phase)image[5];
    i = 8u; next.room_epoch = get_u64(image, &i);
    i = 16u; next.game_id = get_u64(image, &i);
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        next.players[i] = get_u64(image, &offset);
    memcpy(next.secret_deck, image + offset, MYGAMES_DDZ_CARDS);
    offset += MYGAMES_DDZ_CARDS;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        next.initial_hands[i] = get_u64(image, &offset);
    next.secret_kitty = get_u64(image, &offset);
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        round->hands[i] = get_u64(image, &offset);
    round->played_cards = get_u64(image, &offset);
    round->last_cards = get_u64(image, &offset);
    round->turn_id = get_u64(image, &offset);
    round->last_move.kind = (mygames_ddz_move_kind)image[offset++];
    round->last_move.high_rank = image[offset++];
    round->last_move.chain_length = image[offset++];
    round->last_move.card_count = image[offset++];
    memcpy(round->remaining, image + offset, MYGAMES_DDZ_SEATS);
    offset += MYGAMES_DDZ_SEATS;
    round->landlord_seat = image[offset++];
    round->turn_seat = image[offset++];
    round->last_seat = image[offset++];
    round->passes = image[offset++];
    round->winner_seat = image[offset++];
    if (image[offset] > 1u) return MYGAMES_DDZ_CHECKPOINT_CORRUPT;
    round->finished = image[offset++] != 0u;
    next.bid_turn_id = get_u64(image, &offset);
    next.bid_start_seat = image[offset++];
    next.bid_turn_seat = image[offset++];
    next.bid_count = image[offset++];
    next.highest_bid = image[offset++];
    next.highest_seat = image[offset++];
    if (offset != MYGAMES_DDZ_CHECKPOINT_V1_BYTES ||
        mygames_ddz_match_validate(&next) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_DDZ_CHECKPOINT_CORRUPT;
    *out_match = next;
    return MYGAMES_DDZ_CHECKPOINT_OK;
}
