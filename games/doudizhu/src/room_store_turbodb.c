#include "mygames/room_store.h"

#include <orm.h>
#include <orm_runtime.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mygames_room_store {
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    bool quarantined;
#ifdef MYGAMES_STORE_FAULT_TESTING
    bool fail_after_checkpoint;
#endif
    char last_error[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static mygames_store_status error_from_orm(
    mygames_room_store *store, orm_status_t status, const orm_error_t *detail) {
    if (store) {
        const char *message = detail && detail->message[0] ?
            detail->message : orm_status_message(status);
        (void)snprintf(store->last_error, sizeof(store->last_error),
                       "TurboDB status=%d: %s", (int)status,
                       message ? message : "unknown");
        if (status == ORM_STATUS_COMMIT_UNKNOWN ||
            status == ORM_STATUS_CLEANUP_FAILED) store->quarantined = true;
    }
    return status == ORM_STATUS_COMMIT_UNKNOWN ?
        MYGAMES_STORE_COMMIT_UNKNOWN :
        status == ORM_STATUS_CLEANUP_FAILED ?
            MYGAMES_STORE_QUARANTINED : MYGAMES_STORE_BACKEND_ERROR;
}

static bool request_valid(const char *id) {
    size_t i;
    if (!id) return false;
    for (i = 0u; i <= MYGAMES_STORE_REQUEST_ID_MAX; ++i) {
        const unsigned char ch = (unsigned char)id[i];
        if (ch == '\0') return i != 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '.' || ch == ':')) return false;
    }
    return false;
}
static bool bounded_digest(const uint8_t *digest) {
    size_t i;
    uint8_t any = 0u;
    if (!digest) return false;
    for (i = 0u; i < MYGAMES_STORE_COMMAND_DIGEST_BYTES; ++i)
        any |= digest[i];
    return any != 0u;
}
static bool owner_valid(const mygames_room_store *store) {
    return store != NULL && store->runtime != NULL &&
           store->connection != NULL && !store->quarantined;
}
static mygames_store_status backend_query(
    mygames_room_store *store, orm_transaction_t *tx, const char *sql,
    const orm_value_t *params, size_t count, orm_result_t **out) {
    orm_error_t error;
    orm_query_t *q = NULL;
    orm_status_t status;
    size_t i;
    if (out) *out = NULL;
    orm_error_init(&error);
    status = orm_raw(store->connection, orm_view(sql), &q, &error);
    if (status == ORM_STATUS_OK) {
        for (i = 0u; i < count; ++i) {
            status = orm_query_bind(q, params[i], &error);
            if (status != ORM_STATUS_OK) break;
        }
    }
    if (status == ORM_STATUS_OK) {
        if (tx) status = orm_query_execute_in_transaction(q, tx, out, &error);
        else status = orm_query_execute(q, out, &error);
    }
    if (q) orm_query_destroy(q);
    if (status != ORM_STATUS_OK) {
        if (out && *out) { orm_result_destroy(*out); *out = NULL; }
        return error_from_orm(store, status, &error);
    }
    if (!out || !*out) {
        (void)snprintf(store->last_error, sizeof(store->last_error),
                       "TurboDB returned no materialized query result");
        return MYGAMES_STORE_BACKEND_ERROR;
    }
    return MYGAMES_STORE_OK;
}
static mygames_store_status execute(
    mygames_room_store *s, orm_transaction_t *tx, const char *sql,
    const orm_value_t *values, size_t n, uint64_t *affected) {
    orm_result_t *result = NULL;
    orm_error_t error;
    mygames_store_status st = backend_query(s, tx, sql, values, n, &result);
    orm_status_t rc;
    if (st != MYGAMES_STORE_OK) return st;
    if (affected) {
        orm_error_init(&error);
        rc = orm_result_affected_rows(result, affected, &error);
        if (rc != ORM_STATUS_OK) st = error_from_orm(s, rc, &error);
    }
    orm_result_destroy(result);
    return st;
}
static mygames_store_status begin_transaction(
    mygames_room_store *s, orm_transaction_t **out) {
    orm_error_t error;
    orm_status_t rc;
    *out = NULL;
    orm_error_init(&error);
    rc = orm_transaction_begin(s->connection, ORM_ISOLATION_SERIALIZABLE,
                               out, &error);
    return rc == ORM_STATUS_OK ? MYGAMES_STORE_OK :
                                 error_from_orm(s, rc, &error);
}
static mygames_store_status rollback(
    mygames_room_store *s, orm_transaction_t *tx,
    mygames_store_status original) {
    orm_error_t error;
    orm_status_t rc;
    orm_error_init(&error);
    rc = orm_transaction_rollback(tx, &error);
    orm_transaction_destroy(tx);
    if (rc != ORM_STATUS_OK) {
        s->quarantined = true;
        (void)error_from_orm(s, rc, &error);
        return MYGAMES_STORE_QUARANTINED;
    }
    return original;
}
static mygames_store_status commit_transaction(
    mygames_room_store *s, orm_transaction_t *tx) {
    orm_error_t error;
    orm_status_t rc;
    orm_error_init(&error);
    rc = orm_transaction_commit(tx, &error);
    if (rc == ORM_STATUS_OK) {
        orm_transaction_destroy(tx);
        return MYGAMES_STORE_OK;
    }
    if (rc == ORM_STATUS_COMMIT_UNKNOWN) {
        (void)error_from_orm(s, rc, &error);
        orm_transaction_destroy(tx);
        return MYGAMES_STORE_COMMIT_UNKNOWN;
    }
    /* Non-OK commit might still have touched native state. Rollback is the
     * only permitted terminal attempt. A failed rollback quarantines owner. */
    (void)error_from_orm(s, rc, &error);
    return rollback(s, tx, MYGAMES_STORE_BACKEND_ERROR);
}

