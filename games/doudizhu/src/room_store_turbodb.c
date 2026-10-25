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
    bool fail_after_command;
#endif
    char last_error[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static mygames_store_status error_from_orm(
    mygames_room_store *store, orm_status_t status, const orm_error_t *detail) {
    if (store) {
        const char *message = detail && detail->message[0] ?
            detail->message : orm_status_message(status);
        (void)snprintf(store->last_error, sizeof(store->last_error),
                       "TurboDB status=%d: %.420s", (int)status,
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
/* On re-open, fail closed if a checkpoint advanced without its durable
 * command/outbox counterparts, even when the binary checksum looks valid.
 * Per-sequence uniqueness is enforced by the SQLite schema itself. */
static mygames_store_status validate_journal(
    mygames_room_store *store, orm_transaction_t *tx, uint64_t seq) {
    static const char sql[] =
        "select (select count(*) from mg_commands),"
        "(select count(*) from mg_outbox),"
        "(select coalesce(min(seq),0) from mg_commands),"
        "(select coalesce(max(seq),0) from mg_commands),"
        "(select coalesce(min(seq),0) from mg_outbox),"
        "(select coalesce(max(seq),0) from mg_outbox)";
    orm_result_t *result = NULL;
    orm_error_t error;
    int64_t value[6] = {0};
    unsigned int col;
    uint64_t rows = 0u;
    orm_status_t rc;
    mygames_store_status st = backend_query(store, tx, sql, NULL, 0u, &result);
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(store, rc, &error);
        goto done;
    }
    if (rows != 1u) { st = MYGAMES_STORE_CORRUPT; goto done; }
    for (col = 0u; col < 6u; ++col) {
        rc = orm_result_get_int64(result, 0u, col, &value[col], &error);
        if (rc != ORM_STATUS_OK) {
            st = error_from_orm(store, rc, &error);
            goto done;
        }
        if (value[col] < 0) { st = MYGAMES_STORE_CORRUPT; goto done; }
    }
    if ((uint64_t)value[0] != seq || (uint64_t)value[1] != seq ||
        (uint64_t)value[3] != seq || (uint64_t)value[5] != seq ||
        (seq == 0u && (value[2] != 0 || value[4] != 0)) ||
        (seq != 0u && (value[2] != 1 || value[4] != 1))) {
        st = MYGAMES_STORE_CORRUPT;
    }
done:
    orm_result_destroy(result);
    return st;
}

static mygames_store_status create_schema(mygames_room_store *s) {
    static const char *ddl[] = {
        "create table if not exists mg_room_state ("
        "id integer primary key check(id=1),"
        "epoch integer not null check(epoch>0),"
        "seq integer not null check(seq>=0),"
        "image blob not null)",
        "create table if not exists mg_commands ("
        "player_id integer not null check(player_id>0),"
        "request_id text not null, fingerprint blob not null,"
        "response blob not null, epoch integer not null, seq integer not null unique,"
        "primary key(player_id,request_id))",
        "create table if not exists mg_outbox ("
        "seq integer primary key, payload blob not null,"
        "delivered integer not null default 0 check(delivered in (0,1)))",
        "create table if not exists mg_identity_alloc ("
        "id integer primary key check(id=1),"
        "last_issued integer not null check(last_issued>=0))",
        "create table if not exists mg_session_leases ("
        "player_id integer primary key check(player_id>0),"
        "ticket_sha256 blob not null unique check(length(ticket_sha256)=32),"
        "generation integer not null check(generation>0),"
        "attached integer not null check(attached in (0,1)),"
        "expires_at_ms integer not null check(expires_at_ms>=0))"
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
    /* Preserve player ID uniqueness across process restarts, even if a
     * provisional identity's lease has already been destroyed. */
    st = execute(s, tx,
        "insert or ignore into mg_identity_alloc(id,last_issued) "
        "values(1,(select coalesce(max(player_id),0) from mg_commands))",
        NULL, 0u, NULL);
    if (st != MYGAMES_STORE_OK) return rollback(s, tx, st);
    st = execute(s, tx,
        "update mg_identity_alloc set last_issued=max("
        "last_issued, "
        "(select coalesce(max(player_id),0) from mg_commands),"
        "(select coalesce(max(player_id),0) from mg_session_leases))"
        " where id=1",
        NULL, 0u, NULL);
    if (st != MYGAMES_STORE_OK) return rollback(s, tx, st);
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
    } else {
        st = verify_head(&head, NULL);
        if (st == MYGAMES_STORE_OK)
            st = validate_journal(s, tx, head.seq);
    }
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
    st = validate_journal(store, NULL, head.seq);
    if (st != MYGAMES_STORE_OK) return st;
    *out_rooms = decoded;
    *out_epoch = head.epoch;
    *out_sequence = head.seq;
    return MYGAMES_STORE_OK;
}

static bool ticket_hash_valid(
    const uint8_t hash[MYGAMES_STORE_TICKET_HASH_BYTES]) {
    uint8_t any = 0u;
    size_t i;
    if (!hash) return false;
    for (i = 0u; i < MYGAMES_STORE_TICKET_HASH_BYTES; ++i) any |= hash[i];
    return any != 0u;
}

static mygames_store_status transaction_require_epoch(
    mygames_room_store *store, orm_transaction_t *tx, uint64_t epoch) {
    persisted_head head = {0};
    mygames_store_status status;
    bool exists = false;
    status = head_read(store, tx, &head, &exists);
    if (status != MYGAMES_STORE_OK) return status;
    if (!exists) return MYGAMES_STORE_UNINITIALIZED;
    status = verify_head(&head, NULL);
    if (status != MYGAMES_STORE_OK) return status;
    return head.epoch == epoch ? MYGAMES_STORE_OK : MYGAMES_STORE_STALE_EPOCH;
}

mygames_store_status mygames_room_store_has_session(
    mygames_room_store *store, mygames_player_id player) {
    orm_result_t *result = NULL;
    orm_value_t value;
    orm_error_t error;
    orm_status_t rc;
    uint64_t rows = 0u;
    int64_t generation = 0;
    mygames_store_status status;
    if (!store || !player || player > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    value = orm_i64((int64_t)player);
    status = backend_query(store, NULL,
        "select generation from mg_session_leases where player_id=?1",
        &value, 1u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc == ORM_STATUS_OK && rows == 1u)
        rc = orm_result_get_int64(result, 0u, 0u, &generation, &error);
    if (rc != ORM_STATUS_OK) status = error_from_orm(store, rc, &error);
    else if (rows == 0u) status = MYGAMES_STORE_NOT_FOUND;
    else if (rows != 1u || generation <= 0)
        status = MYGAMES_STORE_CORRUPT;
    orm_result_destroy(result);
    return status;
}

mygames_store_status mygames_room_store_issue_session(
    mygames_room_store *store, uint64_t epoch,
    const uint8_t token_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    mygames_player_id *out_allocated_player) {
    orm_transaction_t *tx = NULL;
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_value_t args[3];
    orm_status_t rc;
    mygames_store_status status;
    uint64_t rows = 0u, changed = 0u;
    int64_t previous = 0;
    if (!store || !out_allocated_player || !ticket_hash_valid(token_hash) ||
        epoch == 0u || epoch > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = transaction_require_epoch(store, tx, epoch);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    status = backend_query(store, tx,
        "select last_issued from mg_identity_alloc where id=1",
        NULL, 0u, &result);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc == ORM_STATUS_OK && rows == 1u)
        rc = orm_result_get_int64(result, 0u, 0u, &previous, &error);
    orm_result_destroy(result);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        return rollback(store, tx, status);
    }
    if (rows != 1u || previous < 0 || previous >= INT64_MAX - 1)
        return rollback(store, tx, MYGAMES_STORE_CORRUPT);
    args[0] = orm_i64(previous + 1);
    args[1] = orm_i64(previous);
    status = execute(store, tx,
        "update mg_identity_alloc set last_issued=?1 "
        "where id=1 and last_issued=?2", args, 2u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u)
        return rollback(store, tx, MYGAMES_STORE_STALE_SEQUENCE);
    args[0] = orm_i64(previous + 1);
    args[1] = orm_blob(token_hash, MYGAMES_STORE_TICKET_HASH_BYTES);
    status = execute(store, tx,
        "insert into mg_session_leases("
        "player_id,ticket_sha256,generation,attached,expires_at_ms) "
        "values(?1,?2,1,1,0)", args, 2u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u)
        return rollback(store, tx, MYGAMES_STORE_CORRUPT);
    status = commit_transaction(store, tx);
    if (status == MYGAMES_STORE_OK)
        *out_allocated_player = (mygames_player_id)(previous + 1);
    return status;
}

mygames_store_status mygames_room_store_detach_session(
    mygames_room_store *store, uint64_t epoch, mygames_player_id player,
    uint64_t generation, uint64_t expires_at_realtime_ms) {
    orm_transaction_t *tx = NULL;
    orm_value_t args[3];
    uint64_t changed = 0u;
    mygames_store_status status;
    if (!store || !player || player > (uint64_t)INT64_MAX ||
        epoch == 0u || epoch > (uint64_t)INT64_MAX ||
        generation == 0u || generation > (uint64_t)INT64_MAX ||
        expires_at_realtime_ms == 0u ||
        expires_at_realtime_ms > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = transaction_require_epoch(store, tx, epoch);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    args[0] = orm_i64((int64_t)expires_at_realtime_ms);
    args[1] = orm_i64((int64_t)player);
    args[2] = orm_i64((int64_t)generation);
    status = execute(store, tx,
        "update mg_session_leases set attached=0,expires_at_ms=?1 "
        "where player_id=?2 and generation=?3 and attached=1",
        args, 3u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    return commit_transaction(store, tx);
}

mygames_store_status mygames_room_store_resume_session(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id temporary_player,
    const uint8_t old_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    const uint8_t rotated_hash[MYGAMES_STORE_TICKET_HASH_BYTES],
    uint64_t now_realtime_ms,
    mygames_player_id *out_player, uint64_t *out_generation) {
    orm_transaction_t *tx = NULL;
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    orm_value_t values[5];
    uint64_t rows = 0u, changed = 0u;
    int64_t player = 0, generation = 0, attached = 0, expires = 0;
    mygames_store_status status;
    if (!store || !temporary_player ||
        temporary_player > (uint64_t)INT64_MAX ||
        !ticket_hash_valid(old_hash) || !ticket_hash_valid(rotated_hash) ||
        memcmp(old_hash, rotated_hash, MYGAMES_STORE_TICKET_HASH_BYTES) == 0 ||
        epoch == 0u || epoch > (uint64_t)INT64_MAX ||
        now_realtime_ms == 0u || now_realtime_ms > (uint64_t)INT64_MAX ||
        !out_player || !out_generation)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = transaction_require_epoch(store, tx, epoch);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    values[0] = orm_blob(old_hash, MYGAMES_STORE_TICKET_HASH_BYTES);
    status = backend_query(store, tx,
        "select player_id,generation,attached,expires_at_ms "
        "from mg_session_leases where ticket_sha256=?1",
        values, 1u, &result);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc == ORM_STATUS_OK && rows == 1u) {
        rc = orm_result_get_int64(result, 0u, 0u, &player, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, 0u, 1u, &generation, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, 0u, 2u, &attached, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, 0u, 3u, &expires, &error);
    }
    orm_result_destroy(result);
    if (rc != ORM_STATUS_OK) {
        status = error_from_orm(store, rc, &error);
        return rollback(store, tx, status);
    }
    /* Same deliberately opaque outcome for invalid, expired, attached, or
     * already rotated credentials; never disclose principal existence. */
    if (rows != 1u || player <= 0 ||
        player == (int64_t)temporary_player ||
        generation <= 0 || generation >= INT64_MAX || attached != 0 ||
        expires <= (int64_t)now_realtime_ms)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    values[0] = orm_i64((int64_t)temporary_player);
    status = execute(store, tx,
        "delete from mg_session_leases where player_id=?1 and attached=1",
        values, 1u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    values[0] = orm_blob(rotated_hash, MYGAMES_STORE_TICKET_HASH_BYTES);
    values[1] = orm_i64(generation + 1);
    values[2] = orm_i64(player);
    values[3] = orm_blob(old_hash, MYGAMES_STORE_TICKET_HASH_BYTES);
    values[4] = orm_i64((int64_t)now_realtime_ms);
    status = execute(store, tx,
        "update mg_session_leases set ticket_sha256=?1,generation=?2,"
        "attached=1,expires_at_ms=0 "
        "where player_id=?3 and ticket_sha256=?4 and attached=0 "
        "and expires_at_ms>?5", values, 5u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    status = commit_transaction(store, tx);
    if (status == MYGAMES_STORE_OK) {
        *out_player = (mygames_player_id)player;
        *out_generation = (uint64_t)(generation + 1);
    }
    return status;
}

mygames_store_status mygames_room_store_discard_unsent_session(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id player, uint64_t generation) {
    orm_transaction_t *tx = NULL;
    orm_value_t args[2];
    uint64_t changed = 0u;
    mygames_store_status status;
    if (!store || !epoch || epoch > (uint64_t)INT64_MAX ||
        !player || player > (uint64_t)INT64_MAX ||
        !generation || generation > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = transaction_require_epoch(store, tx, epoch);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    args[0] = orm_i64((int64_t)player);
    args[1] = orm_i64((int64_t)generation);
    status = execute(store, tx,
        "delete from mg_session_leases where player_id=?1 "
        "and generation=?2 and attached=1",
        args, 2u, &changed);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (changed != 1u) return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    return commit_transaction(store, tx);
}

mygames_store_status mygames_room_store_next_expired_session(
    mygames_room_store *store, uint64_t now_realtime_ms,
    mygames_player_id after_player,
    mygames_player_id *out_player, uint64_t *out_generation) {
    orm_value_t args[2];
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    mygames_store_status status;
    uint64_t count = 0u;
    int64_t player = 0, generation = 0;
    if (!store || !out_player || !out_generation ||
        now_realtime_ms == 0u || now_realtime_ms > (uint64_t)INT64_MAX ||
        after_player > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    args[0] = orm_i64((int64_t)now_realtime_ms);
    args[1] = orm_i64((int64_t)after_player);
    status = backend_query(store, NULL,
        "select player_id,generation from mg_session_leases "
        "where attached=0 and expires_at_ms<=?1 and player_id>?2 "
        "order by player_id asc limit 1",
        args, 2u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc == ORM_STATUS_OK && count == 1u) {
        rc = orm_result_get_int64(result, 0u, 0u, &player, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, 0u, 1u, &generation, &error);
    }
    if (rc != ORM_STATUS_OK) status = error_from_orm(store, rc, &error);
    else if (count == 0u) status = MYGAMES_STORE_NOT_FOUND;
    else if (count != 1u || player <= 0 ||
             generation <= 0 || (uint64_t)player <= after_player)
        status = MYGAMES_STORE_CORRUPT;
    else {
        *out_player = (mygames_player_id)player;
        *out_generation = (uint64_t)generation;
    }
    orm_result_destroy(result);
    return status;
}

mygames_store_status mygames_room_store_delete_expired_unseated(
    mygames_room_store *store, uint64_t epoch,
    mygames_player_id player, uint64_t generation,
    uint64_t now_realtime_ms) {
    orm_transaction_t *tx = NULL;
    orm_value_t args[3];
    uint64_t affected = 0u;
    mygames_store_status status;
    if (!store || !epoch || epoch > (uint64_t)INT64_MAX ||
        !player || player > (uint64_t)INT64_MAX ||
        !generation || generation > (uint64_t)INT64_MAX ||
        !now_realtime_ms || now_realtime_ms > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = transaction_require_epoch(store, tx, epoch);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    args[0] = orm_i64((int64_t)player);
    args[1] = orm_i64((int64_t)generation);
    args[2] = orm_i64((int64_t)now_realtime_ms);
    status = execute(store, tx,
        "delete from mg_session_leases where player_id=?1 "
        "and generation=?2 and attached=0 and expires_at_ms<=?3",
        args, 3u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u)
        return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    return commit_transaction(store, tx);
}

mygames_store_status mygames_room_store_player_high_watermark(
    mygames_room_store *store, mygames_player_id *out_player) {
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    int64_t maximum = 0;
    uint64_t rows = 0u;
    mygames_store_status status;
    if (!store || !out_player) return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = backend_query(store, NULL,
        "select max(last_issued,"
        "(select coalesce(max(player_id),0) from mg_commands)) "
        "from mg_identity_alloc where id=1",
        NULL, 0u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc == ORM_STATUS_OK && rows == 1u)
        rc = orm_result_get_int64(result, 0u, 0u, &maximum, &error);
    if (rc != ORM_STATUS_OK) status = error_from_orm(store, rc, &error);
    else if (rows != 1u || maximum < 0 || maximum >= INT64_MAX)
        status = MYGAMES_STORE_CORRUPT;
    else *out_player = (mygames_player_id)maximum;
    orm_result_destroy(result);
    return status;
}

static mygames_store_status claim_epoch_internal(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t recovery_deadline, uint64_t *out_claimed_epoch) {
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
        expected_epoch >= (uint64_t)INT64_MAX ||
        recovery_deadline > (uint64_t)INT64_MAX)
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
    if (recovery_deadline) {
        orm_value_t deadline = orm_i64((int64_t)recovery_deadline);
        st = execute(store, tx,
            "update mg_session_leases set attached=0,expires_at_ms=?1 "
            "where attached=1",
            &deadline, 1u, NULL);
        if (st != MYGAMES_STORE_OK) return rollback(store, tx, st);
    }
    st = commit_transaction(store, tx);
    if (st == MYGAMES_STORE_OK) *out_claimed_epoch = expected_epoch + 1u;
    return st;
}

mygames_store_status mygames_room_store_claim_epoch(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t *out_claimed_epoch) {
    return claim_epoch_internal(store, expected_epoch, 0u, out_claimed_epoch);
}

mygames_store_status mygames_room_store_claim_epoch_for_recovery(
    mygames_room_store *store, uint64_t expected_epoch,
    uint64_t recovery_deadline_realtime_ms, uint64_t *out_claimed_epoch) {
    if (!recovery_deadline_realtime_ms) return MYGAMES_STORE_INVALID;
    return claim_epoch_internal(store, expected_epoch,
                                recovery_deadline_realtime_ms, out_claimed_epoch);
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
    mygames_player_id player_id,
    const char *request_id, recorded_outcome *out) {
    orm_value_t keys[2] = {
        orm_i64((int64_t)player_id), orm_text(request_id)
    };
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_blob_t fingerprint = {0}, response = {0};
    orm_status_t rc;
    int64_t sequence = 0;
    uint64_t rows = 0u;
    mygames_store_status status = backend_query(store, tx,
        "select fingerprint,response,seq from mg_commands "
        "where player_id=?1 and request_id=?2",
        keys, 2u, &result);
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
    mygames_room_store *store, mygames_player_id player_id,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    void *result, size_t result_capacity, size_t *out_result_size,
    uint64_t *out_sequence) {
    recorded_outcome existing = {0};
    mygames_store_status status;
    if (!store || !player_id || player_id > (uint64_t)INT64_MAX ||
        !request_valid(request_id) ||
        !bounded_digest(command_digest) || !out_result_size || !out_sequence ||
        (!result && result_capacity)) return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = find_outcome(store, NULL, player_id, request_id, &existing);
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

typedef struct optional_seated_expiry {
    mygames_player_id player;
    uint64_t generation;
    uint64_t now_ms;
} optional_seated_expiry;

static mygames_store_status commit_room_internal(
    mygames_room_store *store, mygames_player_id player_id,
    const mygames_room_directory *post_command_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    const void *event_payload, size_t event_bytes,
    uint64_t *out_committed_sequence,
    const optional_seated_expiry *expired) {
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
    if (!store || !player_id || player_id > (uint64_t)INT64_MAX ||
        !post_command_rooms || !out_committed_sequence ||
        !request_valid(request_id) || !bounded_digest(command_digest) ||
        (!response && response_bytes) || response_bytes > MYGAMES_STORE_RESULT_MAX ||
        (!event_payload && event_bytes) || event_bytes == 0u ||
        event_bytes > MYGAMES_STORE_EVENT_MAX ||
        expected_epoch == 0u || expected_epoch > (uint64_t)INT64_MAX ||
        expected_sequence >= (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    if (expired && (expired->player != player_id ||
        expired->generation == 0u || expired->generation > (uint64_t)INT64_MAX ||
        expired->now_ms == 0u || expired->now_ms > (uint64_t)INT64_MAX ||
        mygames_room_directory_get_player(post_command_rooms, player_id)))
        return MYGAMES_STORE_INVALID;
    if (mygames_room_checkpoint_encode(
            post_command_rooms, expected_epoch, image, sizeof(image), &image_size)
        != MYGAMES_CHECKPOINT_OK) return MYGAMES_STORE_INVALID;
    status = begin_transaction(store, &tx);
    if (status != MYGAMES_STORE_OK) return status;
    status = head_read(store, tx, &head, &exists);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (!exists) return rollback(store, tx, MYGAMES_STORE_UNINITIALIZED);
    if (expired) {
        mygames_room_directory previous_rooms;
        const mygames_room *prior_room;
        status = verify_head(&head, &previous_rooms);
        if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
        prior_room = mygames_room_directory_get_player(
            &previous_rooms, expired->player);
        if (!prior_room || prior_room->phase != MYGAMES_ROOM_WAITING)
            return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    } else {
        status = verify_head(&head, NULL);
        if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    }
    if (head.epoch != expected_epoch)
        return rollback(store, tx, MYGAMES_STORE_STALE_EPOCH);
    status = find_outcome(store, tx, player_id, request_id, &prior);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (prior.found) {
        const bool same_digest = memcmp(prior.fingerprint, command_digest,
                                      MYGAMES_STORE_COMMAND_DIGEST_BYTES) == 0;
        /* Never recompute the old response as part of retry admission.
         * Its immutable bytes belong to the existing transaction record. */
        status = same_digest ?
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

    if (expired) {
        /* Atomic with Checkpoint+command+Outbox: a recovered or active
         * lease cannot be deleted by stale timer work. */
        args[0] = orm_i64((int64_t)expired->player);
        args[1] = orm_i64((int64_t)expired->generation);
        args[2] = orm_i64((int64_t)expired->now_ms);
        status = execute(store, tx,
            "delete from mg_session_leases where player_id=?1 "
            "and generation=?2 and attached=0 and expires_at_ms<=?3",
            args, 3u, &affected);
        if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
        if (affected != 1u)
            return rollback(store, tx, MYGAMES_STORE_NOT_FOUND);
    }

    args[0] = orm_i64((int64_t)player_id);
    args[1] = orm_text(request_id);
    args[2] = orm_blob(command_digest, MYGAMES_STORE_COMMAND_DIGEST_BYTES);
    args[3] = orm_blob(response, response_bytes);
    args[4] = orm_i64((int64_t)expected_epoch);
    args[5] = orm_i64((int64_t)next_seq);
    status = execute(store, tx,
        "insert into mg_commands(player_id,request_id,fingerprint,response,epoch,seq) "
        "values(?1,?2,?3,?4,?5,?6)",
        args, 6u, &affected);
    if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    if (affected != 1u) return rollback(store, tx, MYGAMES_STORE_CORRUPT);

#ifdef MYGAMES_STORE_FAULT_TESTING
    if (store->fail_after_command) {
        store->fail_after_command = false;
        return rollback(store, tx, MYGAMES_STORE_BACKEND_ERROR);
    }
#endif

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

mygames_store_status mygames_room_store_commit(
    mygames_room_store *store, mygames_player_id player_id,
    const mygames_room_directory *post_command_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    const void *event_payload, size_t event_bytes,
    uint64_t *out_committed_sequence) {
    return commit_room_internal(
        store, player_id, post_command_rooms, expected_epoch, expected_sequence,
        request_id, command_digest, response, response_bytes,
        event_payload, event_bytes, out_committed_sequence, NULL);
}

mygames_store_status mygames_room_store_expire_seated(
    mygames_room_store *store,
    const mygames_room_directory *post_expiry_rooms,
    uint64_t expected_epoch, uint64_t expected_sequence,
    mygames_player_id expired_player, uint64_t lease_generation,
    uint64_t now_realtime_ms, const char *system_request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *outcome, size_t outcome_bytes,
    const void *sanitized_event, size_t event_bytes,
    uint64_t *out_committed_sequence) {
    const optional_seated_expiry expiry = {
        expired_player, lease_generation, now_realtime_ms
    };
    return commit_room_internal(
        store, expired_player, post_expiry_rooms,
        expected_epoch, expected_sequence,
        system_request_id, command_digest, outcome, outcome_bytes,
        sanitized_event, event_bytes, out_committed_sequence, &expiry);
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

mygames_store_status mygames_room_store_get_committed_event(
    mygames_room_store *store, uint64_t event_sequence,
    void *payload, size_t capacity, size_t *out_size) {
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    orm_value_t arg;
    orm_blob_t stored = {0};
    uint64_t row_count = 0u;
    int64_t stored_seq = 0;
    persisted_head head = {0};
    bool exists = false;
    mygames_store_status status;
    if (!store || !event_sequence || event_sequence > (uint64_t)INT64_MAX ||
        !out_size || (!payload && capacity)) return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    status = head_read(store, NULL, &head, &exists);
    if (status != MYGAMES_STORE_OK) return status;
    if (!exists) return MYGAMES_STORE_UNINITIALIZED;
    if (event_sequence > head.seq) return MYGAMES_STORE_NOT_FOUND;
    status = verify_head(&head, NULL);
    if (status != MYGAMES_STORE_OK) return status;
    arg = orm_i64((int64_t)event_sequence);
    status = backend_query(store, NULL,
        "select seq,payload from mg_outbox where seq=?1",
        &arg, 1u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &row_count, &error);
    if (rc == ORM_STATUS_OK && row_count == 1u) {
        rc = orm_result_get_int64(result, 0u, 0u, &stored_seq, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_blob(result, 0u, 1u, &stored, &error);
    }
    if (rc != ORM_STATUS_OK) status = error_from_orm(store, rc, &error);
    else if (row_count != 1u || stored_seq != (int64_t)event_sequence ||
             !stored.data || stored.size == 0u ||
             stored.size > MYGAMES_STORE_EVENT_MAX)
        status = MYGAMES_STORE_CORRUPT;
    else if (capacity < stored.size) {
        *out_size = stored.size;
        status = MYGAMES_STORE_CAPACITY;
    } else if (!payload) status = MYGAMES_STORE_INVALID;
    else {
        memcpy(payload, stored.data, stored.size);
        *out_size = stored.size;
        status = MYGAMES_STORE_OK;
    }
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
void mygames_room_store_fail_after_command_once(mygames_room_store *store) {
    if (store) store->fail_after_command = true;
}
#endif
