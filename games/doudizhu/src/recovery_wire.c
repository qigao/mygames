#include "mygames/recovery_wire.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct wire_buffer {
    char *data;
    size_t capacity;
    size_t used;
} wire_buffer;

static bool wire_append(wire_buffer *b, const char *format, ...) {
    va_list args;
    int n;
    if (!b || !format || !b->data || b->used >= b->capacity)
        return false;
    va_start(args, format);
    n = vsnprintf(b->data + b->used, b->capacity - b->used,
                  format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= b->capacity - b->used)
        return false;
    b->used += (size_t)n;
    return true;
}

static bool wire_bytes(wire_buffer *b, const void *data, size_t size) {
    if (!b || !b->data || !data || b->used > b->capacity ||
        size >= b->capacity - b->used)
        return false;
    memcpy(b->data + b->used, data, size);
    b->used += size;
    b->data[b->used] = '\0';
    return true;
}

static bool wire_decimal(const char *p, size_t size, uint64_t cap,
                         uint64_t *out) {
    uint64_t val = 0u;
    size_t i;
    if (!p || !out || size == 0u ||
        (size > 1u && p[0] == '0'))
        return false;
    for (i = 0u; i < size; ++i) {
        const unsigned char ch = (unsigned char)p[i];
        const uint64_t digit = (uint64_t)(ch - (unsigned char)'0');
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9' ||
            val > (cap - digit) / UINT64_C(10))
            return false;
        val = val * UINT64_C(10) + digit;
    }
    *out = val;
    return true;
}

mygames_recovery_wire_status mygames_recovery_parse_target(
    const char *target, uint64_t *out_after, uint32_t *out_limit) {
    static const char prefix[] = MYGAMES_RECOVERY_PATH "?";
    const char *p;
    bool seen_after = false, seen_limit = false;
    uint64_t after = 0u, limit = 0u;
    if (!target || !out_after || !out_limit ||
        strncmp(target, prefix, sizeof(prefix) - 1u) != 0)
        return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
    p = target + sizeof(prefix) - 1u;
    if (!*p) return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
    while (*p) {
        const char *pair = p;
        const char *end = strchr(pair, '&');
        const char *eq;
        size_t part_len, key_len;
        uint64_t value = 0u;
        if (!end) end = pair + strlen(pair);
        part_len = (size_t)(end - pair);
        eq = (const char *)memchr(pair, '=', part_len);
        if (!eq || eq == pair || eq + 1 == end)
            return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
        key_len = (size_t)(eq - pair);
        if (key_len == 5u && memcmp(pair, "after", 5u) == 0 &&
            !seen_after) {
            if (!wire_decimal(eq + 1u, (size_t)(end - eq - 1u),
                              (uint64_t)INT64_MAX, &value))
                return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
            seen_after = true;
            after = value;
        } else if (key_len == 5u && memcmp(pair, "limit", 5u) == 0 &&
                   !seen_limit) {
            if (!wire_decimal(eq + 1u, (size_t)(end - eq - 1u),
                              (uint64_t)MYGAMES_RECOVERY_MAX_PAGE, &value) ||
                value == 0u)
                return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
            seen_limit = true;
            limit = value;
        } else {
            return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
        }
        if (!*end) break;
        p = end + 1u;
        if (!*p) return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
    }
    if (!seen_after || !seen_limit)
        return MYGAMES_RECOVERY_WIRE_BAD_QUERY;
    *out_after = after;
    *out_limit = (uint32_t)limit;
    return MYGAMES_RECOVERY_WIRE_OK;
}

/* The ACL core admits only ASCII identifiers. Recheck the borrowed grant
 * before ever embedding its fields in JSON (no JSON string escaping). */
static bool wire_name(const char *name, size_t capacity) {
    size_t i;
    if (!name) return false;
    for (i = 0u; i < capacity; ++i) {
        const unsigned char ch = (unsigned char)name[i];
        if (ch == 0u) return i > 0u;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-' || ch == ':'))
            return false;
    }
    return false;
}

mygames_recovery_wire_status mygames_recovery_encode_page(
    const mygames_recovery_grant *grant,
    const mygames_store_archive_page *page,
    mygames_recovery_public_event_validate_fn validate_event,
    char *output, size_t output_capacity, size_t *out_bytes) {
    wire_buffer buffer;
    uint64_t next;
    size_t i;
    if (out_bytes) *out_bytes = 0u;
    if (!grant || !page || !validate_event || !output || !out_bytes ||
        output_capacity == 0u ||
        output_capacity > MYGAMES_RECOVERY_RESPONSE_MAX ||
        !grant->generation ||
        !wire_name(grant->source_namespace, sizeof(grant->source_namespace)) ||
        !wire_name(grant->consumer_id, sizeof(grant->consumer_id)) ||
        grant->page_limit == 0u ||
        grant->page_limit > MYGAMES_RECOVERY_MAX_PAGE ||
        grant->after_committed_seq > (uint64_t)INT64_MAX ||
        page->epoch == 0u || page->epoch > (uint64_t)INT64_MAX ||
        page->checkpoint_sequence > (uint64_t)INT64_MAX ||
        page->retained_from_sequence != 1u ||
        page->checkpoint_sequence < grant->after_committed_seq ||
        page->count > grant->page_limit ||
        page->count > MYGAMES_STORE_ARCHIVE_PAGE_MAX ||
        page->has_more != 0 && page->has_more != 1)
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    next = grant->after_committed_seq;
    /* Validate the WHOLE page before writing any response bytes. */
    for (i = 0u; i < page->count; ++i) {
        const mygames_store_archive_item *e = &page->items[i];
        if (next == UINT64_MAX || e->sequence != next + 1u ||
            e->sequence > page->checkpoint_sequence ||
            e->size == 0u || e->size > MYGAMES_STORE_EVENT_MAX ||
            !validate_event(e->payload, e->size, page->epoch, e->sequence))
            return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
        next = e->sequence;
    }
    if (page->resume_after_sequence != next ||
        (page->has_more != (next < page->checkpoint_sequence)))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    /* If there are committed rows past the requested cursor, there must be
     * a nonempty page; empty-but-has-more would cause a stuck consumer. */
    if (next < page->checkpoint_sequence && !page->count)
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;

    buffer = (wire_buffer){output, output_capacity, 0u};
    if (!wire_append(&buffer,
            "{\"v\":1,\"source_namespace\":\"%s\",\"consumer_id\":\"%s\","
            "\"owner_epoch\":%" PRIu64 ",\"checkpoint_seq\":%" PRIu64 ","
            "\"retained_from_seq\":1,\"events\":[",
            grant->source_namespace, grant->consumer_id,
            page->epoch, page->checkpoint_sequence))
        return MYGAMES_RECOVERY_WIRE_CAPACITY;
    for (i = 0u; i < page->count; ++i) {
        const mygames_store_archive_item *e = &page->items[i];
        if ((i != 0u && !wire_append(&buffer, ",")) ||
            !wire_append(&buffer, "{\"seq\":%" PRIu64 ",\"payload\":",
                         e->sequence) ||
            !wire_bytes(&buffer, e->payload, e->size) ||
            !wire_append(&buffer, "}"))
            return MYGAMES_RECOVERY_WIRE_CAPACITY;
    }
    if (!wire_append(&buffer, "],\"resume_after\":%" PRIu64
                     ",\"has_more\":%s}",
                     page->resume_after_sequence,
                     page->has_more ? "true" : "false"))
        return MYGAMES_RECOVERY_WIRE_CAPACITY;
    *out_bytes = buffer.used;
    return MYGAMES_RECOVERY_WIRE_OK;
}