typedef struct persisted_head {
    uint64_t epoch;
    uint64_t seq;
    uint8_t checkpoint[MYGAMES_CHECKPOINT_MAX_BYTES];
    size_t checkpoint_size;
} persisted_head;

static mygames_store_status head_read(
    mygames_room_store *s, orm_transaction_t *tx, persisted_head *head,
    bool *exists) {
    static const char sql[] =
        "select epoch, seq, image from mg_room_state where id=1";
    orm_result_t *result = NULL;
    orm_error_t error;
    int64_t e = 0, seq = 0;
    uint64_t count = 0u;
    orm_blob_t image = {0};
    mygames_store_status st = backend_query(s, tx, sql, NULL, 0u, &result);
    orm_status_t rc;
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(s, rc, &error);
        goto done;
    }
    if (count == 0u) {
        *exists = false;
        st = MYGAMES_STORE_OK;
        goto done;
    }
    if (count != 1u) { st = MYGAMES_STORE_CORRUPT; goto done; }
    rc = orm_result_get_int64(result, 0u, 0u, &e, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 1u, &seq, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_blob(result, 0u, 2u, &image, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(s, rc, &error);
        goto done;
    }
    if (e <= 0 || seq < 0 || !image.data ||
        image.size < MYGAMES_CHECKPOINT_HEADER_BYTES ||
        image.size > MYGAMES_CHECKPOINT_MAX_BYTES) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    head->epoch = (uint64_t)e;
    head->seq = (uint64_t)seq;
    head->checkpoint_size = image.size;
    memcpy(head->checkpoint, image.data, image.size);
    *exists = true;
done:
    orm_result_destroy(result);
    return st;
}
static mygames_store_status verify_head(const persisted_head *head,
                                         mygames_room_directory *out) {
    uint64_t embedded_epoch = 0u;
    mygames_room_directory temporary;
    if (mygames_room_checkpoint_decode(head->checkpoint, head->checkpoint_size,
                                      &temporary, &embedded_epoch)
            != MYGAMES_CHECKPOINT_OK || embedded_epoch != head->epoch)
        return MYGAMES_STORE_CORRUPT;
    if (out) *out = temporary;
    return MYGAMES_STORE_OK;
}
static mygames_store_status create_schema(mygames_room_store *s) {
    static const char *ddl[] = {
        "create table if not exists mg_room_state ("
        "id integer primary key check(id=1),"
        "epoch integer not null check(epoch>0),"
        "seq integer not null check(seq>=0),"
        "image blob not null)",
        "create table if not exists mg_commands ("
        "request_id text primary key, fingerprint blob not null,"
        "response blob not null, epoch integer not null, seq integer not null)",
        "create table if not exists mg_outbox ("
        "seq integer primary key, payload blob not null,"
        "delivered integer not null default 0 check(delivered in (0,1)))"
    };
    orm_transaction_t *tx = NULL;
    mygames_store_status st = begin_transaction(s, &tx);
    persisted_head head = {0};
    bool exists = false;
    size_t i;
    if (st != MYGAMES_STORE_OK) return st;
    for (i = 0u; i < sizeof(ddl) / sizeof(ddl[0]); ++i) {
        st = execute(s, tx, ddl[i], NULL, 0u, NULL);
        if (st != MYGAMES_STORE_OK) return rollback(s, tx, st);
    }
    st = head_read(s, tx, &head, &exists);
    if (st != MYGAMES_STORE_OK) return rollback(s, tx, st);
    if (!exists) {
        uint8_t initial[MYGAMES_CHECKPOINT_MAX_BYTES] = {0};
        mygames_room_directory empty;
        size_t bytes = 0u;
        orm_value_t args[3];
        mygames_room_directory_init(&empty);
        if (mygames_room_checkpoint_encode(&empty, 1u, initial,
                                           sizeof(initial), &bytes)
            != MYGAMES_CHECKPOINT_OK)
            return rollback(s, tx, MYGAMES_STORE_CORRUPT);
        args[0] = orm_i64(1);
        args[1] = orm_i64(0);
        args[2] = orm_blob(initial, bytes);
        st = execute(s, tx,
            "insert into mg_room_state(id,epoch,seq,image) values(1,?1,?2,?3)",
            args, 3u, NULL);
    } else st = verify_head(&head, NULL);
    if (st != MYGAMES_STORE_OK) return rollback(s, tx, st);
    return commit_transaction(s, tx);
}
static mygames_store_status prepare_sqlite(mygames_room_store *s) {
    mygames_store_status st;
    orm_result_t *result = NULL;
    orm_string_view_t mode = {0};
    orm_error_t error;
    orm_status_t rc;
    /* No silent reduced durability: successful PRAGMA must confirm WAL.
     * SQLite PRAGMAs are run OUTSIDE any explicit transaction. */
    st = backend_query(s, NULL, "pragma journal_mode=WAL",
                       NULL, 0u, &result);
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_get_text(result, 0u, 0u, &mode, &error);
    if (rc == ORM_STATUS_OK &&
        (mode.len != 3u || memcmp(mode.data, "wal", 3u) != 0))
        st = MYGAMES_STORE_BACKEND_ERROR;
    else if (rc != ORM_STATUS_OK) st = error_from_orm(s, rc, &error);
    orm_result_destroy(result);
    if (st != MYGAMES_STORE_OK) return st;
    return execute(s, NULL, "pragma synchronous=FULL",
                   NULL, 0u, NULL);
}

