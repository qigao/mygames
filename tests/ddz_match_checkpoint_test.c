#include "mygames/ddz_match_checkpoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(check) do { if (!(check)) { \
    fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #check); \
    exit(1); \
} } while (0)

static uint32_t checksum(const unsigned char *bytes) {
    uint32_t result = UINT32_C(2166136261);
    size_t i;
    for (i = 0u; i < MYGAMES_DDZ_CHECKPOINT_V2_BYTES; ++i) {
        const unsigned char x = (i >= 28u && i < 32u) ? 0u : bytes[i];
        result = (result ^ (uint32_t)x) * UINT32_C(16777619);
    }
    return result;
}

static void rewrite_checksum(unsigned char *bytes) {
    uint32_t value = checksum(bytes);
    size_t i;
    for (i = 0u; i < 4u; ++i)
        bytes[28u + i] = (unsigned char)(value >> (i * 8u));
}

static mygames_room started_room(void) {
    mygames_room room = {0};
    unsigned int seat = 99u;
    REQUIRE(mygames_room_init(&room, "m5c-game", 101u) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_join(&room, 202u, &seat) == MYGAMES_ROOM_OK && seat == 1u);
    REQUIRE(mygames_room_join(&room, 303u, &seat) == MYGAMES_ROOM_OK && seat == 2u);
    REQUIRE(mygames_room_set_ready(&room, 101u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(&room, 202u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_set_ready(&room, 303u, true) == MYGAMES_ROOM_OK);
    REQUIRE(mygames_room_start(&room, 101u) == MYGAMES_ROOM_OK);
    return room;
}

static void roundtrip(const mygames_ddz_match *source) {
    unsigned char encoded[MYGAMES_DDZ_CHECKPOINT_V2_BYTES] = {0};
    unsigned char reencoded[MYGAMES_DDZ_CHECKPOINT_V2_BYTES] = {0};
    unsigned char contaminated[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    mygames_ddz_match restored = {0}, sentinel = {0}, candidate;
    size_t bytes = 12345u, reread = 0u;
    REQUIRE(mygames_ddz_match_validate(source) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_checkpoint_encode(source, encoded, sizeof(encoded), &bytes) ==
            MYGAMES_DDZ_CHECKPOINT_OK);
    REQUIRE(bytes == MYGAMES_DDZ_CHECKPOINT_V2_BYTES);
    REQUIRE(memcmp(encoded, "MGDG", 4u) == 0 && encoded[4] == 2u);
    REQUIRE(encoded[6] == 0u && encoded[7] == 0u);
    REQUIRE(encoded[8] == (unsigned char)(source->room_epoch & 255u));
    REQUIRE(encoded[16] == (unsigned char)(source->game_id & 255u));
    REQUIRE(mygames_ddz_checkpoint_decode(encoded, bytes, &restored) ==
            MYGAMES_DDZ_CHECKPOINT_OK);
    REQUIRE(mygames_ddz_match_validate(&restored) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(mygames_ddz_checkpoint_encode(&restored, reencoded,
                                          sizeof(reencoded), &reread) ==
            MYGAMES_DDZ_CHECKPOINT_OK);
    REQUIRE(reread == bytes && memcmp(encoded, reencoded, bytes) == 0);

    memset(contaminated, 0x5a, sizeof(contaminated));
    bytes = 12345u;
    REQUIRE(mygames_ddz_checkpoint_encode(source, contaminated,
        sizeof(contaminated) - 1u, &bytes) == MYGAMES_DDZ_CHECKPOINT_CAPACITY);
    REQUIRE(bytes == 12345u);
    { size_t i; for (i = 0u; i < sizeof(contaminated); ++i)
        REQUIRE(contaminated[i] == 0x5au); }
    sentinel.game_id = UINT64_C(0x1234);
    sentinel.room_epoch = UINT64_C(0xabcd);
    candidate = sentinel;
    REQUIRE(mygames_ddz_checkpoint_decode(encoded, sizeof(encoded) - 1u,
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    REQUIRE(memcmp(&candidate, &sentinel, sizeof(sentinel)) == 0);
    REQUIRE(mygames_ddz_checkpoint_decode(encoded, sizeof(encoded) + 1u,
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    REQUIRE(memcmp(&candidate, &sentinel, sizeof(sentinel)) == 0);

    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[0] = 'X';
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_UNSUPPORTED);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[4] = 3u;
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_UNSUPPORTED);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[7] = 1u; rewrite_checksum(contaminated);
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[MYGAMES_DDZ_CHECKPOINT_V2_BYTES - 1u] = 1u;
    rewrite_checksum(contaminated);
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[32u + 24u] = contaminated[32u + 25u]; /* Duplicated physical card. */
    rewrite_checksum(contaminated);
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[5] = 255u; rewrite_checksum(contaminated);
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    memcpy(contaminated, encoded, sizeof(contaminated));
    contaminated[110u] ^= 0x1u; /* First private hand no longer matches deck. */
    rewrite_checksum(contaminated);
    REQUIRE(mygames_ddz_checkpoint_decode(contaminated, sizeof(contaminated),
        &candidate) == MYGAMES_DDZ_CHECKPOINT_CORRUPT);
    REQUIRE(memcmp(&candidate, &sentinel, sizeof(sentinel)) == 0);
}

int main(void) {
    mygames_room room = started_room();
    mygames_ddz_match start = {0}, next = {0}, bogus = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result action = {0};
    unsigned char encoded[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    size_t bytes = 77u;
    uint64_t hand, first;
    REQUIRE(mygames_ddz_match_start(&start, &room, 42u, 513u) ==
            MYGAMES_DDZ_MATCH_OK);
    roundtrip(&start); /* Hidden kitty still absent from public view. */
    REQUIRE(mygames_ddz_match_bid(&start, 42u, 513u, 101u, 1u, 3u,
                                  &next, &bid) == MYGAMES_DDZ_MATCH_OK);
    REQUIRE(next.phase == MYGAMES_DDZ_MATCH_PLAYING);
    roundtrip(&next); /* Physical 20/17/17 hands and winner are private. */
    REQUIRE(mygames_ddz_match_private_hand(&next, 42u, 513u, 101u, &hand) ==
            MYGAMES_DDZ_MATCH_OK);
    first = hand & (~hand + UINT64_C(1));
    start = next;
    REQUIRE(mygames_ddz_match_play(&start, 42u, 513u, 101u, 1u, first,
                                   &next, &action) == MYGAMES_DDZ_MATCH_OK);
    roundtrip(&next); /* The original physical card was retired by the Owner. */

    REQUIRE(mygames_ddz_checkpoint_encode(&bogus, encoded, sizeof(encoded), &bytes) ==
            MYGAMES_DDZ_CHECKPOINT_INVALID);
    REQUIRE(bytes == 77u);
    puts("mygames.ddz_match_checkpoint: PASS");
    return 0;
}
