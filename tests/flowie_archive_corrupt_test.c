/* Test-only TurboDB fault injector for retained committed Outbox rows.
 * Never installed as an application executable. The sole purpose is proving
 * the privileged reconciliation path rejects private and forged history.
 */
#include <orm.h>
#include <orm_runtime.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    orm_runtime_t *runtime = NULL;
    orm_connection_t *conn = NULL;
    orm_result_t *result = NULL;
    orm_query_t *query = NULL;
    orm_runtime_config_t runtime_config;
    orm_driver_load_config_t load = {0};
    orm_config_t config;
    orm_option_t option;
    orm_error_t error;
    uint64_t affected = 0u;
    const char *payload;
    static const char private_payload[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":1,\"seq\":1,"
        "\"resume_token\":\"DUMMY-NOT-A-REAL-CREDENTIAL\"}";
    static const char changed_payload[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":777,\"seq\":1}";
    if (argc != 4) {
        fprintf(stderr, "usage: corrupt_outbox sqlite-driver database "
                        "--private|--forged-valid\n");
        return 2;
    }
    if (strcmp(argv[3], "--private") == 0) payload = private_payload;
    else if (strcmp(argv[3], "--forged-valid") == 0) payload = changed_payload;
    else return 2;
    orm_runtime_config_init(&runtime_config);
    orm_error_init(&error);
    assert(orm_runtime_create(&runtime_config, &runtime, &error) == ORM_STATUS_OK);
    load.struct_size = (uint32_t)sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(argv[1]);
    load.expected_driver_id = orm_view("sqlite");
    assert(orm_runtime_load_driver(runtime, &load, &error) == ORM_STATUS_OK);
    orm_config(&config);
    option.keyword = orm_view("filename");
    option.value = orm_view(argv[2]);
    config.driver = orm_view("sqlite");
    config.options = &option;
    config.option_count = 1u;
    assert(orm_runtime_connect(runtime, &config, &conn, &error) == ORM_STATUS_OK);
    assert(orm_raw(conn,
        orm_view("update mg_outbox set payload=?1 where seq=1"),
        &query, &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_blob(payload, strlen(payload)), &error)
           == ORM_STATUS_OK);
    assert(orm_query_execute(query, &result, &error) == ORM_STATUS_OK);
    assert(orm_result_affected_rows(result, &affected, &error) == ORM_STATUS_OK);
    assert(affected == 1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    assert(orm_connection_close(conn, &error) == ORM_STATUS_OK);
    orm_connection_release(conn);
    assert(orm_runtime_close(runtime, &error) == ORM_STATUS_OK);
    orm_runtime_release(runtime);
    puts("mygames.archive_corrupt_fixture: PASS");
    return 0;
}
