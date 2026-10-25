#include "mygames/consumer_projection.h"
#include <orm.h>
#include <orm_runtime.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Deliberately corrupt only this isolated test database via TurboDB,
 * without directly editing native SQLite files or bypassing ORM ownership. */
static void tamper_receipt_via_turbodb(const char *plugin, const char *database) {
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_runtime_config_t runtime_cfg;
    orm_driver_load_config_t load = {0};
    orm_config_t cfg;
    orm_option_t option;
    orm_error_t error;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    uint64_t removed = 0u;
    orm_runtime_config_init(&runtime_cfg);
    orm_error_init(&error);
    assert(orm_runtime_create(&runtime_cfg, &runtime, &error) == ORM_STATUS_OK);
    load.struct_size = sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(plugin);
    load.expected_driver_id = orm_view("sqlite");
    assert(orm_runtime_load_driver(runtime, &load, &error) == ORM_STATUS_OK);
    orm_config(&cfg);
    option.keyword = orm_view("filename");
    option.value = orm_view(database);
    cfg.driver = orm_view("sqlite");
    cfg.options = &option;
    cfg.option_count = 1u;
    assert(orm_runtime_connect(runtime, &cfg, &connection, &error) == ORM_STATUS_OK);
    assert(orm_raw(connection, orm_view(
        "delete from mg_consumer_receipt where source_namespace=?1 "
        "and consumer_id=?2 and event_seq=?3"),
        &query, &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_text("arena-west"), &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_text("dashboard"), &error) == ORM_STATUS_OK);
    assert(orm_query_bind(query, orm_i64(1), &error) == ORM_STATUS_OK);
    assert(orm_query_execute(query, &result, &error) == ORM_STATUS_OK);
    assert(orm_result_affected_rows(result, &removed, &error) == ORM_STATUS_OK);
    assert(removed == 1u);
    orm_result_destroy(result);
    orm_query_destroy(query);
    assert(orm_connection_close(connection, &error) == ORM_STATUS_OK);
    orm_connection_release(connection);
    assert(orm_runtime_close(runtime, &error) == ORM_STATUS_OK);
    orm_runtime_release(runtime);
}

static void assert_snapshot(mygames_consumer_projection *store,
                            const char *ns, const char *id,
                            uint64_t seq, uint64_t applied) {
    uint64_t s = UINT64_MAX, a = UINT64_MAX;
    assert(mygames_consumer_projection_snapshot(store, ns, id, &s, &a)
           == MYGAMES_CONSUMER_APPLIED);
    assert(s == seq && a == applied);
}
int main(int argc, char **argv) {
    mygames_consumer_projection *store = NULL;
    uint8_t first[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES];
    uint8_t second[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES];
    uint64_t count = 999u;
    char sidecar[1024];
    if (argc != 3) {
        fprintf(stderr, "usage: consumer_projection_test sqlite-plugin db\n");
        return 2;
    }
    (void)remove(argv[2]);
    if (snprintf(sidecar,sizeof(sidecar),"%s-wal",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    if (snprintf(sidecar,sizeof(sidecar),"%s-shm",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    memset(first, 0x31, sizeof(first));
    memset(second, 0x52, sizeof(second));

    assert(mygames_consumer_projection_open(argv[1],argv[2],&store)
           == MYGAMES_CONSUMER_APPLIED);
    assert_snapshot(store,"arena-west","dashboard",0u,0u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",3u,first,&count)
        == MYGAMES_CONSUMER_SEQUENCE_GAP);
    assert_snapshot(store,"arena-west","dashboard",0u,0u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",1u,first,&count)
        == MYGAMES_CONSUMER_APPLIED);
    assert(count == 1u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",1u,first,&count)
        == MYGAMES_CONSUMER_REPLAY);
    assert(count == 1u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",1u,second,&count)
        == MYGAMES_CONSUMER_CONFLICT);
    assert_snapshot(store,"arena-west","dashboard",1u,1u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",3u,second,&count)
        == MYGAMES_CONSUMER_SEQUENCE_GAP);

    mygames_consumer_projection_fail_after_receipt_once(store);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",2u,second,&count)
        == MYGAMES_CONSUMER_BACKEND_ERROR);
    assert_snapshot(store,"arena-west","dashboard",1u,1u);
    mygames_consumer_projection_close(store);
    store=NULL;
    /* A fully different ORM runtime on the same on-disk SQLite file confirms
     * that the injected partial receipt was completely rolled back. */
    assert(mygames_consumer_projection_open(argv[1],argv[2],&store)
           == MYGAMES_CONSUMER_APPLIED);
    assert_snapshot(store,"arena-west","dashboard",1u,1u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",2u,second,&count)
        == MYGAMES_CONSUMER_APPLIED);
    assert(count == 2u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",1u,first,&count)
        == MYGAMES_CONSUMER_REPLAY);
    assert(count == 2u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",2u,first,&count)
        == MYGAMES_CONSUMER_CONFLICT);
    assert_snapshot(store,"arena-west","dashboard",2u,2u);

    /* Namespaces and Consumer IDs are independent durable identities. */
    assert(mygames_consumer_projection_apply(
        store,"arena-east","dashboard",1u,second,&count)
        == MYGAMES_CONSUMER_APPLIED && count == 1u);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","metrics",1u,second,&count)
        == MYGAMES_CONSUMER_APPLIED && count == 1u);
    assert_snapshot(store,"arena-west","dashboard",2u,2u);
    assert_snapshot(store,"arena-east","dashboard",1u,1u);
    assert_snapshot(store,"arena-west","metrics",1u,1u);
    assert(mygames_consumer_projection_apply(
        store,"arena/west","dashboard",3u,first,&count)
        == MYGAMES_CONSUMER_INVALID);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",0u,first,&count)
        == MYGAMES_CONSUMER_INVALID);
    assert(mygames_consumer_projection_apply(
        store,"arena-west","dashboard",3u,NULL,&count)
        == MYGAMES_CONSUMER_INVALID);
    assert_snapshot(store,"arena-west","dashboard",2u,2u);
    mygames_consumer_projection_close(store);
    store = NULL;
    tamper_receipt_via_turbodb(argv[1], argv[2]);
    /* The cursor still says 2 applied effects, but one historical
     * receipt is absent. A future service must not trust this state. */
    assert(mygames_consumer_projection_open(argv[1], argv[2], &store)
           == MYGAMES_CONSUMER_CORRUPT);
    assert(store == NULL);
    (void)remove(argv[2]);
    if (snprintf(sidecar,sizeof(sidecar),"%s-wal",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    if (snprintf(sidecar,sizeof(sidecar),"%s-shm",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    puts("mygames.consumer_projection: PASS");
    return 0;
}
