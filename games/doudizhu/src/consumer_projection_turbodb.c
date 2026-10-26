#include "mygames/consumer_projection.h"

#include <orm.h>
#include <orm_runtime.h>

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct mygames_consumer_projection {
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    bool quarantined;
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
    bool fail_after_receipt;
#endif
};

static mygames_consumer_status orm_failure(
    mygames_consumer_projection *s, orm_status_t status) {
    if (status == ORM_STATUS_COMMIT_UNKNOWN) {
        if (s) s->quarantined = true;
        return MYGAMES_CONSUMER_COMMIT_UNKNOWN;
    }
    if (status == ORM_STATUS_CLEANUP_FAILED) {
        if (s) s->quarantined = true;
        return MYGAMES_CONSUMER_QUARANTINED;
    }
    return MYGAMES_CONSUMER_BACKEND_ERROR;
}
static bool valid_name(const char *s) {
    size_t i;
    if (!s) return false;
    for (i = 0u; i <= MYGAMES_CONSUMER_IDENTIFIER_MAX; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0') return i > 0u;
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return false;
}
static bool valid_digest(
    const uint8_t hash[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES]) {
    size_t i;
    uint8_t acc = 0u;
    if (!hash) return false;
    for (i = 0u; i < MYGAMES_CONSUMER_EVENT_DIGEST_BYTES; ++i)
        acc |= hash[i];
    return acc != 0u;
}
static mygames_consumer_status query(
    mygames_consumer_projection *s, orm_transaction_t *tx, const char *sql,
    const orm_value_t *params, size_t count, orm_result_t **out) {
    orm_error_t error;
    orm_query_t *q = NULL;
    orm_status_t rc;
    size_t i;
    if (out) *out = NULL;
    orm_error_init(&error);
    rc = orm_raw(s->connection, orm_view(sql), &q, &error);
    if (rc == ORM_STATUS_OK) {
        for (i = 0u; i < count; ++i) {
            rc = orm_query_bind(q, params[i], &error);
            if (rc != ORM_STATUS_OK) break;
        }
    }
    if (rc == ORM_STATUS_OK) {
        if (tx) rc = orm_query_execute_in_transaction(q, tx, out, &error);
        else rc = orm_query_execute(q, out, &error);
    }
    if (q) orm_query_destroy(q);
    if (rc != ORM_STATUS_OK) {
        if (out && *out) {
            orm_result_destroy(*out);
            *out = NULL;
        }
        return orm_failure(s, rc);
    }
    return out && *out ? MYGAMES_CONSUMER_APPLIED
                       : MYGAMES_CONSUMER_BACKEND_ERROR;
}
static mygames_consumer_status exec_sql(
    mygames_consumer_projection *s, orm_transaction_t *tx, const char *sql,
    const orm_value_t *values, size_t count, uint64_t *changed) {
    orm_result_t *result = NULL;
    mygames_consumer_status st = query(s, tx, sql, values, count, &result);
    orm_error_t error;
    orm_status_t rc;
    if (st != MYGAMES_CONSUMER_APPLIED) return st;
    if (changed) {
        orm_error_init(&error);
        rc = orm_result_affected_rows(result, changed, &error);
        if (rc != ORM_STATUS_OK) st = orm_failure(s, rc);
    }
    orm_result_destroy(result);
    return st;
}
static mygames_consumer_status begin(
    mygames_consumer_projection *s, orm_transaction_t **out) {
    orm_error_t error;
    orm_status_t rc;
    *out = NULL;
    orm_error_init(&error);
    rc = orm_transaction_begin(s->connection, ORM_ISOLATION_SERIALIZABLE,
                               out, &error);
    return rc == ORM_STATUS_OK ? MYGAMES_CONSUMER_APPLIED : orm_failure(s, rc);
}
static mygames_consumer_status rollback(
    mygames_consumer_projection *s, orm_transaction_t *tx,
    mygames_consumer_status original) {
    orm_error_t error;
    orm_status_t rc;
    orm_error_init(&error);
    rc = orm_transaction_rollback(tx, &error);
    orm_transaction_destroy(tx);
    if (rc != ORM_STATUS_OK) {
        s->quarantined = true;
        return MYGAMES_CONSUMER_QUARANTINED;
    }
    return original;
}
static mygames_consumer_status commit(
    mygames_consumer_projection *s, orm_transaction_t *tx) {
    orm_error_t error;
    orm_status_t rc;
    orm_error_init(&error);
    rc = orm_transaction_commit(tx, &error);
    if (rc == ORM_STATUS_OK) {
        orm_transaction_destroy(tx);
        return MYGAMES_CONSUMER_APPLIED;
    }
    if (rc == ORM_STATUS_COMMIT_UNKNOWN) {
        (void)orm_failure(s, rc);
        orm_transaction_destroy(tx);
        return MYGAMES_CONSUMER_COMMIT_UNKNOWN;
    }
    return rollback(s, tx, orm_failure(s, rc));
}

static mygames_consumer_status read_cursor(
    mygames_consumer_projection *s, orm_transaction_t *tx,
    const char *ns, const char *consumer,
    uint64_t *last_seq, uint64_t *applied, bool *exists) {
    orm_value_t args[2] = {orm_text(ns), orm_text(consumer)};
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    uint64_t count = 0u;
    int64_t last = 0, effects = 0;
    mygames_consumer_status st = query(s, tx,
        "select last_seq, applied_count from mg_consumer_cursor "
        "where source_namespace=?1 and consumer_id=?2",
        args, 2u, &result);
    if (st != MYGAMES_CONSUMER_APPLIED) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &count, &error);
    if (rc == ORM_STATUS_OK && count == 1u) {
        rc = orm_result_get_int64(result, 0u, 0u, &last, &error);
        if (rc == ORM_STATUS_OK)
            rc = orm_result_get_int64(result, 0u, 1u, &effects, &error);
    }
    if (rc != ORM_STATUS_OK) st = orm_failure(s, rc);
    else if (count > 1u || last < 0 || effects < 0 ||
             last != effects)
        st = MYGAMES_CONSUMER_CORRUPT;
    else {
        *last_seq = (uint64_t)last;
        *applied = (uint64_t)effects;
        *exists = count == 1u;
    }
    orm_result_destroy(result);
    return st;
}

