#include "mygames/room_store.h"
#include "mygames/ddz_match_checkpoint.h"

#include <orm.h>
#include <orm_runtime.h>

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mygames_room_store {
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    bool quarantined;
    bool read_only;
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
    if (!s || !out) return MYGAMES_STORE_INVALID;
    *out = NULL;
    /* Fail before touching any SQL. SQLite open_mode=read_only provides the
     * independently enforced backend boundary for this connection. */
    if (s->read_only) return MYGAMES_STORE_READ_ONLY;
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
        "expires_at_ms integer not null check(expires_at_ms>=0))",
        "create table if not exists mg_game_state ("
        "room_id text primary key,"
        "epoch integer not null check(epoch>0),"
        "game_id integer not null check(game_id>0),"
        "seq integer not null check(seq>0),"
        "private_image blob not null check(length(private_image)=224))",
        /* Per-room/game immutable, nonmonetary outcome. A unique global
         * finish_seq couples this historical settlement to precisely one
         * committed CFlow Room/receipt/Outbox sequence. No private cards. */
        "create table if not exists mg_game_results ("
        "room_id text not null,"
        "game_id integer not null check(game_id>0),"
        "finish_seq integer not null unique check(finish_seq>0),"
        "p0 integer not null check(p0>0),"
        "p1 integer not null check(p1>0),"
        "p2 integer not null check(p2>0),"
        "base integer not null check(base between 1 and 3),"
        "bombs integer not null check(bombs between 0 and 14),"
        "spring integer not null check(spring between 0 and 2),"
        "landlord_seat integer not null check(landlord_seat between 0 and 2),"
        "winner_seat integer not null check(winner_seat between 0 and 2),"
        "multiplier integer not null check(multiplier>0),"
        "unit integer not null check(unit>0),"
        "d0 integer not null,"
        "d1 integer not null,"
        "d2 integer not null,"
        "primary key(room_id,game_id),"
        "check(p0<>p1 and p0<>p2 and p1<>p2),"
        "check(d0+d1+d2=0))"
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

static mygames_store_status room_store_open_internal(
    const char *sqlite_plugin_absolute_path, const char *database_path,
    bool read_only, mygames_room_store **out_store) {
    mygames_room_store *s;
    orm_runtime_config_t runtime_config;
    orm_driver_load_config_t load = {0};
    orm_config_t connection_config;
    orm_option_t options[2];
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
    s->read_only = read_only;
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
    options[0].keyword = orm_view("filename");
    options[0].value = orm_view(database_path);
    options[1].keyword = orm_view("open_mode");
    options[1].value = orm_view("read_only");
    connection_config.driver = orm_view("sqlite");
    connection_config.options = options;
    connection_config.option_count = read_only ? 2u : 1u;
    rc = orm_runtime_connect(s->runtime, &connection_config,
                             &s->connection, &error);
    if (rc != ORM_STATUS_OK) { st = error_from_orm(s, rc, &error); goto fail; }
    if (read_only) {
        mygames_room_directory verified;
        uint64_t epoch = 0u, sequence = 0u;
        /* Only SELECT; never initialize, repair, or PRAGMA-mutate source.
         * Journal contiguity and checkpoint integrity are mandatory. */
        st = mygames_room_store_load(s, &verified, &epoch, &sequence);
    } else {
        st = prepare_sqlite(s);
        if (st == MYGAMES_STORE_OK) st = create_schema(s);
    }
    if (st != MYGAMES_STORE_OK) goto fail;
    *out_store = s;
    return MYGAMES_STORE_OK;
fail:
    mygames_room_store_close(s);
    return st;
}

mygames_store_status mygames_room_store_open(
    const char *sqlite_plugin_absolute_path, const char *database_path,
    mygames_room_store **out_store) {
    return room_store_open_internal(
        sqlite_plugin_absolute_path, database_path, false, out_store);
}

mygames_store_status mygames_room_store_open_readonly(
    const char *sqlite_plugin_absolute_path, const char *database_path,
    mygames_room_store **out_store) {
    return room_store_open_internal(
        sqlite_plugin_absolute_path, database_path, true, out_store);
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

/* M5c: private game image is versioned C11 bytes, never a native C
 * struct or SQLite file opened by the application. All access stays on the
 * unique ORM Room Owner; SQL credentials are NOT client-visible. */
typedef struct stored_private_game {
    bool found;
    uint64_t epoch;
    uint64_t game_id;
    uint64_t sequence;
    uint8_t image[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    mygames_ddz_match match;
} stored_private_game;

static bool game_room_id_valid(const char *name) {
    size_t i;
    if (!name) return false;
    for (i = 0u; i < MYGAMES_ROOM_ID_CAPACITY; ++i) {
        const unsigned char ch = (unsigned char)name[i];
        if (!ch) return i != 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            return false;
    }
    return false;
}

static bool game_matches_room(const mygames_ddz_match *match,
                              const mygames_room *room) {
    size_t i;
    if (!match || !room || room->phase != MYGAMES_ROOM_STARTING ||
        room->players != MYGAMES_DDZ_SEATS) return false;
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        if (!room->seats[i].player || !room->seats[i].ready ||
            room->seats[i].player != match->players[i])
            return false;
    return true;
}

static mygames_store_status private_game_read(
    mygames_room_store *store, orm_transaction_t *tx,
    const char *room_id, stored_private_game *out) {
    const char *sql =
        "select epoch,game_id,seq,private_image "
        "from mg_game_state where room_id=?1";
    orm_value_t arg;
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_blob_t blob = {0};
    orm_status_t rc;
    mygames_store_status st;
    stored_private_game game = {0};
    uint64_t count = 0u;
    int64_t epoch = 0, game_id = 0, sequence = 0;
    if (!store || !game_room_id_valid(room_id) || !out)
        return MYGAMES_STORE_INVALID;
    arg = orm_text(room_id);
    st = backend_query(store, tx, sql, &arg, 1u, &result);
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(store, rc, &error);
        goto done;
    }
    if (count == 0u) goto done;
    if (count != 1u) { st = MYGAMES_STORE_CORRUPT; goto done; }
    rc = orm_result_get_int64(result, 0u, 0u, &epoch, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 1u, &game_id, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 2u, &sequence, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_blob(result, 0u, 3u, &blob, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(store, rc, &error);
        goto done;
    }
    if (epoch <= 0 || game_id <= 0 || sequence <= 0 || !blob.data ||
        blob.size != sizeof(game.image)) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    memcpy(game.image, blob.data, sizeof(game.image));
    if (mygames_ddz_checkpoint_decode(game.image, sizeof(game.image),
                                     &game.match) != MYGAMES_DDZ_CHECKPOINT_OK ||
        game.match.room_epoch != (uint64_t)epoch ||
        game.match.game_id != (uint64_t)game_id) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    game.epoch = (uint64_t)epoch;
    game.game_id = (uint64_t)game_id;
    game.sequence = (uint64_t)sequence;
    game.found = true;
done:
    orm_result_destroy(result);
    if (st == MYGAMES_STORE_OK) *out = game;
    return st;
}

/* Claiming a new Room Owner Epoch fences EVERY durable game in the same
 * transaction, alongside the public room checkpoint and leases. Do not
 * revive stale game epochs after restart or let a partial migration commit.
 * At most 32 private game images exist in this bounded process. */
static mygames_store_status rebind_private_games(
    mygames_room_store *store, orm_transaction_t *tx,
    const mygames_room_directory *rooms, uint64_t old_epoch) {
    struct game_rebind {
        char room_id[MYGAMES_ROOM_ID_CAPACITY];
        uint8_t image[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
        uint64_t sequence;
    } games[MYGAMES_ROOM_DIRECTORY_CAPACITY];
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    mygames_store_status st;
    uint64_t count = 0u;
    size_t i;
    st = backend_query(store, tx,
        "select room_id,epoch,game_id,seq,private_image "
        "from mg_game_state order by room_id", NULL, 0u, &result);
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(store, rc, &error);
        goto done;
    }
    if (count > MYGAMES_ROOM_DIRECTORY_CAPACITY) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    for (i = 0u; i < (size_t)count; ++i) {
        orm_string_view_t name = {0};
        orm_blob_t blob = {0};
        int64_t epoch = 0, id = 0, sequence = 0;
        mygames_ddz_match game = {0};
        size_t output = 0u;
        rc = orm_result_get_text(result, i, 0u, &name, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, i, 1u, &epoch, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, i, 2u, &id, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, i, 3u, &sequence, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_blob(result, i, 4u, &blob, &error);
        if (rc != ORM_STATUS_OK) {
            st = error_from_orm(store, rc, &error);
            goto done;
        }
        if (!name.data || name.len == 0u ||
            name.len >= MYGAMES_ROOM_ID_CAPACITY ||
            !blob.data || blob.size != MYGAMES_DDZ_CHECKPOINT_V2_BYTES ||
            epoch <= 0 || (uint64_t)epoch != old_epoch ||
            id <= 0 || sequence <= 0) {
            st = MYGAMES_STORE_CORRUPT;
            goto done;
        }
        memcpy(games[i].room_id, name.data, name.len);
        games[i].room_id[name.len] = '\0';
        if (!game_room_id_valid(games[i].room_id) ||
            mygames_ddz_checkpoint_decode(blob.data, blob.size, &game) !=
                MYGAMES_DDZ_CHECKPOINT_OK ||
            game.room_epoch != old_epoch || game.game_id != (uint64_t)id ||
            !game_matches_room(&game,
                mygames_room_directory_get(rooms, games[i].room_id))) {
            st = MYGAMES_STORE_CORRUPT;
            goto done;
        }
        game.room_epoch = old_epoch + 1u;
        if (mygames_ddz_checkpoint_encode(&game, games[i].image,
                sizeof(games[i].image), &output) != MYGAMES_DDZ_CHECKPOINT_OK ||
            output != sizeof(games[i].image)) {
            st = MYGAMES_STORE_CORRUPT;
            goto done;
        }
        games[i].sequence = (uint64_t)sequence;
    }
done:
    orm_result_destroy(result);
    if (st != MYGAMES_STORE_OK) return st;
    for (i = 0u; i < (size_t)count; ++i) {
        orm_value_t args[5];
        uint64_t affected = 0u;
        args[0] = orm_i64((int64_t)(old_epoch + 1u));
        args[1] = orm_blob(games[i].image, sizeof(games[i].image));
        args[2] = orm_text(games[i].room_id);
        args[3] = orm_i64((int64_t)old_epoch);
        args[4] = orm_i64((int64_t)games[i].sequence);
        st = execute(store, tx,
            "update mg_game_state set epoch=?1,private_image=?2 "
            "where room_id=?3 and epoch=?4 and seq=?5",
            args, 5u, &affected);
        if (st != MYGAMES_STORE_OK) return st;
        if (affected != 1u) return MYGAMES_STORE_STALE_EPOCH;
    }
    return MYGAMES_STORE_OK;
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
    /* Never claim an epoch while leaving an older process's private game
     * checkpoint bound to a stale Owner generation. This shares the SAME
     * atomic transaction with public Room checkpoint and lease fencing. */
    st = rebind_private_games(store, tx, &decoded, expected_epoch);
    if (st != MYGAMES_STORE_OK) return rollback(store, tx, st);
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

typedef struct optional_game_commit {
    const char *room_id;
    const mygames_ddz_match *previous;
    const mygames_ddz_match *candidate;
} optional_game_commit;

/* Validate actual move/bid transition by REEXECUTING the canonical M5b
 * immutable rule function, rather than trusting a client-supplied or
 * model-authored candidate. Reject every forged private-hand transition.
 * A totally fresh game requires either no prior game row, or a finished /
 * all-pass round with a strictly newer game ID and server CSPRNG deck. */
static bool game_transition_authorized(
    const mygames_ddz_match *prior, const mygames_ddz_match *candidate,
    mygames_player_id player) {
    mygames_ddz_match derived = {0};
    mygames_ddz_action_result move = {0};
    mygames_ddz_bid_result bid = {0};
    uint8_t expected[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    uint8_t actual[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    size_t expected_size = 0u, actual_size = 0u;
    unsigned int seat;
    if (!candidate || player == 0u ||
        mygames_ddz_match_validate(candidate) != MYGAMES_DDZ_MATCH_OK)
        return false;
    for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat)
        if (candidate->players[seat] == player) break;
    if (seat == MYGAMES_DDZ_SEATS) return false;

    if (!prior || candidate->game_id != prior->game_id) {
        if (candidate->phase != MYGAMES_DDZ_MATCH_BIDDING ||
            candidate->bid_count != 0u || candidate->bid_turn_id != 1u ||
            candidate->bid_start_seat != seat)
            return false;
        if (prior && (candidate->room_epoch != prior->room_epoch ||
            candidate->game_id <= prior->game_id ||
            (prior->phase != MYGAMES_DDZ_MATCH_FINISHED &&
             prior->phase != MYGAMES_DDZ_MATCH_REDEAL_REQUIRED)))
            return false;
        return true;
    }

    if (candidate->room_epoch != prior->room_epoch ||
        candidate->bid_start_seat != prior->bid_start_seat ||
        memcmp(candidate->players, prior->players, sizeof(prior->players)) != 0 ||
        memcmp(candidate->secret_deck, prior->secret_deck,
               sizeof(prior->secret_deck)) != 0 ||
        memcmp(candidate->initial_hands, prior->initial_hands,
               sizeof(prior->initial_hands)) != 0 ||
        candidate->secret_kitty != prior->secret_kitty)
        return false;
    if (prior->phase == MYGAMES_DDZ_MATCH_BIDDING) {
        const uint8_t score = candidate->highest_bid > prior->highest_bid ?
            candidate->highest_bid : 0u;
        if (seat != prior->bid_turn_seat ||
            mygames_ddz_match_bid(prior, prior->room_epoch, prior->game_id,
                                  player, prior->bid_turn_id, score,
                                  &derived, &bid) != MYGAMES_DDZ_MATCH_OK)
            return false;
    } else if (prior->phase == MYGAMES_DDZ_MATCH_PLAYING) {
        const uint64_t cards = candidate->round.played_cards &
                               ~prior->round.played_cards;
        if (seat != prior->round.turn_seat ||
            mygames_ddz_match_play(prior, prior->room_epoch, prior->game_id,
                                   player, prior->round.turn_id, cards,
                                   &derived, &move) != MYGAMES_DDZ_MATCH_OK)
            return false;
    } else return false;
    if (mygames_ddz_checkpoint_encode(&derived, expected,
            sizeof(expected), &expected_size) != MYGAMES_DDZ_CHECKPOINT_OK ||
        mygames_ddz_checkpoint_encode(candidate, actual,
            sizeof(actual), &actual_size) != MYGAMES_DDZ_CHECKPOINT_OK)
        return false;
    return expected_size == actual_size &&
           memcmp(expected, actual, expected_size) == 0;
}

static mygames_store_status game_store_candidate(
    mygames_room_store *store, orm_transaction_t *tx,
    const optional_game_commit *game,
    const mygames_room_directory *rooms,
    uint64_t epoch, uint64_t expected_sequence, uint64_t next_sequence,
    mygames_player_id authenticated_player) {
    stored_private_game existing = {0};
    uint8_t candidate_image[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    uint8_t prior_image[MYGAMES_DDZ_CHECKPOINT_V2_BYTES];
    size_t size = 0u, previous_size = 0u;
    orm_value_t args[6];
    mygames_store_status st;
    const mygames_room *room;
    uint64_t affected = 0u;
    if (!game || !game_room_id_valid(game->room_id) ||
        !game->candidate || !rooms ||
        epoch != game->candidate->room_epoch ||
        game->candidate->game_id > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    room = mygames_room_directory_get(rooms, game->room_id);
    if (!game_matches_room(game->candidate, room) ||
        !game_transition_authorized(game->previous, game->candidate,
                                    authenticated_player))
        return MYGAMES_STORE_INVALID;
    if (mygames_ddz_checkpoint_encode(game->candidate, candidate_image,
            sizeof(candidate_image), &size) != MYGAMES_DDZ_CHECKPOINT_OK ||
        size != sizeof(candidate_image))
        return MYGAMES_STORE_INVALID;

    st = private_game_read(store, tx, game->room_id, &existing);
    if (st != MYGAMES_STORE_OK) return st;
    if (existing.found) {
        if (!game->previous || existing.epoch != epoch ||
            existing.sequence > expected_sequence ||
            game->previous->game_id != existing.game_id ||
            mygames_ddz_checkpoint_encode(game->previous, prior_image,
                sizeof(prior_image), &previous_size) != MYGAMES_DDZ_CHECKPOINT_OK ||
            previous_size != sizeof(existing.image) ||
            memcmp(prior_image, existing.image, sizeof(existing.image)) != 0)
            return MYGAMES_STORE_STALE_GAME;
        args[0] = orm_i64((int64_t)epoch);
        args[1] = orm_i64((int64_t)game->candidate->game_id);
        args[2] = orm_i64((int64_t)next_sequence);
        args[3] = orm_blob(candidate_image, sizeof(candidate_image));
        args[4] = orm_text(game->room_id);
        args[5] = orm_i64((int64_t)existing.sequence);
        st = execute(store, tx,
            "update mg_game_state set epoch=?1,game_id=?2,seq=?3,private_image=?4 "
            "where room_id=?5 and seq=?6",
            args, 6u, &affected);
    } else {
        if (game->previous != NULL)
            return MYGAMES_STORE_STALE_GAME;
        args[0] = orm_text(game->room_id);
        args[1] = orm_i64((int64_t)epoch);
        args[2] = orm_i64((int64_t)game->candidate->game_id);
        args[3] = orm_i64((int64_t)next_sequence);
        args[4] = orm_blob(candidate_image, sizeof(candidate_image));
        st = execute(store, tx,
            "insert into mg_game_state(room_id,epoch,game_id,seq,private_image) "
            "values(?1,?2,?3,?4,?5)", args, 5u, &affected);
    }
    if (st != MYGAMES_STORE_OK) return st;
    if (affected != 1u) return MYGAMES_STORE_STALE_GAME;

    /* Only an actual accepted PLAYING -> FINISHED transition may create a
     * historical settlement. The same unique Room-Owner transaction also
     * commits the final private v2 image, exact Game ACK receipt and public
     * Outbox. No independent score transaction or application-side wallet. */
    if (game->previous &&
        game->previous->phase == MYGAMES_DDZ_MATCH_PLAYING &&
        game->candidate->phase == MYGAMES_DDZ_MATCH_FINISHED) {
        mygames_ddz_settlement score = {0};
        orm_value_t values[16];
        size_t i;
        if (mygames_ddz_match_settlement(game->candidate, &score) !=
                MYGAMES_DDZ_MATCH_OK)
            return MYGAMES_STORE_INVALID;
        for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
            if (game->candidate->players[i] == 0u ||
                game->candidate->players[i] > (uint64_t)INT64_MAX)
                return MYGAMES_STORE_INVALID;
        values[0] = orm_text(game->room_id);
        values[1] = orm_i64((int64_t)game->candidate->game_id);
        values[2] = orm_i64((int64_t)next_sequence);
        values[3] = orm_i64((int64_t)game->candidate->players[0]);
        values[4] = orm_i64((int64_t)game->candidate->players[1]);
        values[5] = orm_i64((int64_t)game->candidate->players[2]);
        values[6] = orm_i64((int64_t)score.bid_base);
        values[7] = orm_i64((int64_t)score.bombs_rockets);
        values[8] = orm_i64((int64_t)score.spring);
        values[9] = orm_i64((int64_t)score.landlord_seat);
        values[10] = orm_i64((int64_t)score.winner_seat);
        values[11] = orm_i64((int64_t)score.multiplier);
        values[12] = orm_i64((int64_t)score.unit);
        values[13] = orm_i64((int64_t)score.delta[0]);
        values[14] = orm_i64((int64_t)score.delta[1]);
        values[15] = orm_i64((int64_t)score.delta[2]);
        affected = 0u;
        st = execute(store, tx,
            "insert into mg_game_results("
            "room_id,game_id,finish_seq,p0,p1,p2,base,bombs,spring,"
            "landlord_seat,winner_seat,multiplier,unit,d0,d1,d2) "
            "values(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)",
            values, 16u, &affected);
        if (st != MYGAMES_STORE_OK) return st;
        if (affected != 1u) return MYGAMES_STORE_CORRUPT;
    }
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
    const optional_seated_expiry *expired,
    const optional_game_commit *game) {
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

    if (game) {
        status = game_store_candidate(store, tx, game, post_command_rooms,
                                      expected_epoch, expected_sequence,
                                      next_seq, player_id);
        if (status != MYGAMES_STORE_OK) return rollback(store, tx, status);
    }

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
        event_payload, event_bytes, out_committed_sequence, NULL, NULL);
}

/* This is a trusted Owner-only staging API. The public Outbox event is
 * derived solely from public scalar fields, not supplied as arbitrary JSON.
 * Private full-deck checkpoint, game receipt, Room checkpoint and public
 * Outbox are in the SAME SQLite WAL/FULL transaction. */
mygames_store_status mygames_room_store_commit_game(
    mygames_room_store *store, mygames_player_id player_id,
    const mygames_room_directory *post_command_rooms,
    const char *room_id,
    const mygames_ddz_match *expected_previous_match,
    const mygames_ddz_match *candidate_match,
    uint64_t expected_epoch, uint64_t expected_sequence,
    const char *request_id,
    const uint8_t command_digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES],
    const void *response, size_t response_bytes,
    uint64_t *out_committed_sequence) {
    char event[MYGAMES_STORE_EVENT_MAX];
    const optional_game_commit game = {
        room_id, expected_previous_match, candidate_match
    };
    uint64_t visible_turn;
    int count;
    if (!game_room_id_valid(room_id) || !candidate_match ||
        expected_sequence >= (uint64_t)INT64_MAX ||
        mygames_ddz_match_validate(candidate_match) != MYGAMES_DDZ_MATCH_OK)
        return MYGAMES_STORE_INVALID;
    visible_turn = candidate_match->phase == MYGAMES_DDZ_MATCH_BIDDING ||
                   candidate_match->phase == MYGAMES_DDZ_MATCH_REDEAL_REQUIRED ?
                   candidate_match->bid_turn_id : candidate_match->round.turn_id;
    count = snprintf(event, sizeof(event),
        "{\"v\":1,\"type\":\"game.state_committed\","
        "\"room_id\":\"%s\",\"room_epoch\":%" PRIu64 ","
        "\"game_id\":%" PRIu64 ",\"phase\":%u,"
        "\"turn_id\":%" PRIu64 ",\"seq\":%" PRIu64 "}",
        room_id, expected_epoch, candidate_match->game_id,
        (unsigned int)candidate_match->phase, visible_turn,
        expected_sequence + 1u);
    if (count <= 0 || (size_t)count >= sizeof(event))
        return MYGAMES_STORE_INVALID;
    return commit_room_internal(
        store, player_id, post_command_rooms,
        expected_epoch, expected_sequence, request_id, command_digest,
        response, response_bytes, event, (size_t)count,
        out_committed_sequence, NULL, &game);
}

/* Strictly checked immutable historical game result. Do not reuse a live
 * Game ACK or reexecute M5b to answer queries: this row belongs to the
 * already-confirmed finish COMMIT and survives a new game/owner epoch. */
static bool ledger_score_valid(const mygames_store_game_result *value) {
    const mygames_ddz_settlement *score;
    uint32_t expected_multiplier, expected_unit;
    bool landlord_won;
    unsigned int seat;
    if (!value || !value->game_id || !value->finish_sequence ||
        value->players[0] == 0u || value->players[1] == 0u ||
        value->players[2] == 0u || value->players[0] == value->players[1] ||
        value->players[0] == value->players[2] ||
        value->players[1] == value->players[2])
        return false;
    score = &value->score;
    if (score->bid_base < 1u || score->bid_base > 3u ||
        score->bombs_rockets > 14u ||
        score->spring > MYGAMES_DDZ_SPRING_FARMERS ||
        score->landlord_seat >= MYGAMES_DDZ_SEATS ||
        score->winner_seat >= MYGAMES_DDZ_SEATS)
        return false;
    landlord_won = score->landlord_seat == score->winner_seat;
    if ((score->spring == MYGAMES_DDZ_SPRING_LANDLORD && !landlord_won) ||
        (score->spring == MYGAMES_DDZ_SPRING_FARMERS && landlord_won))
        return false;
    expected_multiplier = UINT32_C(1) <<
        ((unsigned int)score->bombs_rockets +
         (score->spring != MYGAMES_DDZ_SPRING_NONE ? 1u : 0u));
    expected_unit = (uint32_t)score->bid_base * expected_multiplier;
    if (score->multiplier != expected_multiplier ||
        score->unit != expected_unit) return false;
    for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat) {
        const int32_t expected = seat == score->landlord_seat ?
            (landlord_won ? (int32_t)(expected_unit * 2u) :
                            -(int32_t)(expected_unit * 2u)) :
            (landlord_won ? -(int32_t)expected_unit :
                             (int32_t)expected_unit);
        if (score->delta[seat] != expected) return false;
    }
    return true;
}

mygames_store_status mygames_room_store_game_result(
    mygames_room_store *store, const char *room_id, uint64_t game_id,
    mygames_store_game_result *out_result) {
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_value_t args[2];
    orm_status_t rc = ORM_STATUS_OK;
    mygames_store_status status;
    mygames_store_game_result item = {0};
    uint64_t count = 0u;
    int64_t values[14] = {0};
    unsigned int i;
    if (!store || !out_result || !game_room_id_valid(room_id) ||
        game_id == 0u || game_id > (uint64_t)INT64_MAX)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    args[0] = orm_text(room_id);
    args[1] = orm_i64((int64_t)game_id);
    status = backend_query(store, NULL,
        "select finish_seq,p0,p1,p2,base,bombs,spring,"
        "landlord_seat,winner_seat,multiplier,unit,d0,d1,d2 "
        "from mg_game_results where room_id=?1 and game_id=?2",
        args, 2u, &result);
    if (status != MYGAMES_STORE_OK) return status;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc != ORM_STATUS_OK) goto failed;
    if (count == 0u) {
        status = MYGAMES_STORE_NOT_FOUND;
        goto done;
    }
    if (count != 1u) {
        status = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    for (i = 0u; i < 14u && rc == ORM_STATUS_OK; ++i)
        rc = orm_result_get_int64(result, 0u, i, &values[i], &error);
    if (rc != ORM_STATUS_OK) goto failed;
    if (values[0] <= 0 ||
        values[1] <= 0 || values[2] <= 0 || values[3] <= 0 ||
        values[4] < 1 || values[4] > 3 ||
        values[5] < 0 || values[5] > 14 ||
        values[6] < 0 || values[6] > 2 ||
        values[7] < 0 || values[7] > 2 ||
        values[8] < 0 || values[8] > 2 ||
        values[9] < 1 || values[9] > INT32_MAX ||
        values[10] < 1 || values[10] > INT32_MAX ||
        values[11] < INT32_MIN || values[11] > INT32_MAX ||
        values[12] < INT32_MIN || values[12] > INT32_MAX ||
        values[13] < INT32_MIN || values[13] > INT32_MAX) {
        status = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    item.game_id = game_id;
    item.finish_sequence = (uint64_t)values[0];
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        item.players[i] = (mygames_player_id)values[1u + i];
    item.score.bid_base = (uint8_t)values[4];
    item.score.bombs_rockets = (uint8_t)values[5];
    item.score.spring = (mygames_ddz_spring_kind)values[6];
    item.score.landlord_seat = (uint8_t)values[7];
    item.score.winner_seat = (uint8_t)values[8];
    item.score.multiplier = (uint32_t)values[9];
    item.score.unit = (uint32_t)values[10];
    for (i = 0u; i < MYGAMES_DDZ_SEATS; ++i)
        item.score.delta[i] = (int32_t)values[11u + i];
    if (!ledger_score_valid(&item)) {
        status = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    *out_result = item;
    status = MYGAMES_STORE_OK;
    goto done;
failed:
    status = error_from_orm(store, rc, &error);
done:
    orm_result_destroy(result);
    return status;
}

/* Read-only aggregate derived entirely from settled per-game facts,
 * never from pending Room state or a mutable account wallet. Individual
 * scores are simple room/game points; no monetary authority is implied. */
mygames_store_status mygames_room_store_player_game_points(
    mygames_room_store *store, const char *room_id, mygames_player_id player_id,
    int64_t *out_points, uint64_t *out_finished_games) {
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_value_t args[2];
    orm_status_t rc;
    mygames_store_status st;
    uint64_t row_count = 0u;
    int64_t points = 0, matches = 0;
    if (!store || !game_room_id_valid(room_id) ||
        !player_id || player_id > (uint64_t)INT64_MAX ||
        !out_points || !out_finished_games)
        return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    args[0] = orm_text(room_id);
    args[1] = orm_i64((int64_t)player_id);
    st = backend_query(store, NULL,
        "select coalesce(sum(case when p0=?2 then d0 "
        "when p1=?2 then d1 when p2=?2 then d2 else 0 end),0),"
        "count(*) from mg_game_results where room_id=?1 "
        "and (p0=?2 or p1=?2 or p2=?2)",
        args, 2u, &result);
    if (st != MYGAMES_STORE_OK) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &row_count, &error);
    if (rc == ORM_STATUS_OK && row_count != 1u) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 0u, &points, &error);
    if (rc == ORM_STATUS_OK)
        rc = orm_result_get_int64(result, 0u, 1u, &matches, &error);
    if (rc != ORM_STATUS_OK) {
        st = error_from_orm(store, rc, &error);
        goto done;
    }
    if (matches < 0) {
        st = MYGAMES_STORE_CORRUPT;
        goto done;
    }
    *out_points = points;
    *out_finished_games = (uint64_t)matches;
    st = MYGAMES_STORE_OK;
done:
    orm_result_destroy(result);
    return st;
}

mygames_store_status mygames_room_store_load_game(
    mygames_room_store *store, const char *room_id,
    mygames_ddz_match *out_match, uint64_t *out_sequence) {
    mygames_room_directory rooms;
    stored_private_game game = {0};
    persisted_head head = {0};
    mygames_store_status st;
    bool exists = false;
    if (!store || !game_room_id_valid(room_id) ||
        !out_match || !out_sequence) return MYGAMES_STORE_INVALID;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    st = head_read(store, NULL, &head, &exists);
    if (st != MYGAMES_STORE_OK) return st;
    if (!exists) return MYGAMES_STORE_UNINITIALIZED;
    st = verify_head(&head, &rooms);
    if (st != MYGAMES_STORE_OK) return st;
    st = validate_journal(store, NULL, head.seq);
    if (st != MYGAMES_STORE_OK) return st;
    st = private_game_read(store, NULL, room_id, &game);
    if (st != MYGAMES_STORE_OK) return st;
    if (!game.found) return MYGAMES_STORE_NOT_FOUND;
    if (game.epoch != head.epoch || game.sequence > head.seq ||
        !game_matches_room(&game.match,
            mygames_room_directory_get(&rooms, room_id)))
        return MYGAMES_STORE_CORRUPT;
    if (game.match.phase == MYGAMES_DDZ_MATCH_FINISHED) {
        /* A committed terminal private image without its corresponding
         * immutable per-game result is a CORRUPT ledger, not an unsolved
         * match or a cue to replay/pay the scoring transaction. */
        mygames_store_game_result historical = {0};
        mygames_ddz_settlement calculated = {0};
        unsigned int seat;
        st = mygames_room_store_game_result(store, room_id,
                                           game.game_id, &historical);
        if (st == MYGAMES_STORE_NOT_FOUND) return MYGAMES_STORE_CORRUPT;
        if (st != MYGAMES_STORE_OK) return st;
        if (historical.finish_sequence != game.sequence ||
            mygames_ddz_match_settlement(&game.match, &calculated) !=
                MYGAMES_DDZ_MATCH_OK ||
            calculated.bid_base != historical.score.bid_base ||
            calculated.bombs_rockets != historical.score.bombs_rockets ||
            calculated.spring != historical.score.spring ||
            calculated.multiplier != historical.score.multiplier ||
            calculated.unit != historical.score.unit ||
            calculated.landlord_seat != historical.score.landlord_seat ||
            calculated.winner_seat != historical.score.winner_seat)
            return MYGAMES_STORE_CORRUPT;
        for (seat = 0u; seat < MYGAMES_DDZ_SEATS; ++seat)
            if (historical.players[seat] != game.match.players[seat] ||
                historical.score.delta[seat] != calculated.delta[seat])
                return MYGAMES_STORE_CORRUPT;
    }
    *out_match = game.match;
    *out_sequence = game.sequence;
    return MYGAMES_STORE_OK;
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
        sanitized_event, event_bytes, out_committed_sequence, &expiry, NULL);
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

/* This function intentionally uses a SQLite *read* transaction on a
 * physical SQLITE_OPEN_READONLY handle. Separate head/event SELECTs are not
 * sufficient to promise a consistent page during concurrent Room commits.
 * Do not use begin_transaction(), which is reserved for write ownership. */
mygames_store_status mygames_room_store_read_archive_page(
    mygames_room_store *store, uint64_t after_sequence,
    uint32_t max_events, mygames_store_archive_page *out_page) {
    orm_transaction_t *tx = NULL;
    orm_error_t error;
    orm_status_t rc;
    persisted_head head = {0};
    mygames_store_archive_page page = {0};
    mygames_store_status status;
    bool exists = false;
    uint64_t available, limit;
    uint64_t i;

    if (!store || !out_page || after_sequence > (uint64_t)INT64_MAX ||
        max_events == 0u || max_events > MYGAMES_STORE_ARCHIVE_PAGE_MAX)
        return MYGAMES_STORE_INVALID;
    /* Unlike the normal Room Store, this API never makes a writer handle
     * look like a read-only source; no output publication on failure. */
    if (!store->read_only) return MYGAMES_STORE_READ_ONLY;
    if (!owner_valid(store)) return MYGAMES_STORE_QUARANTINED;
    orm_error_init(&error);
    rc = orm_transaction_begin(
        store->connection, ORM_ISOLATION_SERIALIZABLE, &tx, &error);
    if (rc != ORM_STATUS_OK) return error_from_orm(store, rc, &error);

    status = head_read(store, tx, &head, &exists);
    if (status != MYGAMES_STORE_OK) goto fail;
    if (!exists) { status = MYGAMES_STORE_UNINITIALIZED; goto fail; }
    status = verify_head(&head, NULL);
    if (status != MYGAMES_STORE_OK) goto fail;
    status = validate_journal(store, tx, head.seq);
    if (status != MYGAMES_STORE_OK) goto fail;
    if (after_sequence > head.seq) {
        status = MYGAMES_STORE_STALE_SEQUENCE;
        goto fail;
    }

    page.epoch = head.epoch;
    page.checkpoint_sequence = head.seq;
    /* This store still requires a complete 1..checkpoint history and does
     * not expose a configurable prune watermark. Never invent retention. */
    page.retained_from_sequence = 1u;
    page.resume_after_sequence = after_sequence;
    available = head.seq - after_sequence;
    limit = available < max_events ? available : max_events;
    for (i = 0u; i < limit; ++i) {
        orm_result_t *result = NULL;
        orm_value_t arg = orm_i64((int64_t)(after_sequence + i + 1u));
        orm_blob_t stored = {0};
        uint64_t row_count = 0u;
        int64_t stored_seq = 0;
        mygames_store_archive_item *item = &page.items[i];

        status = backend_query(store, tx,
            "select seq,payload from mg_outbox where seq=?1",
            &arg, 1u, &result);
        if (status != MYGAMES_STORE_OK) goto fail;
        orm_error_init(&error);
        rc = orm_result_row_count(result, &row_count, &error);
        if (rc == ORM_STATUS_OK && row_count == 1u)
            rc = orm_result_get_int64(result, 0u, 0u, &stored_seq, &error);
        if (rc == ORM_STATUS_OK && row_count == 1u)
            rc = orm_result_get_blob(result, 0u, 1u, &stored, &error);

        if (rc != ORM_STATUS_OK) status = error_from_orm(store, rc, &error);
        else if (row_count != 1u ||
                 stored_seq != (int64_t)(after_sequence + i + 1u) ||
                 !stored.data || stored.size == 0u ||
                 stored.size > MYGAMES_STORE_EVENT_MAX)
            status = MYGAMES_STORE_CORRUPT;
        else {
            item->sequence = (uint64_t)stored_seq;
            item->size = stored.size;
            memcpy(item->payload, stored.data, stored.size);
            status = MYGAMES_STORE_OK;
        }
        orm_result_destroy(result);
        if (status != MYGAMES_STORE_OK) goto fail;
        ++page.count;
        page.resume_after_sequence = item->sequence;
    }
    page.has_more = page.resume_after_sequence < head.seq;
    status = commit_transaction(store, tx);
    if (status == MYGAMES_STORE_OK) *out_page = page;
    return status;

fail:
    return rollback(store, tx, status);
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