mygames_store_status mygames_room_store_open(
    const char *sqlite_plugin_absolute_path, const char *database_path,
    mygames_room_store **out_store) {
    mygames_room_store *s;
    orm_runtime_config_t runtime_config;
    orm_driver_load_config_t load = {0};
    orm_config_t connection_config;
    orm_option_t option;
    orm_error_t error;
    orm_status_t rc;
    mygames_store_status st;
    if (!out_store || !sqlite_plugin_absolute_path ||
        !sqlite_plugin_absolute_path[0] || !database_path ||
        !database_path[0] || strcmp(database_path, ":memory:") == 0)
        return MYGAMES_STORE_INVALID;
    *out_store = NULL;
    s = (mygames_room_store *)calloc(1u, sizeof(*s));
    if (!s) return MYGAMES_STORE_BACKEND_ERROR;
    orm_runtime_config_init(&runtime_config);
    orm_error_init(&error);
    rc = orm_runtime_create(&runtime_config, &s->runtime, &error);
    if (rc != ORM_STATUS_OK) { st = error_from_orm(s, rc, &error); goto fail; }
    load.struct_size = (uint32_t)sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(sqlite_plugin_absolute_path);
    load.expected_driver_id = orm_view("sqlite");
    rc = orm_runtime_load_driver(s->runtime, &load, &error);
    if (rc != ORM_STATUS_OK) { st = error_from_orm(s, rc, &error); goto fail; }
    orm_config(&connection_config);
    option.keyword = orm_view("filename");
    option.value = orm_view(database_path);
    connection_config.driver = orm_view("sqlite");
    connection_config.options = &option;
    connection_config.option_count = 1u;
    rc = orm_runtime_connect(s->runtime, &connection_config,
                             &s->connection, &error);
    if (rc != ORM_STATUS_OK) { st = error_from_orm(s, rc, &error); goto fail; }
    st = prepare_sqlite(s);
    if (st != MYGAMES_STORE_OK) goto fail;
    st = create_schema(s);
    if (st != MYGAMES_STORE_OK) goto fail;
    *out_store = s;
    return MYGAMES_STORE_OK;
fail:
    mygames_room_store_close(s);
    return st;
}