/* Do not treat a plausible cursor as truth after crash/corruption when
 * the receipt history no longer matches its committed projection count.
 * SQLite uniqueness guarantees no repeated event_seq per consumer stream;
 * min/max/count must cover the exact contiguous interval 1..last_seq. */
static mygames_consumer_status validate_receipt_history(
    mygames_consumer_projection *s, orm_transaction_t *tx) {
    static const char sql[] =
        "select "
        "(select count(*) from mg_consumer_cursor where "
        "last_seq<>applied_count) + "
        "(select count(*) from mg_consumer_cursor c where "
        "(select count(*) from mg_consumer_receipt r where "
        "r.source_namespace=c.source_namespace and "
        "r.consumer_id=c.consumer_id)<>c.last_seq or "
        "(c.last_seq>0 and ("
        "coalesce((select min(r.event_seq) from mg_consumer_receipt r where "
        "r.source_namespace=c.source_namespace and r.consumer_id=c.consumer_id),0)<>1 or "
        "coalesce((select max(r.event_seq) from mg_consumer_receipt r where "
        "r.source_namespace=c.source_namespace and r.consumer_id=c.consumer_id),0)<>c.last_seq))) + "
        "(select count(*) from mg_consumer_receipt r where "
        "not exists (select 1 from mg_consumer_cursor c where "
        "c.source_namespace=r.source_namespace and c.consumer_id=r.consumer_id))";
    orm_result_t *result = NULL;
    orm_error_t error;
    orm_status_t rc;
    uint64_t rows = 0u;
    int64_t inconsistent = 0;
    mygames_consumer_status st = query(s, tx, sql, NULL, 0u, &result);
    if (st != MYGAMES_CONSUMER_APPLIED) return st;
    orm_error_init(&error);
    rc = orm_result_row_count(result, &rows, &error);
    if (rc == ORM_STATUS_OK && rows == 1u)
        rc = orm_result_get_int64(result, 0u, 0u, &inconsistent, &error);
    if (rc != ORM_STATUS_OK) st = orm_failure(s, rc);
    else if (rows != 1u || inconsistent != 0)
        st = MYGAMES_CONSUMER_CORRUPT;
    orm_result_destroy(result);
    return st;
}

