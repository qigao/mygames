#include "mygames/douzero_remote_ticket.h"
#include "mygames/ddz_round.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
                 exit(1); } \
} while (0)

#define OK MYGAMES_DOUZERO_REMOTE_OK
#define INVALID MYGAMES_DOUZERO_REMOTE_INVALID
#define STALE MYGAMES_DOUZERO_REMOTE_STALE
#define REPLAY MYGAMES_DOUZERO_REMOTE_REPLAY

static mygames_douzero_remote_identity owner_identity(void) {
    mygames_douzero_remote_identity id = {0};
    unsigned i;
    id.protocol_version = MYGAMES_DOUZERO_REMOTE_PROTOCOL_V1;
    id.rules_profile = MYGAMES_DOUZERO_REMOTE_PROFILE_ORIGINAL_WP_V1;
    id.ticket_id = UINT64_C(101);
    id.wire_request_id_base = UINT64_C(9001);
    id.room_generation = 4u;
    id.owner_epoch = 8u;
    id.transport_generation = 12u;
    id.game_id = UINT64_C(21);
    id.turn_id = UINT64_C(3);
    id.model_version = UINT64_C(222);
    id.legal_action_count = 130u; /* 3 remote chunks: 64,64,2 */
    id.seat = 2u;
    id.landlord_seat = 0u;
    id.role = 1u; /* landlord's counterclockwise farmer */
    for (i = 0u; i < MYGAMES_DOUZERO_REMOTE_SHA256_BYTES; ++i) {
        id.model_sha256[i] = (uint8_t)(i + 1u);
        id.legal_list_sha256[i] = (uint8_t)(255u - i);
    }
    return id;
}

static mygames_douzero_remote_reply_chunk valid_reply(
    const mygames_douzero_remote_identity *id, uint32_t index,
    const float *values, size_t size) {
    mygames_douzero_remote_reply_chunk result = {0};
    result.identity = *id;
    result.wire_request_id = id->wire_request_id_base + index;
    result.chunk_index = index;
    result.chunk_count = 3u;
    result.score_count = size;
    result.scores = values;
    return result;
}

static void test_begin_atomic(void) {
    float scores[130];
    mygames_douzero_remote_ticket ticket = {0};
    mygames_douzero_remote_ticket unchanged = {0};
    mygames_douzero_remote_identity id = owner_identity();
    unsigned i;
    for (i = 0u; i < 130u; ++i) scores[i] = -999.0f;
    id.model_sha256[0] = 0u;
    id.role = 2u; /* wrong for physical seat 2 */
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID);
    CHECK(memcmp(&ticket, &unchanged, sizeof(ticket)) == 0);
    id.role = 1u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 129u,
        1000u, 50u) == MYGAMES_DOUZERO_REMOTE_CAPACITY);
    CHECK(memcmp(&ticket, &unchanged, sizeof(ticket)) == 0);
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        50u, 50u) == INVALID);
    CHECK(memcmp(&ticket, &unchanged, sizeof(ticket)) == 0);
    id.wire_request_id_base = UINT64_MAX - UINT64_C(1);
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID); /* 3 chunk wire IDs overflow */
    id.wire_request_id_base = UINT64_C(9001);
    memset(id.model_sha256, 0, sizeof(id.model_sha256));
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID);
    id = owner_identity();
    memset(id.legal_list_sha256, 0, sizeof(id.legal_list_sha256));
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID);
    id = owner_identity();
    id.rules_profile = 42u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID);
    id = owner_identity();
    id.legal_action_count = MYGAMES_DOUZERO_REMOTE_MAX_ACTIONS + 1u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == INVALID);
    id = owner_identity();
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == OK);
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        1000u, 50u) == MYGAMES_DOUZERO_REMOTE_BUSY);
    for (i = 0u; i < 130u; ++i) CHECK(scores[i] == -999.0f);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
    CHECK(memcmp(&ticket, &unchanged, sizeof(ticket)) == 0);
}

