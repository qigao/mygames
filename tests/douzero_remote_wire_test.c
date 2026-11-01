#include "mygames/douzero_remote_wire.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void check_unchanged(const uint8_t *p, size_t n, uint8_t byte) {
    size_t i;
    for (i = 0; i < n; ++i) REQUIRE(p[i] == byte);
}
static void check_scores(const float *p, size_t n, float value) {
    size_t i;
    for (i = 0; i < n; ++i) REQUIRE(p[i] == value);
}
static mygames_douzero_ticket make_ticket(uint8_t role) {
    mygames_douzero_ticket t = {0};
    size_t i;
    t.role = role;
    t.request_id = UINT64_C(0xfedcba9876543210);
    t.room_epoch = UINT64_C(17);
    t.game_id = UINT64_C(98765);
    t.turn_id = UINT64_C(432);
    t.owner_generation = UINT64_C(19);
    t.model_version = UINT64_C(201);
    t.first_action = 64u;
    t.total_actions = 130u;
    t.action_count = 64u;
    for (i = 0; i < 32u; ++i) {
        t.legal_actions_sha256[i] = (uint8_t)(i + 1u);
        t.model_artifact_sha256[i] = (uint8_t)(i + 101u);
    }
    return t;
}
static void test_roundtrip(uint8_t role) {
    const size_t width = role == 0u ? 319u : 430u;
    mygames_douzero_ticket ticket = make_ticket(role);
    mygames_douzero_ticket changed;
    mygames_douzero_request_view view = {0};
    uint8_t request[MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX];
    uint8_t response[MYGAMES_DOUZERO_REMOTE_REPLY_BYTES_MAX];
    uint8_t copy[MYGAMES_DOUZERO_REMOTE_REQUEST_BYTES_MAX];
    int8_t x[MYGAMES_DOUZERO_REMOTE_STATE_FARMER];
    int8_t z[MYGAMES_DOUZERO_REMOTE_HISTORY_BYTES];
    int8_t action[MYGAMES_DOUZERO_REMOTE_BATCH_MAX * MYGAMES_DOUZERO_REMOTE_ACTION_BYTES];
    float q[MYGAMES_DOUZERO_REMOTE_BATCH_MAX], observed[MYGAMES_DOUZERO_REMOTE_BATCH_MAX];
    size_t request_size = 1999u, response_size = 4444u;
    size_t i;

    for (i = 0; i < sizeof(x); ++i) x[i] = (int8_t)(i % 2u);
    for (i = 0; i < sizeof(z); ++i) z[i] = (int8_t)((i % 3u) == 0u);
    for (i = 0; i < sizeof(action); ++i) action[i] = (int8_t)((i % 4u) == 0u);
    for (i = 0; i < 64u; ++i) q[i] = ((float)i - 24.0f) / 8.0f;
    memset(request, 0xa5, sizeof(request));
    REQUIRE(mygames_douzero_wire_encode_request(
        &ticket, x, width, z, sizeof(z), action, sizeof(action),
        request, sizeof(request), &request_size) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(request_size == MYGAMES_DOUZERO_REMOTE_HEADER_BYTES +
                            width + 810u + 64u * 54u);
    REQUIRE(request[0] == 'D' && request[1] == 'Z' &&
            request[2] == 'Q' && request[3] == 'I' &&
            request[4] == MYGAMES_DOUZERO_REMOTE_ABI &&
            request[5] == 1u && request[6] == role && request[7] == 1u);
    REQUIRE(mygames_douzero_wire_decode_request(
        request, request_size, &view) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(view.ticket.request_id == ticket.request_id);
    REQUIRE(view.ticket.game_id == ticket.game_id);
    REQUIRE(view.ticket.owner_generation == ticket.owner_generation);
    REQUIRE(view.ticket.first_action == ticket.first_action);
    REQUIRE(view.ticket.total_actions == ticket.total_actions);
    REQUIRE(view.ticket.action_count == ticket.action_count);
    REQUIRE(memcmp(view.ticket.legal_actions_sha256,
                   ticket.legal_actions_sha256, 32u) == 0);
    REQUIRE(memcmp(view.ticket.model_artifact_sha256,
                   ticket.model_artifact_sha256, 32u) == 0);
    REQUIRE(memcmp(view.x, x, width) == 0);
    REQUIRE(memcmp(view.z, z, 810u) == 0);
    REQUIRE(memcmp(view.actions, action, sizeof(action)) == 0);

    REQUIRE(mygames_douzero_wire_encode_reply(
        &ticket, q, 64u, response, sizeof(response), &response_size) ==
            MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(response_size == MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + 64u * 4u);
    for (i = 0; i < 64u; ++i) observed[i] = 991.25f;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &ticket, observed, 64u) ==
            MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(memcmp(observed, q, sizeof(q)) == 0);

    /* Every identity field is an authoritative owner-fence. */
    changed = ticket; ++changed.request_id;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.room_epoch;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.game_id;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.turn_id;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.owner_generation;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.model_version;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; changed.legal_actions_sha256[2] ^= 4u;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; changed.model_artifact_sha256[2] ^= 4u;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.first_action;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);
    changed = ticket; ++changed.total_actions;
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &changed, observed, 64u) == MYGAMES_DOUZERO_WIRE_STALE);

    memcpy(copy, request, request_size);
    copy[5] = 2u;
    REQUIRE(mygames_douzero_wire_decode_request(
        copy, request_size, &view) == MYGAMES_DOUZERO_WIRE_INVALID);
    copy[5] = 1u; copy[4] = 2u;
    REQUIRE(mygames_douzero_wire_decode_request(
        copy, request_size, &view) == MYGAMES_DOUZERO_WIRE_INVALID);
    copy[4] = 1u; copy[7] = 2u;
    REQUIRE(mygames_douzero_wire_decode_request(
        copy, request_size, &view) == MYGAMES_DOUZERO_WIRE_INVALID);
    REQUIRE(mygames_douzero_wire_decode_request(
        request, request_size - 1u, &view) == MYGAMES_DOUZERO_WIRE_INVALID);
    memcpy(copy, request, request_size);
    copy[MYGAMES_DOUZERO_REMOTE_HEADER_BYTES + width + 810u] = 2u;
    REQUIRE(mygames_douzero_wire_decode_request(
        copy, request_size, &view) == MYGAMES_DOUZERO_WIRE_INVALID);

    /* Capacity and invalid features leave caller bytes and used size alone. */
    memset(copy, 0x5a, sizeof(copy));
    request_size = 714u;
    REQUIRE(mygames_douzero_wire_encode_request(
        &ticket, x, width, z, sizeof(z), action, sizeof(action),
        copy, 1u, &request_size) == MYGAMES_DOUZERO_WIRE_CAPACITY);
    REQUIRE(request_size == 714u);
    check_unchanged(copy, sizeof(copy), 0x5a);
    x[0] = 2;
    REQUIRE(mygames_douzero_wire_encode_request(
        &ticket, x, width, z, sizeof(z), action, sizeof(action),
        copy, sizeof(copy), &request_size) == MYGAMES_DOUZERO_WIRE_INVALID);
    check_unchanged(copy, sizeof(copy), 0x5a);
    x[0] = 0;

    for (i = 0; i < 64u; ++i) observed[i] = 991.25f;
    copy[0] = response[0];
    memcpy(copy, response, response_size);
    copy[response_size - 1u] = 0x7f;
    copy[response_size - 2u] = 0xc0;
    copy[response_size - 3u] = 0x00;
    copy[response_size - 4u] = 0x00; /* IEEE NaN */
    REQUIRE(mygames_douzero_wire_decode_reply(
        copy, response_size, &ticket, observed, 64u) ==
            MYGAMES_DOUZERO_WIRE_NONFINITE);
    check_scores(observed, 64u, 991.25f);
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size - 1u, &ticket, observed, 64u) ==
            MYGAMES_DOUZERO_WIRE_INVALID);
    check_scores(observed, 64u, 991.25f);
    REQUIRE(mygames_douzero_wire_decode_reply(
        response, response_size, &ticket, observed, 63u) ==
            MYGAMES_DOUZERO_WIRE_INVALID);
    check_scores(observed, 64u, 991.25f);
    q[0] = NAN;
    memset(copy, 0x5a, sizeof(copy));
    response_size = 123u;
    REQUIRE(mygames_douzero_wire_encode_reply(
        &ticket, q, 64u, copy, sizeof(copy), &response_size) ==
            MYGAMES_DOUZERO_WIRE_NONFINITE);
    REQUIRE(response_size == 123u);
    check_unchanged(copy, sizeof(copy), 0x5a);

    /* Terminal chunk must be exact: total=130, index=128, count=2. */
    ticket.first_action = 128u; ticket.action_count = 2u;
    REQUIRE(mygames_douzero_wire_encode_request(
        &ticket, x, width, z, sizeof(z), action, 108u,
        request, sizeof(request), &request_size) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(mygames_douzero_wire_decode_request(
        request, request_size, &view) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(view.ticket.first_action == 128u && view.ticket.action_count == 2u);
    ticket.action_count = 3u;
    REQUIRE(mygames_douzero_wire_encode_request(
        &ticket, x, width, z, sizeof(z), action, 162u,
        request, sizeof(request), &request_size) == MYGAMES_DOUZERO_WIRE_INVALID);
    (void)role;
}

static void test_full_legal_batches(void) {
    mygames_douzero_ticket first = make_ticket(0u), batch[3];
    mygames_douzero_score_collector tracker = {0};
    uint8_t packet[3][MYGAMES_DOUZERO_REMOTE_REPLY_BYTES_MAX];
    size_t bytes[3] = {0};
    float values[130], expected[130], rows[64];
    size_t i, j;
    first.first_action = 0u;
    first.action_count = 64u;
    for (i = 0; i < 130u; ++i) {
        values[i] = 991.25f;
        expected[i] = ((float)i - 52.0f) / 8.0f;
    }
    REQUIRE(mygames_douzero_score_collector_begin(
        &first, values, 129u, &tracker) == MYGAMES_DOUZERO_WIRE_INVALID);
    REQUIRE(tracker.expected_count == 0u);
    REQUIRE(mygames_douzero_score_collector_begin(
        &first, values, 130u, &tracker) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(tracker.expected_count == 3u);
    REQUIRE(!mygames_douzero_score_collector_complete(&tracker));
    for (i = 0; i < 3u; ++i) {
        REQUIRE(mygames_douzero_score_collector_ticket(
            &tracker, i, &batch[i]) == MYGAMES_DOUZERO_WIRE_OK);
        REQUIRE(batch[i].first_action == 64u * i);
        REQUIRE(batch[i].request_id == first.request_id + i);
        REQUIRE(batch[i].action_count == (i == 2u ? 2u : 64u));
        for (j = 0u; j < batch[i].action_count; ++j)
            rows[j] = expected[64u * i + j];
        REQUIRE(mygames_douzero_wire_encode_reply(
            &batch[i], rows, batch[i].action_count,
            packet[i], sizeof(packet[i]), &bytes[i]) ==
            MYGAMES_DOUZERO_WIRE_OK);
    }
    /* HTTP/2 responses need not arrive in request order. */
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[2], bytes[2]) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(!mygames_douzero_score_collector_complete(&tracker));
    REQUIRE(values[128] == expected[128] && values[129] == expected[129]);
    REQUIRE(values[0] == 991.25f && values[64] == 991.25f);
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[2], bytes[2]) == MYGAMES_DOUZERO_WIRE_STALE);
    REQUIRE(tracker.received_count == 1u);
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[0], bytes[0]) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(!mygames_douzero_score_collector_complete(&tracker));
    /* Wrong owner generation must not corrupt missing batch scores. */
    packet[1][40] ^= 1u;
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[1], bytes[1]) == MYGAMES_DOUZERO_WIRE_STALE);
    check_scores(values + 64, 64u, 991.25f);
    packet[1][40] ^= 1u;
    /* Incorrect full-list ordered digest is also a stale proposal. */
    packet[1][72] ^= 1u;
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[1], bytes[1]) == MYGAMES_DOUZERO_WIRE_STALE);
    check_scores(values + 64, 64u, 991.25f);
    packet[1][72] ^= 1u;
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[1], bytes[1]) == MYGAMES_DOUZERO_WIRE_OK);
    REQUIRE(mygames_douzero_score_collector_complete(&tracker));
    REQUIRE(tracker.received_count == 3u);
    REQUIRE(memcmp(values, expected, sizeof(expected)) == 0);
    REQUIRE(mygames_douzero_score_collector_accept(
        &tracker, packet[0], bytes[0]) == MYGAMES_DOUZERO_WIRE_STALE);
    REQUIRE(memcmp(values, expected, sizeof(expected)) == 0);
    REQUIRE(mygames_douzero_score_collector_ticket(
        &tracker, 3u, &batch[0]) == MYGAMES_DOUZERO_WIRE_INVALID);
}

int main(void) {
    test_full_legal_batches();
    test_roundtrip(0u);
    test_roundtrip(1u);
    test_roundtrip(2u);
    puts("mygames.douzero_remote_h2_ws_wire: PASS "
         "roles=3 batch64=true terminal_chunk=true "
         "owner_turn_model_sha_fences=true nonfinite_no_mutation=true "
         "complete_130_actions_three_batches_out_of_order=true");
    return 0;
}