void mygames_room_store_close(mygames_room_store *store) {
    orm_error_t error;
    if (!store) return;
    orm_error_init(&error);
    if (store->connection) {
        (void)orm_connection_close(store->connection, &error);
        orm_connection_release(store->connection);
    }
    if (store->runtime) {
        (void)orm_runtime_close(store->runtime, &error);
        orm_runtime_release(store->runtime);
    }
    memset(store, 0, sizeof(*store));
    free(store);
}
const char *mygames_room_store_last_error(const mygames_room_store *store) {
    return store ? store->last_error : "room store is NULL";
}

mygames_store_status mygames_room_store_load(
    mygames_room_store *store, mygames_room_directory *out_rooms,
    uint64_t *out_epoch, uint64_t *out_sequence) {
    persisted_head head = {0};
    mygames_room_directory decoded;
    bool exists = false;
    mygames_store_status st;
    if (!store || !out_rooms || !out_epoch || !out_sequence)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    st = head_read(store, NULL, &head, &exists);
    if (st != MYGAMES_STORE_OK) return st;
    if (!exists) return MYGAMES_STORE_UNINITIALIZED;
    st = verify_head(&head, &decoded);
    if (st != MYGAMES_STORE_OK) return st;
    *out_rooms = decoded;
    *out_epoch = head.epoch;
    *out_sequence = head.seq;
    return MYGAMES_STORE_OK;
}