static void test_chunk_and_owner_fences(void) {
    float buffer[130], first[64], second[64], last[2];
    float previous[130];
    size_t choice = 999u, size;
    uint32_t chunk, total;
    uint64_t wire;
    mygames_douzero_remote_ticket ticket = {0};
    mygames_douzero_remote_identity id = owner_identity(), altered;
    mygames_douzero_remote_reply_chunk reply, fake;
    size_t i;
    for (i = 0u; i < 130u; ++i) buffer[i] = -999.0f;
    for (i = 0u; i < 64u; ++i) {
        first[i] = (float)i / 100.0f;
        second[i] = 0.1f;
    }
    for (i = 0u; i < 2u; ++i) last[i] = 0.2f;
    second[4] = 9.0f;  /* canonical global action 68 */
    second[25] = 9.0f; /* tie at global 89; choose lower legal index */
    CHECK(mygames_douzero_remote_begin(&ticket, &id, buffer, 130u,
        1000u, 50u) == OK);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 50u, &wire, &chunk, &total, &size) == OK);
    CHECK(wire == 9001u && chunk == 0u && total == 3u && size == 64u);
    CHECK(mygames_douzero_remote_best_index(&ticket, &id, 50u, &choice) ==
          MYGAMES_DOUZERO_REMOTE_INCOMPLETE && choice == 999u);
    fake = valid_reply(&id, 1u, second, 64u);
    memcpy(previous, buffer, sizeof(buffer));
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    CHECK(memcmp(buffer, previous, sizeof(buffer)) == 0);
    reply = valid_reply(&id, 0u, first, 64u);
    fake = reply;
    fake.identity.owner_epoch++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.room_generation++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.transport_generation++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.model_sha256[9] ^= 1u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.legal_list_sha256[5] ^= 1u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.game_id++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.turn_id++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.seat = 0u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.role = 0u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.identity.ticket_id++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.chunk_count = 2u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.wire_request_id++;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    fake.score_count = 63u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) == STALE);
    fake = reply;
    {
        float invalid_scores[64];
        memcpy(invalid_scores, first, sizeof(first));
        invalid_scores[15] = NAN;
        fake.scores = invalid_scores;
        CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) ==
              MYGAMES_DOUZERO_REMOTE_NONFINITE);
        invalid_scores[15] = INFINITY;
        CHECK(mygames_douzero_remote_accept(&ticket, &fake, 51u) ==
              MYGAMES_DOUZERO_REMOTE_NONFINITE);
    }
    CHECK(memcmp(buffer, previous, sizeof(buffer)) == 0);
    CHECK(mygames_douzero_remote_accept(&ticket, &reply, 52u) == OK);
    CHECK(mygames_douzero_remote_accept(&ticket, &reply, 53u) == STALE);
    for (i = 0u; i < 64u; ++i) CHECK(buffer[i] == first[i]);
    for (i = 64u; i < 130u; ++i) CHECK(buffer[i] == -999.0f);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 53u, &wire, &chunk, &total, &size) == OK);
    CHECK(wire == 9002u && chunk == 1u && total == 3u && size == 64u);
    reply = valid_reply(&id, 1u, second, 64u);
    CHECK(mygames_douzero_remote_accept(&ticket, &reply, 54u) == OK);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 54u, &wire, &chunk, &total, &size) == OK);
    CHECK(wire == 9003u && chunk == 2u && total == 3u && size == 2u);
    reply = valid_reply(&id, 2u, last, 2u);
    fake = reply;
    fake.score_count = 1u;
    CHECK(mygames_douzero_remote_accept(&ticket, &fake, 55u) == STALE);
    CHECK(buffer[128] == -999.0f && buffer[129] == -999.0f);
    CHECK(mygames_douzero_remote_accept(&ticket, &reply, 55u) == OK);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 55u, &wire, &chunk, &total, &size) == REPLAY);
    altered = id;
    altered.model_version++;
    CHECK(mygames_douzero_remote_best_index(&ticket, &altered, 55u, &choice) ==
          STALE && choice == 999u);
    altered = id;
    altered.legal_list_sha256[0] ^= 1u;
    CHECK(mygames_douzero_remote_best_index(&ticket, &altered, 55u, &choice) ==
          STALE && choice == 999u);
    altered = id;
    altered.owner_epoch++;
    CHECK(mygames_douzero_remote_best_index(&ticket, &altered, 55u, &choice) ==
          STALE && choice == 999u);
    altered = id;
    altered.transport_generation++;
    CHECK(mygames_douzero_remote_best_index(&ticket, &altered, 55u, &choice) ==
          STALE && choice == 999u);
    CHECK(mygames_douzero_remote_best_index(&ticket, &id, 55u, &choice) == OK);
    CHECK(choice == 68u);
    CHECK(mygames_douzero_remote_best_index(&ticket, &id, 55u, &choice) ==
          REPLAY && choice == 68u);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
}

