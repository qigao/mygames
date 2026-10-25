#include "mygames/consumer_projection.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

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
    (void)remove(argv[2]);
    if (snprintf(sidecar,sizeof(sidecar),"%s-wal",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    if (snprintf(sidecar,sizeof(sidecar),"%s-shm",argv[2]) < (int)sizeof(sidecar))
        (void)remove(sidecar);
    puts("mygames.consumer_projection: PASS");
    return 0;
}
