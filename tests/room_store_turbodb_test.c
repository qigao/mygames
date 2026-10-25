#include "mygames/room_store.h"
#include <orm.h>
#include <orm_runtime.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(mygames_store_status got, mygames_store_status expected) {
    if (got != expected) {
        fprintf(stderr, "MyGames TurboDB status %d != expected %d\n",
                (int)got, (int)expected);
        abort();
    }
}

static void load(mygames_room_store *store, mygames_room_directory *out,
                 uint64_t wanted_epoch, uint64_t wanted_seq) {
    uint64_t epoch = UINT64_C(999), seq = UINT64_C(999);
    check(mygames_room_store_load(store, out, &epoch, &seq), MYGAMES_STORE_OK);
    assert(epoch == wanted_epoch && seq == wanted_seq);
}

/* Test-only corruption through the SAME TurboDB Driver API, not raw file IO.
 * Deleting one journal row simulates a torn/misrecovered application history.
 * A matching checkpoint checksum must not mask the missing command record. */
static void corrupt_journal_with_turbodb(const char *plugin, const char *file) {
    orm_runtime_config_t conf;
    orm_driver_load_config_t load = {0};
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_config_t connection_config;
    orm_option_t option;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    orm_error_t error;
    uint64_t deleted = 0u;
    orm_runtime_config_init(&conf);
    orm_error_init(&error);
    assert(orm_runtime_create(&conf, &runtime, &error) == ORM_STATUS_OK);
    load.struct_size = sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(plugin);
    load.expected_driver_id = orm_view("sqlite");
    assert(orm_runtime_load_driver(runtime, &load, &error) == ORM_STATUS_OK);
    orm_config(&connection_config);
    option.keyword = orm_view("filename");
    option.value = orm_view(file);
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &option;
    connection_config.option_count = 1u;
    assert(orm_runtime_connect(runtime, &connection_config,
                               &connection, &error) == ORM_STATUS_OK);
    assert(orm_raw(connection,
        orm_view("delete from mg_commands where player_id=?1 and request_id=?2"),
        &query, &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_i64(101), &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_text("req-1"), &error) == ORM_STATUS_OK);
    assert(orm_query_execute(query, &result, &error) == ORM_STATUS_OK);
    assert(orm_result_affected_rows(result, &deleted, &error) == ORM_STATUS_OK);
    assert(deleted == 1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    assert(orm_connection_close(connection, &error) == ORM_STATUS_OK);
    orm_connection_release(connection);
    assert(orm_runtime_close(runtime, &error) == ORM_STATUS_OK);
    orm_runtime_release(runtime);
}

int main(int argc, char **argv) {
    mygames_room_store *store = NULL;
    mygames_room_directory rooms, loaded;
    const mygames_room *room;
    uint8_t digest_a[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    uint8_t digest_b[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    unsigned int seat = 100u;
    uint64_t seq = 0u, claimed = 0u;
    uint8_t reply[MYGAMES_STORE_RESULT_MAX];
    uint8_t event[MYGAMES_STORE_EVENT_MAX];
    size_t result_bytes = 0u;
    size_t outbox_bytes = 0u;
    static const char response_a[] = "{\"type\":\"room.ack\",\"revision\":3}";
    static const char response_b[] = "{\"type\":\"room.ack\",\"revision\":4}";
    static const char event_a[] = "{\"type\":\"room.ready\",\"room_id\":\"room-1\"}";
    static const char event_b[] = "{\"type\":\"room.not_ready\",\"room_id\":\"room-1\"}";
    char sidecar[1024];

    if (argc != 3) {
        fprintf(stderr, "usage: room_store_turbodb_test /absolute/plugin /test/file.db\n");
        return 2;
    }
    (void)remove(argv[2]);
    if (snprintf(sidecar, sizeof(sidecar), "%s-wal", argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    if (snprintf(sidecar, sizeof(sidecar), "%s-shm", argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);

    memset(digest_a, 0x33, sizeof(digest_a));
    memset(digest_b, 0x77, sizeof(digest_b));

    check(mygames_room_store_open(argv[1], argv[2], &store),
          MYGAMES_STORE_OK);
    assert(store != NULL);
    load(store, &loaded, 1u, 0u);
    assert(loaded.next_serial == 0u);
    mygames_room_directory_init(&rooms);
    check((mygames_store_status)mygames_room_directory_create(
              &rooms, 100u, &room), MYGAMES_STORE_OK);
    assert(!strcmp(room->id, "room-1"));
    assert(mygames_room_directory_join(&rooms, room->id, 101u, &seat)
           == MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(&rooms, 101u, true)
           == MYGAMES_ROOM_OK);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 0u, "req-1", digest_a,
        response_a, sizeof(response_a) - 1u,
        event_a, sizeof(event_a) - 1u, &seq), MYGAMES_STORE_OK);
    assert(seq == 1u);
    {
        mygames_player_id maximum = 0u;
        check(mygames_room_store_player_high_watermark(store, &maximum),
              MYGAMES_STORE_OK);
        assert(maximum == 100u);
    }
    load(store, &loaded, 1u, 1u);
    assert(mygames_room_directory_get_player(&loaded, 101u)->seats[1].ready);

    /* Replay leaves facts unchanged but recovers its exact prior commit seq. */
    seq = 555u;
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 0u, "req-1", digest_a,
        response_a, sizeof(response_a) - 1u,
        event_a, sizeof(event_a) - 1u, &seq), MYGAMES_STORE_REPLAY);
    assert(seq == 1u);
    /* The exact stored outcome must win even if a replaying caller could
     * now compute a different response body from a newer room revision. */
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 1u, "req-1", digest_a,
        response_b, sizeof(response_b) - 1u,
        event_a, sizeof(event_a) - 1u, &seq), MYGAMES_STORE_REPLAY);
    assert(seq == 1u);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 1u, "req-1", digest_b,
        response_a, sizeof(response_a) - 1u,
        event_a, sizeof(event_a) - 1u, &seq),
        MYGAMES_STORE_REQUEST_CONFLICT);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-1", digest_b, reply, sizeof(reply), &result_bytes, &seq),
        MYGAMES_STORE_REQUEST_CONFLICT);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-1", digest_a, reply, 1u, &result_bytes, &seq),
        MYGAMES_STORE_CAPACITY);
    assert(result_bytes == sizeof(response_a) - 1u);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-1", digest_a, reply, sizeof(reply), &result_bytes, &seq),
        MYGAMES_STORE_OK);
    assert(seq == 1u && result_bytes == sizeof(response_a) - 1u);
    assert(!memcmp(reply, response_a, result_bytes));

    check(mygames_room_store_next_event(
        store, 0u, event, 1u, &outbox_bytes, &seq),
        MYGAMES_STORE_CAPACITY);
    assert(outbox_bytes == sizeof(event_a) - 1u);
    check(mygames_room_store_next_event(
        store, 0u, event, sizeof(event), &outbox_bytes, &seq),
        MYGAMES_STORE_OK);
    assert(seq == 1u && outbox_bytes == sizeof(event_a) - 1u);
    assert(!memcmp(event, event_a, outbox_bytes));
    check(mygames_room_store_ack_event(store, 1u, 1u), MYGAMES_STORE_OK);
    check(mygames_room_store_ack_event(store, 1u, 1u),
          MYGAMES_STORE_NOT_FOUND);
    check(mygames_room_store_next_event(
        store, 0u, event, sizeof(event), &outbox_bytes, &seq),
        MYGAMES_STORE_NOT_FOUND);

    assert(mygames_room_directory_ready(&rooms, 101u, false)
           == MYGAMES_ROOM_OK);

    /* Inject failure after updating the state row but before commands/outbox.
     * A reopened store must still expose the previous snapshot and seq. */
    mygames_room_store_fail_after_checkpoint_once(store);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 1u, "req-2", digest_b,
        response_b, sizeof(response_b) - 1u,
        event_b, sizeof(event_b) - 1u, &seq),
        MYGAMES_STORE_BACKEND_ERROR);
    load(store, &loaded, 1u, 1u);
    assert(mygames_room_directory_get_player(&loaded, 101u)->seats[1].ready);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-2", digest_b, reply, sizeof(reply), &result_bytes, &seq),
        MYGAMES_STORE_NOT_FOUND);

    /* A second injection point is AFTER the command journal INSERT and
     * BEFORE outbox INSERT. All three facts must still roll back together. */
    mygames_room_store_fail_after_command_once(store);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 1u, "req-2", digest_b,
        response_b, sizeof(response_b) - 1u,
        event_b, sizeof(event_b) - 1u, &seq),
        MYGAMES_STORE_BACKEND_ERROR);
    load(store, &loaded, 1u, 1u);
    assert(mygames_room_directory_get_player(&loaded, 101u)->seats[1].ready);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-2", digest_b, reply, sizeof(reply), &result_bytes, &seq),
        MYGAMES_STORE_NOT_FOUND);
    check(mygames_room_store_next_event(
        store, 1u, event, sizeof(event), &outbox_bytes, &seq),
        MYGAMES_STORE_NOT_FOUND);

    check(mygames_room_store_claim_epoch(store, 2u, &claimed),
          MYGAMES_STORE_STALE_EPOCH);
    check(mygames_room_store_claim_epoch(store, 1u, &claimed),
          MYGAMES_STORE_OK);
    assert(claimed == 2u);
    load(store, &loaded, 2u, 1u);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 1u, 1u, "req-2", digest_b,
        response_b, sizeof(response_b) - 1u,
        event_b, sizeof(event_b) - 1u, &seq),
        MYGAMES_STORE_STALE_EPOCH);
    mygames_room_store_close(store);
    store = NULL;

    /* Second runtime and SQLite connection recover the physical DB file,
     * including the claimed owner epoch and durable command result. */
    check(mygames_room_store_open(argv[1], argv[2], &store),
          MYGAMES_STORE_OK);
    load(store, &loaded, 2u, 1u);
    check(mygames_room_store_commit(
        store, 100u, &rooms, 2u, 1u, "req-2", digest_b,
        response_b, sizeof(response_b) - 1u,
        event_b, sizeof(event_b) - 1u, &seq),
        MYGAMES_STORE_OK);
    assert(seq == 2u);
    check(mygames_room_store_next_event(
        store, 0u, event, sizeof(event), &outbox_bytes, &seq),
        MYGAMES_STORE_OK);
    assert(seq == 2u && !memcmp(event, event_b, outbox_bytes));
    mygames_room_store_close(store);
    store = NULL;

    check(mygames_room_store_open(argv[1], argv[2], &store),
          MYGAMES_STORE_OK);
    load(store, &loaded, 2u, 2u);
    assert(!mygames_room_directory_get_player(&loaded, 101u)->seats[1].ready);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-2", digest_b, reply, sizeof(reply), &result_bytes, &seq),
        MYGAMES_STORE_OK);
    assert(seq == 2u && result_bytes == sizeof(response_b) - 1u);
    check(mygames_room_store_claim_epoch(store, 2u, &claimed),
          MYGAMES_STORE_OK);
    assert(claimed == 3u);
    load(store, &loaded, 3u, 2u);
    check(mygames_room_store_ack_event(store, 2u, 2u),
          MYGAMES_STORE_STALE_EPOCH);
    check(mygames_room_store_ack_event(store, 3u, 2u),
          MYGAMES_STORE_OK);
    check(mygames_room_store_next_event(
        store, 0u, event, sizeof(event), &outbox_bytes, &seq),
        MYGAMES_STORE_NOT_FOUND);

    /* Different players are allowed to reuse their locally generated
     * request_id, but outcomes must remain isolated by authoritative ID. */
    check(mygames_room_store_get_outcome(
        store, 101u, "req-1", digest_a, reply, sizeof(reply),
        &result_bytes, &seq), MYGAMES_STORE_NOT_FOUND);
    check(mygames_room_store_commit(
        store, 101u, &rooms, 3u, 2u, "req-1", digest_a,
        response_a, sizeof(response_a) - 1u,
        event_a, sizeof(event_a) - 1u, &seq), MYGAMES_STORE_OK);
    assert(seq == 3u);
    check(mygames_room_store_get_outcome(
        store, 101u, "req-1", digest_a, reply, sizeof(reply),
        &result_bytes, &seq), MYGAMES_STORE_OK);
    assert(seq == 3u);
    check(mygames_room_store_get_outcome(
        store, 100u, "req-1", digest_a, reply, sizeof(reply),
        &result_bytes, &seq), MYGAMES_STORE_OK);
    assert(seq == 1u);
    mygames_room_store_close(store);
    store = NULL;

    /* A disconnected player is recovered by an opaque 256-bit ticket
     * fingerprint; SQLite never needs the plaintext bearer capability. */
    {
        uint8_t old_hash[MYGAMES_STORE_TICKET_HASH_BYTES];
        uint8_t provisional_hash[MYGAMES_STORE_TICKET_HASH_BYTES];
        uint8_t rotated_hash[MYGAMES_STORE_TICKET_HASH_BYTES];
        uint8_t later_hash[MYGAMES_STORE_TICKET_HASH_BYTES];
        uint8_t temp_hash[MYGAMES_STORE_TICKET_HASH_BYTES];
        mygames_player_id old_player = 0u, temporary_player = 0u;
        mygames_player_id recovered_player = 0u, next_player = 0u;
        uint64_t recovered_generation = 0u;
        memset(old_hash, 0x12, sizeof(old_hash));
        memset(provisional_hash, 0x34, sizeof(provisional_hash));
        memset(rotated_hash, 0x56, sizeof(rotated_hash));
        memset(later_hash, 0x78, sizeof(later_hash));
        memset(temp_hash, 0x9a, sizeof(temp_hash));
        check(mygames_room_store_open(argv[1], argv[2], &store),
              MYGAMES_STORE_OK);
        load(store, &loaded, 3u, 3u);
        check(mygames_room_store_issue_session(
                  store, 3u, old_hash, &old_player), MYGAMES_STORE_OK);
        assert(old_player == 102u);
        check(mygames_room_store_issue_session(
                  store, 3u, provisional_hash, &temporary_player),
              MYGAMES_STORE_OK);
        assert(temporary_player == 103u);
        check(mygames_room_store_has_session(store, old_player),
              MYGAMES_STORE_OK);
        check(mygames_room_store_resume_session(
                  store, 3u, temporary_player, old_hash, rotated_hash,
                  40000u, &recovered_player, &recovered_generation),
              MYGAMES_STORE_NOT_FOUND); /* active lease is not transferable */
        check(mygames_room_store_detach_session(
                  store, 3u, old_player, 1u, 50000u), MYGAMES_STORE_OK);
        check(mygames_room_store_resume_session(
                  store, 3u, temporary_player, old_hash, rotated_hash,
                  40000u, &recovered_player, &recovered_generation),
              MYGAMES_STORE_OK);
        assert(recovered_player == old_player && recovered_generation == 2u);
        check(mygames_room_store_has_session(store, temporary_player),
              MYGAMES_STORE_NOT_FOUND);
        check(mygames_room_store_resume_session(
                  store, 3u, temporary_player, old_hash, later_hash,
                  40001u, &recovered_player, &recovered_generation),
              MYGAMES_STORE_NOT_FOUND); /* old hash is consumed */
        check(mygames_room_store_detach_session(
                  store, 3u, old_player, 1u, 52000u),
              MYGAMES_STORE_NOT_FOUND); /* stale generation */
        check(mygames_room_store_player_high_watermark(store, &next_player),
              MYGAMES_STORE_OK);
        assert(next_player == 103u);
        mygames_room_store_close(store);
        store = NULL;

        /* Re-open from disk. The old process had a live connection, so the
         * epoch takeover must atomically make it detached for recovery. */
        check(mygames_room_store_open(argv[1], argv[2], &store),
              MYGAMES_STORE_OK);
        check(mygames_room_store_claim_epoch_for_recovery(
                  store, 3u, 60000u, &claimed), MYGAMES_STORE_OK);
        assert(claimed == 4u);
        load(store, &loaded, 4u, 3u);
        check(mygames_room_store_issue_session(
                  store, 3u, temp_hash, &next_player),
              MYGAMES_STORE_STALE_EPOCH);
        check(mygames_room_store_issue_session(
                  store, 4u, temp_hash, &next_player), MYGAMES_STORE_OK);
        assert(next_player == 104u);
        check(mygames_room_store_resume_session(
                  store, 4u, next_player, rotated_hash, later_hash,
                  60000u, &recovered_player, &recovered_generation),
              MYGAMES_STORE_NOT_FOUND); /* inclusive expiry boundary */
        check(mygames_room_store_resume_session(
                  store, 4u, next_player, rotated_hash, later_hash,
                  59999u, &recovered_player, &recovered_generation),
              MYGAMES_STORE_OK);
        assert(recovered_player == old_player && recovered_generation == 3u);
        check(mygames_room_store_detach_session(
                  store, 3u, old_player, 3u, 70000u),
              MYGAMES_STORE_STALE_EPOCH);
        check(mygames_room_store_detach_session(
                  store, 4u, old_player, 3u, 70000u),
              MYGAMES_STORE_OK);
        check(mygames_room_store_player_high_watermark(store, &next_player),
              MYGAMES_STORE_OK);
        assert(next_player == 104u);
        mygames_room_store_close(store);
        store = NULL;
    }

    corrupt_journal_with_turbodb(argv[1], argv[2]);
    /* State checkpoint is still byte-valid, but command/outbox sequence is
     * no longer consistent: fail closed without destroying persisted data. */
    check(mygames_room_store_open(argv[1], argv[2], &store),
          MYGAMES_STORE_CORRUPT);
    assert(store == NULL);

    /* Remove only the test's explicitly named DB, never native runtime data. */
    (void)remove(argv[2]);
    if (snprintf(sidecar, sizeof(sidecar), "%s-wal", argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    if (snprintf(sidecar, sizeof(sidecar), "%s-shm", argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    puts("mygames.turbodb_store: PASS");
    return 0;
}