static void test_cancel_deadline_connection_reuse(void) {
    mygames_douzero_remote_identity id = owner_identity(), next = id;
    mygames_douzero_remote_ticket ticket = {0};
    float scores[130] = {0}, first[64] = {0};
    mygames_douzero_remote_reply_chunk old_reply;
    uint64_t wire = 77u;
    uint32_t index = 77u, chunks = 77u;
    size_t count = 77u, choice = 777u;
    size_t i;
    for (i = 0u; i < 130u; ++i) scores[i] = 123.25f;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 130u,
        100u, 50u) == OK);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 100u, &wire, &index, &chunks, &count) ==
          MYGAMES_DOUZERO_REMOTE_DEADLINE);
    CHECK(wire == 77u && index == 77u && chunks == 77u && count == 77u);
    old_reply = valid_reply(&id, 0u, first, 64u);
    CHECK(mygames_douzero_remote_accept(&ticket, &old_reply, 100u) ==
          MYGAMES_DOUZERO_REMOTE_DEADLINE);
    CHECK(mygames_douzero_remote_best_index(&ticket, &id, 100u, &choice) ==
          MYGAMES_DOUZERO_REMOTE_DEADLINE && choice == 777u);
    CHECK(mygames_douzero_remote_reset(&ticket) == MYGAMES_DOUZERO_REMOTE_BUSY);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_accept(&ticket, &old_reply, 51u) ==
          MYGAMES_DOUZERO_REMOTE_CANCELLED);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
    next.ticket_id++;
    next.wire_request_id_base = 9100u;
    next.transport_generation++;
    CHECK(mygames_douzero_remote_begin(&ticket, &next, scores, 130u,
        500u, 101u) == OK);
    CHECK(mygames_douzero_remote_accept(&ticket, &old_reply, 102u) == STALE);
    CHECK(mygames_douzero_remote_accept(&ticket,
        &(mygames_douzero_remote_reply_chunk){0}, 102u) == INVALID);
    CHECK(scores[0] == 123.25f);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_cancel(&ticket) ==
          MYGAMES_DOUZERO_REMOTE_CANCELLED);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
}

static void test_role_and_one_chunk(void) {
    mygames_douzero_remote_identity id = owner_identity();
    mygames_douzero_remote_ticket ticket = {0};
    mygames_douzero_remote_reply_chunk response;
    float scores[1] = {77.0f}, value[1] = {0.625f};
    size_t choice = 88u;
    uint64_t wire;
    uint32_t chunk, total;
    size_t size;
    /* Physical seat 0 landlord when landlord seat is 0. */
    id.seat = 0u; id.role = 0u; id.legal_action_count = 1u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 1u,
        200u, 100u) == OK);
    CHECK(mygames_douzero_remote_expected_chunk(
        &ticket, 101u, &wire, &chunk, &total, &size) == OK);
    CHECK(wire == 9001u && chunk == 0u && total == 1u && size == 1u);
    response = valid_reply(&id, 0u, value, 1u);
    response.chunk_count = 1u;
    CHECK(mygames_douzero_remote_accept(&ticket, &response, 102u) == OK);
    CHECK(mygames_douzero_remote_best_index(&ticket, &id, 102u, &choice) == OK);
    CHECK(choice == 0u && scores[0] == 0.625f);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
    /* Moving landlord to physical seat 1 must recalculate role mapping. */
    id.landlord_seat = 1u;
    id.seat = 2u; id.role = 2u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 1u,
        200u, 100u) == OK);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
    id.seat = 0u; id.role = 1u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 1u,
        200u, 100u) == OK);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
}