mygames_consumer_status mygames_consumer_projection_open(
    const char *sqlite_driver_absolute_path, const char *database_path,
    mygames_consumer_projection **out) {
    mygames_consumer_projection *s = NULL;
    orm_runtime_config_t runtime_cfg;
    orm_driver_load_config_t load = {0};
    orm_config_t cfg;
    orm_option_t opt;
    orm_error_t error;
    orm_status_t rc;
    orm_result_t *mode = NULL;
    orm_string_view_t text = {0};
    orm_transaction_t *tx = NULL;
    mygames_consumer_status st;
    static const char *const ddl[] = {
        "create table if not exists mg_consumer_cursor ("
        "source_namespace text not null, consumer_id text not null,"
        "last_seq integer not null check(last_seq>=0),"
        "applied_count integer not null check(applied_count>=0),"
        "primary key(source_namespace,consumer_id))",
        "create table if not exists mg_consumer_receipt ("
        "source_namespace text not null, consumer_id text not null,"
        "event_seq integer not null check(event_seq>0),"
        "sha256 blob not null check(length(sha256)=32),"
        "primary key(source_namespace,consumer_id,event_seq))"
    };
    size_t i;
    if (!out || !sqlite_driver_absolute_path ||
        !sqlite_driver_absolute_path[0] || !database_path ||
        !database_path[0] || strcmp(database_path, ":memory:") == 0)
        return MYGAMES_CONSUMER_INVALID;
    *out = NULL;
    s = (mygames_consumer_projection *)calloc(1u, sizeof(*s));
    if (!s) return MYGAMES_CONSUMER_BACKEND_ERROR;
    orm_runtime_config_init(&runtime_cfg);
    orm_error_init(&error);
    rc = orm_runtime_create(&runtime_cfg, &s->runtime, &error);
    if (rc != ORM_STATUS_OK) goto fail;
    load.struct_size = (uint32_t)sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(sqlite_driver_absolute_path);
    load.expected_driver_id = orm_view("sqlite");
    rc = orm_runtime_load_driver(s->runtime, &load, &error);
    if (rc != ORM_STATUS_OK) goto fail;
    orm_config(&cfg);
    opt.keyword = orm_view("filename");
    opt.value = orm_view(database_path);
    cfg.driver = orm_view("sqlite");
    cfg.options = &opt;
    cfg.option_count = 1u;
    rc = orm_runtime_connect(s->runtime, &cfg, &s->connection, &error);
    if (rc != ORM_STATUS_OK) goto fail;
    st = query(s, NULL, "pragma journal_mode=WAL", NULL, 0u, &mode);
    if (st != MYGAMES_CONSUMER_APPLIED) goto fail_store;
    rc = orm_result_get_text(mode, 0u, 0u, &text, &error);
    if (rc != ORM_STATUS_OK || text.len != 3u ||
        memcmp(text.data, "wal", 3u) != 0) {
        orm_result_destroy(mode);
        goto fail;
    }
    orm_result_destroy(mode);
    mode = NULL;
    st = exec_sql(s, NULL, "pragma synchronous=FULL", NULL, 0u, NULL);
    if (st != MYGAMES_CONSUMER_APPLIED) goto fail_store;
    st = begin(s, &tx);
    if (st != MYGAMES_CONSUMER_APPLIED) goto fail_store;
    for (i = 0u; i < sizeof(ddl) / sizeof(ddl[0]); ++i) {
        st = exec_sql(s, tx, ddl[i], NULL, 0u, NULL);
        if (st != MYGAMES_CONSUMER_APPLIED) {
            (void)rollback(s, tx, st);
            goto fail_store;
        }
    }
    st = validate_receipt_history(s, tx);
    if (st != MYGAMES_CONSUMER_APPLIED) {
        st = rollback(s, tx, st);
        goto fail_store;
    }
    st = commit(s, tx);
    if (st != MYGAMES_CONSUMER_APPLIED) goto fail_store;
    *out = s;
    return MYGAMES_CONSUMER_APPLIED;
fail_store:
    mygames_consumer_projection_close(s);
    return st;
fail:
    if (mode) orm_result_destroy(mode);
    mygames_consumer_projection_close(s);
    return MYGAMES_CONSUMER_BACKEND_ERROR;
}
void mygames_consumer_projection_close(mygames_consumer_projection *s) {
    orm_error_t error;
    if (!s) return;
    orm_error_init(&error);
    if (s->connection) {
        (void)orm_connection_close(s->connection, &error);
        orm_connection_release(s->connection);
    }
    if (s->runtime) {
        (void)orm_runtime_close(s->runtime, &error);
        orm_runtime_release(s->runtime);
    }
    memset(s, 0, sizeof(*s));
    free(s);
}
mygames_consumer_status mygames_consumer_projection_snapshot(
    mygames_consumer_projection *s,
    const char *ns, const char *consumer,
    uint64_t *last_seq, uint64_t *applied_count) {
    mygames_consumer_status st;
    uint64_t seq = 0u, effects = 0u;
    bool exists = false;
    if (!s || !valid_name(ns) || !valid_name(consumer) ||
        !last_seq || !applied_count) return MYGAMES_CONSUMER_INVALID;
    if (s->quarantined) return MYGAMES_CONSUMER_QUARANTINED;
    st = read_cursor(s, NULL, ns, consumer, &seq, &effects, &exists);
    if (st != MYGAMES_CONSUMER_APPLIED) return st;
    *last_seq = seq;
    *applied_count = effects;
    return MYGAMES_CONSUMER_APPLIED;
}