mygames_store_status mygames_room_store_claim_epoch(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t *out_claimed_epoch) {
    persisted_head head = {0};
    mygames_room_directory decoded;
    orm_transaction_t *tx = NULL;
    mygames_store_status st;
    orm_value_t args[4];
    uint8_t image[MYGAMES_CHECKPOINT_MAX_BYTES];
    size_t bytes = 0u;
    uint64_t rows = 0u;
    bool exists = false;
    if (!store || !out_claimed_epoch || !expected_epoch ||
        expected_epoch >= (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    st = begin_transaction(store, &tx);
    if (st != MYGAMES_STORE_OK) return st;
    st = head_read(store, tx, &head, &exists);
    if (st != MYGAMES_STORE_OK) return rollback(store, tx, st);
    if (!exists) return rollback(store, tx, MYGAMES_STORE_UNINITIALIZED);
    if (head.epoch != expected_epoch)
        return rollback(store, tx, MYGAMES_STORE_STALE_EPOCH);
    st = verify_head(&head, &decoded);
    if (st != MYGAMES_STORE_OK) return rollback(store, tx, st);
    if (mygames_room_checkpoint_encode(&decoded, expected_epoch + 1u,
                                      image, sizeof(image), &bytes) != MYGAMES_CHECKPOINT_OK)
        return rollback(store, tx, MYGAMES_STORE_CORRUPT);
    args[0] = orm_i64((int64_t)(expected_epoch + 1u));
    args[1] = orm_blob(image, bytes);
    args[2] = orm_i64((int64_t)expected_epoch);
    args[3] = orm_i64((int64_t)head.seq);
    st = execute(store, tx,
        "update mg_room_state set epoch=?1,image=?2 "
        "where id=1 and epoch=?3 and seq=?4",
        args, 4u, &rows);
    if (st != MYGAMES_STORE_OK) return rollback(store, tx, st);
    if (rows != 1u)
        return rollback(store, tx, MYGAMES_STORE_STALE_EPOCH);
    st = commit_transaction(store, tx);
    if (st == MYGAMES_STORE_OK) *out_claimed_epoch = expected_epoch + 1u;
    return st;
}


typedef struct recorded_outcome {
    bool found;
    uint64_t sequence;
    uint8_t fingerprint[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    uint8_t response[MYGAMES_STORE_RESULT_MAX];
    size_t response_size;
} recorded_outcome;

static mygames_store_status find_outcome(
    mygames_room_store *store, orm_transaction_t *tx,
    const char *request_id, recorded_outcome *out) {
    orm_value_t key = orm_text(request_id);
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_blob_t fingerprint = {0}, response = {0};
    orm_status_t rc;
    int64_t sequence = 0;
    uint64_t rows = 0u;
    mygames_store_status status = backend_query(store, tx,
        "select fingerprint,response,seq from mg_commands where request_id=?1",
        &key, 1u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        goto done;
    }
    if (rows == 0u) {
        out->found = false;
        goto done;
    }
    if (rows != 1u) { status = MYGAMES_STORE_CORRUPT; goto done; }
    rc = orm_result_get_blob(result, 0u, 0u, &fingerprint, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_blob(result, 0u, 1u, &response, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 2u, &sequence, &error);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        goto done;
    }
    if (!fingerprint.data ||
        fingerprint.size != MYGAMES_STORE_COMMAND_DIGEST_BYTES ||
        response.size > MYGAMES_STORE_RESULT_MAX ||
        (response.size && !response.data) || sequence <= 0) {
        status = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    memcpy(out->fingerprint, fingerprint.data, fingerprint.size);
    if (response.size) memcpy(out->response, response.data, response.size);
    out->response_size = response.size;
    out->sequence = (uint64_t)sequence;
    out->found = true;
done:
    orm_result_destroy(result);
    return status;
}

mygames_store_status mygames_room_store_get_outcome(
    mygames_room_store *store, const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    void *result, size_t result_capacity, size_t *out_result_size,
    uint64_t *out_sequence) {
    recorded_outcome existing = {0};
    mygames_store_status status;
    if (!store || !request_valid(request_id) ||
        !bounded_digest(command_digest) || !out_result_size || !out_sequence ||
        (!result && result_capacity)) return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = find_outcome(store, NULL, request_id, &existing);
    if (status != MYGAMES_STORE_OK) return status;
    if (!existing.found) return MYGAMES_STORE_NOT_FOUND;
    if (memcmp(existing.fingerprint, command_digest,
               MYGAMES_STORE_COMMAND_DIGEST_BYTES) != 0)
        return MYGAMES_STORE_REQUEST_CONFLICT;
    if (result_capacity < existing.response_size) {
        *out_result_size = existing.response_size;
        return MYGAMES_STORE_CAPACITY;
    }
    if (existing.response_size && !result) return MYGAMES_STORE_INVALID;
    if (existing.response_size)
        memcpy(result, existing.response, existing.response_size);
    *out_result_size = existing.response_size;
    *out_sequence = existing.sequence;
    return MYGAMES_STORE_OK;
}

mygames_store_status mygames_room_store_commit(
    mygames_room_store *store, const mygames_room_directory *post_command_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    const void *event_payload, size_t event_bytes,
    uint64_t *out_committed_sequence) {
    orm_transaction_t *tx = NULL;
    persisted_head head = {0};
    recorded_outcome prior = {0};
    uint8_t image[MYGAMES_CHECKPOINT_MAX_BYTES];
    size_t image_size = 0u;
    orm_value_t args[6];
    mygames_store_status status;
    uint64_t affected = 0u;
    uint64_t next_seq;
    bool exists = false;
    if (!store || !post_command_rooms || !out_committed_sequence ||
        !request_valid(request_id) || !bounded_digest(command_digest) ||
        (!response && response_bytes) || response_bytes > MYGAMES_STORE_RESULT_MAX ||
        (!event_payload && event_bytes) || event_bytes == 0u ||
        event_bytes > MYGAMES_STORE_EVENT_MAX ||
        expected_epoch == 0u || expected_epoch > (uint64_t)INT64_MAX ||
        expected_sequence >= (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    if (mygames_room_checkpoint_encode(
            post_command_rooms, expected_epoch, image, sizeof(image), &image_size)
        != MYGAMES_CHECKPOINT_OK) return MYGAMES_STORE_INVALID;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = head_read(store, tx, &head, &exists);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (!exists) return rollback(store, tx, MYGAMES_STORE_UNINITIALIZED);
    status = verify_head(&head, NULL);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (head.epoch != expected_epoch)
        return rollback(store, tx, MYGAMES_STORE_STALE_EPOCH);
    status = find_outcome(store, tx, request_id, &prior);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (prior.found) {
        const bool same_digest = memcmp(prior.fingerprint, command_digest,
                                      MYGAMES_STORE_COMMAND_DIGEST_BYTES) == 0;
        const bool same_response = prior.response_size == response_bytes &&
            (response_bytes == 0u || memcmp(prior.response, response, response_bytes) == 0);
        status = same_digest && same_response ?
                 MYGAMES_STORE_REPLAY : MYGAMES_STORE_REQUEST_CONFLICT;
        status = rollback(store, tx, status);
        if (status == MYGAMES_STORE_REPLAY)
            *out_committed_sequence = prior.sequence;
        return status;
    }
    if (head.seq != expected_sequence)
        return rollback(store, tx, MYGAMES_STORE_STALE_SEQUENCE);
    next_seq = head.seq + 1u;
    args[0] = orm_i64((int64_t)next_seq);
    args[1] = orm_blob(image, image_size);
    args[2] = orm_i64((int64_t)expected_epoch);
    args[3] = orm_i64((int64_t)expected_sequence);
    status = execute(store, tx,
        "update mg_room_state set seq=?1,image=?2 "
        "where id=1 and epoch=?3 and seq=?4",
        args, 4u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u)
        return rollback(store, tx, MYGAMES_STORE_STALE_SEQUENCE);

#ifdef MYGAMES_STORE_FAULT_TESTING
    if (store->fail_after_checkpoint) {
        store->fail_after_checkpoint = false;
        return rollback(store, tx, MYGAMES_STORE_BACKEND_ERROR);
    }
#endif

    args[0] = orm_text(request_id);
    args[1] = orm_blob(command_digest, MYGAMES_STORE_COMMAND_DIGEST_BYTES);
    args[2] = orm_blob(response, response_bytes);
    args[3] = orm_i64((int64_t)expected_epoch);
    args[4] = orm_i64((int64_t)next_seq);
    status = execute(store, tx,
        "insert into mg_commands(request_id,fingerprint,response,epoch,seq) "
        "values(?1,?2,?3,?4,?5)",
        args, 5u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u) return rollback(store, tx, MYGAMES_STORE_CORRUPT);

    args[0] = orm_i64((int64_t)next_seq);
    args[1] = orm_blob(event_payload, event_bytes);
    status = execute(store, tx,
        "insert into mg_outbox(seq,payload,delivered) values(?1,?2,0)",
        args, 2u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u) return rollback(store, tx, MYGAMES_STORE_CORRUPT);

    status = commit_transaction(store, tx);
    if (status == MYGAMES_STORE_OK) *out_committed_sequence = next_seq;
    return status;
}

mygames_store_status mygames_room_store_next_event(
    mygames_room_store *store, uint64_t after_sequence,
    void *payload, size_t capacity, size_t *out_size, uint64_t *out_sequence) {
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_value_t arg;
    orm_blob_t value = {0};
    orm_status_t rc;
    int64_t sequence = 0;
    uint64_t count = 0u;
    mygames_store_status status;
    if (!store || !out_size || !out_sequence ||
        (!payload && capacity) || after_sequence > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    arg = orm_i64((int64_t)after_sequence);
    status = backend_query(store, NULL,
        "select seq,payload from mg_outbox "
        "where delivered=0 and seq>?1 order by seq asc limit 1",
        &arg, 1u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        goto done;
    }
    if (count == 0u) { status = MYGAMES_STORE_NOT_FOUND; goto done; }
    if (count != 1u) { status = MYGAMES_STORE_CORRUPT; goto done; }
    rc = orm_result_get_int64(result, 0u, 0u, &sequence, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_blob(result, 0u, 1u, &value, &error);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        goto done;
    }
    if (sequence <= 0 || value.size == 0u ||
        value.size > MYGAMES_STORE_EVENT_MAX || !value.data) {
        status = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    if (capacity < value.size) {
        *out_size = value.size;
        status = MYGAMES_STORE_CAPACITY;
        goto done;
    }
    if (!payload) { status = MYGAMES_STORE_INVALID; goto done; }
    memcpy(payload, value.data, value.size);
    *out_size = value.size;
    *out_sequence = (uint64_t)sequence;
done:
    orm_result_destroy(result);
    return status;
}

mygames_store_status mygames_room_store_ack_event(
    mygames_room_store *store, uint64_t epoch, uint64_t sequence) {
    persisted_head head = {0};
    orm_transaction_t *tx = NULL;
    orm_value_t arg;
    mygames_store_status status;
    uint64_t affected = 0u;
    bool exists = false;
    if (!store || epoch == 0u || epoch > (uint64_t)INT64_MAX ||
        sequence == 0u || sequence > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = head_read(store, tx, &head, &exists);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (!exists) return rollback(store, tx, MYGAMES_STORE_UNINITIALIZED);
    if (head.epoch != epoch)
        return rollback(store, tx, MYGAMES_STORE_STALE_EPOCH);
    if (sequence > head.seq)
        return rollback(store, tx, MYGAMES_STORE_STALE_SEQUENCE);
    arg = orm_i64((int64_t)sequence);
    status = execute(store, tx,
        "update mg_outbox set delivered=1 where seq=?1 and delivered=0",
        &arg, 1u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    return commit_transaction(store, tx);
}

#ifdef MYGAMES_STORE_FAULT_TESTING
void mygames_room_store_fail_after_checkpoint_once(mygames_room_store *store) {
    if (store) store->fail_after_checkpoint = true;
}
#endif