static void test_real_native_turn_transition_rejects_late_q(void) {
    uint8_t deck[MYGAMES_DDZ_CARDS];
    mygames_ddz_round round = {0};
    mygames_ddz_public_view before = {0}, after = {0};
    mygames_ddz_action_result commit = {0};
    mygames_douzero_remote_ticket ticket = {0};
    mygames_douzero_remote_identity id = owner_identity(), current;
    float scores[1] = {91.0f}, q[1] = {0.5f};
    mygames_douzero_remote_reply_chunk reply;
    size_t selected_index = 777u;
    unsigned i;

    /* Real authoritative M5a physical cards and turn history. The remote
     * ticket is only a read-only observer, not a playing authority. */
    for (i = 0u; i < MYGAMES_DDZ_CARDS; ++i) deck[i] = (uint8_t)i;
    CHECK(mygames_ddz_round_deal(&round, deck, 0u) == MYGAMES_DDZ_OK);
    CHECK(mygames_ddz_round_public_view(&round, &before) == MYGAMES_DDZ_OK);
    CHECK(before.turn_seat == 0u && before.landlord_seat == 0u);
    id.turn_id = before.turn_id;
    id.seat = before.turn_seat;
    id.landlord_seat = before.landlord_seat;
    id.role = 0u;
    id.legal_action_count = 1u;
    CHECK(mygames_douzero_remote_begin(&ticket, &id, scores, 1u,
        300u, 100u) == OK);
    reply = valid_reply(&id, 0u, q, 1u);
    reply.chunk_count = 1u;
    CHECK(mygames_douzero_remote_accept(&ticket, &reply, 110u) == OK);

    /* The actual C11 turn changed while the remote Q was in flight:
     * our test actor legally plays one of its OWN cards through Round,
     * not through the remote ticket. The native Game/Room store and ACK
     * are separate; no online Game Owner integration is claimed. */
    CHECK(round.hands[0] & UINT64_C(1));
    CHECK(mygames_ddz_round_submit(&round, 0u, before.turn_id,
        UINT64_C(1), &commit) == MYGAMES_DDZ_OK);
    CHECK(mygames_ddz_round_public_view(&round, &after) == MYGAMES_DDZ_OK);
    CHECK(after.turn_id == before.turn_id + 1u);
    CHECK(after.turn_seat == 1u && after.landlord_seat == 0u);
    current = id;
    current.turn_id = after.turn_id;
    current.seat = after.turn_seat;
    current.role = 2u;
    CHECK(mygames_douzero_remote_best_index(&ticket, &current,
        111u, &selected_index) == STALE);
    CHECK(selected_index == 777u); /* late response cannot select action */
    CHECK(round.turn_id == after.turn_id);
    CHECK(mygames_douzero_remote_cancel(&ticket) == OK);
    CHECK(mygames_douzero_remote_reset(&ticket) == OK);
}

int main(void) {
    test_real_native_turn_transition_rejects_late_q();
    test_begin_atomic();
    test_chunk_and_owner_fences();
    test_cancel_deadline_connection_reuse();
    test_role_and_one_chunk();
    puts("mygames.douzero_remote_ticket: PASS — no action commit, no fallback");
    return 0;
}
