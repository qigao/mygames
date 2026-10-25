#include "mygames/recovery_wire.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool example_validator(const uint8_t *payload, size_t n,
                              uint64_t epoch, uint64_t sequence) {
    static const char canonical[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":1,\"seq\":1}";
    return epoch == 1u && sequence == 1u &&
           n == sizeof(canonical) - 1u &&
           memcmp(payload, canonical, n) == 0;
}

static void assert_query(const char *target, int expect_ok,
                         uint64_t after, uint32_t limit) {
    uint64_t got_after = UINT64_MAX;
    uint32_t got_limit = UINT32_MAX;
    const mygames_recovery_wire_status result =
        mygames_recovery_parse_target(target, &got_after, &got_limit);
    assert((result == MYGAMES_RECOVERY_WIRE_OK) == (expect_ok != 0));
    if (expect_ok) {
        assert(got_after == after && got_limit == limit);
    } else {
        assert(got_after == UINT64_MAX && got_limit == UINT32_MAX);
    }
}

int main(void) {
    static const char event[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":1,\"seq\":1}";
    static const char *const invalid[] = {
        MYGAMES_RECOVERY_PATH,
        MYGAMES_RECOVERY_PATH "?after=0",
        MYGAMES_RECOVERY_PATH "?limit=1",
        MYGAMES_RECOVERY_PATH "?after=0&limit=0",
        MYGAMES_RECOVERY_PATH "?after=0&limit=17",
        MYGAMES_RECOVERY_PATH "?after=-1&limit=1",
        MYGAMES_RECOVERY_PATH "?after=+1&limit=1",
        MYGAMES_RECOVERY_PATH "?after=00&limit=1",
        MYGAMES_RECOVERY_PATH "?after=0&limit=01",
        MYGAMES_RECOVERY_PATH "?after=0&limit=1&after=0",
        MYGAMES_RECOVERY_PATH "?after=0&limit=1&admin=1",
        MYGAMES_RECOVERY_PATH "?after=0&limit=1&",
        MYGAMES_RECOVERY_PATH "?after=0&&limit=1",
        MYGAMES_RECOVERY_PATH "?after=9223372036854775808&limit=1",
        MYGAMES_RECOVERY_PATH "?after=18446744073709551615&limit=1",
        MYGAMES_RECOVERY_PATH "?after=0%26admin=1&limit=1",
        MYGAMES_RECOVERY_PATH "?after=0&limit=1#x",
        MYGAMES_RECOVERY_PATH "?after=0&limit=1=2",
        MYGAMES_RECOVERY_PATH "/extra?after=0&limit=1"
    };
    mygames_recovery_grant grant = {0};
    mygames_store_archive_page page = {0};
    char json[MYGAMES_RECOVERY_RESPONSE_MAX];
    size_t bytes = 0u;
    size_t i;

    assert_query(MYGAMES_RECOVERY_PATH "?after=0&limit=16", 1, 0u, 16u);
    assert_query(MYGAMES_RECOVERY_PATH "?limit=1&after=9223372036854775807",
                 1, (uint64_t)INT64_MAX, 1u);
    for (i = 0u; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        assert_query(invalid[i], 0, 0u, 0u);
    assert(mygames_recovery_parse_target(NULL, NULL, NULL) ==
           MYGAMES_RECOVERY_WIRE_BAD_QUERY);

    grant.generation = 1u;
    grant.page_limit = 1u;
    (void)snprintf(grant.source_namespace, sizeof(grant.source_namespace),
                   "%s", "trusted-source-v1");
    (void)snprintf(grant.consumer_id, sizeof(grant.consumer_id),
                   "%s", "svc-consumer");
    page.epoch = 1u;
    page.checkpoint_sequence = 1u;
    page.retained_from_sequence = 1u;
    page.resume_after_sequence = 1u;
    page.count = 1u;
    page.items[0].sequence = 1u;
    page.items[0].size = sizeof(event) - 1u;
    memcpy(page.items[0].payload, event, sizeof(event) - 1u);
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) == MYGAMES_RECOVERY_WIRE_OK);
    assert(bytes > 100u && bytes == strlen(json));
    assert(strstr(json, "\"source_namespace\":\"trusted-source-v1\"") != NULL);
    assert(strstr(json, "\"consumer_id\":\"svc-consumer\"") != NULL);
    assert(strstr(json, "\"seq\":1,\"payload\":") != NULL);
    assert(strstr(json, "\"has_more\":false}") != NULL);

    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, 32u, &bytes) == MYGAMES_RECOVERY_WIRE_CAPACITY);
    assert(bytes == 0u);

    page.items[0].sequence = 2u;
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) ==
           MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    page.items[0].sequence = 1u;
    page.items[0].payload[0] = '!';
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) ==
           MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    page.items[0].payload[0] = '{';

    page.has_more = 1;
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) ==
           MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    page.has_more = 0;

    page.count = 0u;
    page.resume_after_sequence = 0u;
    page.has_more = 1;
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) ==
           MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    page.count = 1u;
    page.resume_after_sequence = 1u;
    page.has_more = 0;

    grant.consumer_id[0] = '"';
    assert(mygames_recovery_encode_page(
               &grant, &page, example_validator,
               json, sizeof(json), &bytes) ==
           MYGAMES_RECOVERY_WIRE_INVALID_PAGE);
    puts("mygames.recovery_wire: PASS");
    return 0;
}