mygames_consumer_status mygames_consumer_projection_apply(
    mygames_consumer_projection *s,
    const char *ns, const char *consumer,
    uint64_t seq,
    const uint8_t digest[MYGAMES_CONSUMER_EVENT_DIGEST_BYTES],
    uint64_t *out_applied_count) {
    orm_transaction_t *tx = NULL;
    orm_value_t args[5];
    uint64_t last = 0u, count = 0u, changed = 0u;
    bool exists = false;
    mygames_consumer_status st;
    if (!s || !valid_name(ns) || !valid_name(consumer) ||
        !seq || seq > (uint64_t)INT64_MAX ||
        !valid_digest(digest) || !out_applied_count)
        return MYGAMES_CONSUMER_INVALID;
    if (s->quarantined) return MYGAMES_CONSUMER_QUARANTINED;
    st = begin(s, &tx);
    if (st != MYGAMES_CONSUMER_APPLIED) return st;
    args[0] = orm_text(ns);
    args[1] = orm_text(consumer);
    st = exec_sql(s, tx,
        "insert or ignore into mg_consumer_cursor"
        "(source_namespace,consumer_id,last_seq,applied_count) "
        "values(?1,?2,0,0)", args, 2u, NULL);
    if (st != MYGAMES_CONSUMER_APPLIED) return rollback(s, tx, st);
    st = read_cursor(s, tx, ns, consumer, &last, &count, &exists);
    if (st != MYGAMES_CONSUMER_APPLIED)
        return rollback(s, tx, st);
    if (!exists) return rollback(s, tx, MYGAMES_CONSUMER_CORRUPT);
    if (seq <= last) {
        orm_result_t *receipt = NULL;
        orm_error_t error;
        orm_blob_t stored = {0};
        orm_status_t rc;
        uint64_t rows = 0u;
        args[2] = orm_i64((int64_t)seq);
        st = query(s, tx,
            "select sha256 from mg_consumer_receipt where "
            "source_namespace=?1 and consumer_id=?2 and event_seq=?3",
            args, 3u, &receipt);
        if (st != MYGAMES_CONSUMER_APPLIED)
            return rollback(s, tx, st);
        orm_error_init(&error);
        rc = orm_result_row_count(receipt, &rows, &error);
        if (rc == ORM_STATUS_OK && rows == 1u)
            rc = orm_result_get_blob(receipt, 0u, 0u, &stored, &error);
        if (rc != ORM_STATUS_OK) st = orm_failure(s, rc);
        else if (rows != 1u || stored.size != MYGAMES_CONSUMER_EVENT_DIGEST_BYTES ||
                 !stored.data) st = MYGAMES_CONSUMER_CORRUPT;
        else st = memcmp(stored.data, digest, sizeof(uint8_t) *
                         MYGAMES_CONSUMER_EVENT_DIGEST_BYTES) == 0 ?
                  MYGAMES_CONSUMER_REPLAY : MYGAMES_CONSUMER_CONFLICT;
        orm_result_destroy(receipt);
        st = rollback(s, tx, st);
        if (st == MYGAMES_CONSUMER_REPLAY) *out_applied_count = count;
        return st;
    }
    if (last == (uint64_t)INT64_MAX || seq != last + 1u)
        return rollback(s, tx, MYGAMES_CONSUMER_SEQUENCE_GAP);
    args[2] = orm_i64((int64_t)seq);
    args[3] = orm_blob(digest, MYGAMES_CONSUMER_EVENT_DIGEST_BYTES);
    st = exec_sql(s, tx,
        "insert into mg_consumer_receipt "
        "(source_namespace,consumer_id,event_seq,sha256) values(?1,?2,?3,?4)",
        args, 4u, &changed);
    if (st != MYGAMES_CONSUMER_APPLIED) return rollback(s, tx, st);
    if (changed != 1u)
        return rollback(s, tx, MYGAMES_CONSUMER_CORRUPT);
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
    if (s->fail_after_receipt) {
        s->fail_after_receipt = false;
        return rollback(s, tx, MYGAMES_CONSUMER_BACKEND_ERROR);
    }
#endif
    args[2] = orm_i64((int64_t)seq);
    args[3] = orm_i64((int64_t)(count + 1u));
    args[4] = orm_i64((int64_t)last);
    st = exec_sql(s, tx,
        "update mg_consumer_cursor set last_seq=?3,applied_count=?4 "
        "where source_namespace=?1 and consumer_id=?2 "
        "and last_seq=?5",
        args, 5u, &changed);
    if (st != MYGAMES_CONSUMER_APPLIED) return rollback(s, tx, st);
    if (changed != 1u)
        return rollback(s, tx, MYGAMES_CONSUMER_CORRUPT);
    st = commit(s, tx);
    if (st == MYGAMES_CONSUMER_APPLIED)
        *out_applied_count = count + 1u;
    return st;
}
#ifdef MYGAMES_CONSUMER_FAULT_TESTING
void mygames_consumer_projection_fail_after_receipt_once(
    mygames_consumer_projection *s) {
    if (s) s->fail_after_receipt = true;
}
#endif
