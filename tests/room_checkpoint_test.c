#include "mygames/room_checkpoint.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t n) {
    unsigned i;
    for (i = 0u; i < 4u; ++i) p[i] = (uint8_t)(n >> (i * 8u));
}
static uint32_t sum32(const uint8_t *p, size_t n) {
    size_t i;
    uint32_t h = UINT32_C(2166136261);
    for (i = 0u; i < n; ++i) h = (h ^ p[i]) * UINT32_C(16777619);
    return h;
}
static void reseal(uint8_t *b, size_t n) {
    put32(b + 24u, sum32(b + MYGAMES_CHECKPOINT_HEADER_BYTES,
                        n - MYGAMES_CHECKPOINT_HEADER_BYTES));
}
static void test_codec(void) {
    mygames_room_directory original, result, unchanged;
    const mygames_room *a, *b;
    unsigned int seat;
    uint8_t payload[MYGAMES_CHECKPOINT_MAX_BYTES] = {0};
    uint8_t mutation[MYGAMES_CHECKPOINT_MAX_BYTES] = {0};
    uint8_t sentinel[MYGAMES_CHECKPOINT_MAX_BYTES];
    size_t bytes = 0u;
    uint64_t epoch = 999u;

    mygames_room_directory_init(&original);
    assert(mygames_room_directory_create(&original, 101u, &a) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(&original, a->id, 102u, &seat)
           == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(&original, 102u, true) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_create(&original, 201u, &b) == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(&original, b->id, 202u, &seat)
           == MYGAMES_ROOM_OK);

    memset(sentinel, 0xa5, sizeof(sentinel));
    assert(mygames_room_checkpoint_encode(&original, 7u, sentinel, 1u, &bytes)
           == MYGAMES_CHECKPOINT_CAPACITY);
    assert(bytes == MYGAMES_CHECKPOINT_HEADER_BYTES +
                     2u * MYGAMES_CHECKPOINT_ROOM_BYTES);
    assert(sentinel[0] == 0xa5u);
    assert(mygames_room_checkpoint_encode(&original, 7u,
                                          payload, sizeof(payload), &bytes)
           == MYGAMES_CHECKPOINT_OK);
    memset(&result, 0x5a, sizeof(result));
    assert(mygames_room_checkpoint_decode(payload, bytes, &result, &epoch)
           == MYGAMES_CHECKPOINT_OK);
    assert(epoch == 7u && result.next_serial == original.next_serial);
    assert(mygames_room_directory_get_player(&result, 102u)->seats[1].ready);
    assert(mygames_room_directory_get_player(&result, 202u)->players == 2u);
    unchanged = result;
    epoch = 42u;

    memcpy(mutation, payload, bytes);
    mutation[MYGAMES_CHECKPOINT_HEADER_BYTES + 44u] ^= 1u;
    assert(mygames_room_checkpoint_decode(mutation, bytes, &result, &epoch)
           == MYGAMES_CHECKPOINT_CORRUPT);
    assert(memcmp(&result, &unchanged, sizeof(result)) == 0 && epoch == 42u);
    reseal(mutation, bytes); /* digest valid, business owner invariant violated */
    assert(mygames_room_checkpoint_decode(mutation, bytes, &result, &epoch)
           == MYGAMES_CHECKPOINT_CORRUPT);

    memcpy(mutation, payload, bytes);
    mutation[4] = 2u; /* future format must not be guessed */
    assert(mygames_room_checkpoint_decode(mutation, bytes, &result, &epoch)
           == MYGAMES_CHECKPOINT_UNSUPPORTED);
    assert(mygames_room_checkpoint_decode(payload, bytes - 1u, &result, &epoch)
           == MYGAMES_CHECKPOINT_CORRUPT);
    assert(mygames_room_checkpoint_decode(payload, bytes + 1u, &result, &epoch)
           == MYGAMES_CHECKPOINT_CORRUPT);

    memcpy(mutation, payload, bytes);
    {
        const size_t second = MYGAMES_CHECKPOINT_HEADER_BYTES +
                              MYGAMES_CHECKPOINT_ROOM_BYTES;
        mutation[second + 60u] = 101u; /* duplicate player across two rooms */
        mutation[second + 61u] = 0u;
        reseal(mutation, bytes);
        assert(mygames_room_checkpoint_decode(mutation, bytes, &result, &epoch)
               == MYGAMES_CHECKPOINT_CORRUPT);
    }
    assert(memcmp(&result, &unchanged, sizeof(result)) == 0 && epoch == 42u);

    original.entries[0].room.owner = 0u;
    bytes = 999u;
    assert(mygames_room_checkpoint_encode(&original, 8u,
                                          payload, sizeof(payload), &bytes)
           == MYGAMES_CHECKPOINT_INVALID);
    assert(bytes == 999u);
}
int main(void) {
    test_codec();
    puts("mygames.checkpoint: PASS");
    return 0;
}
