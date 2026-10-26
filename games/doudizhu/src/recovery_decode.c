#include "mygames/recovery_wire.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Deliberately deterministic and strict. No generic JSON parser can silently
 * admit escaped keys, duplicate fields, reordered authority or nested payload
 * objects. The server's v1 wire encoder emits exactly this representation. */
typedef struct recovery_input {
    const uint8_t *data;
    size_t size;
    size_t offset;
} recovery_input;

static bool read_literal(recovery_input *in, const char *literal) {
    const size_t n = strlen(literal);
    if (in->offset > in->size || n > in->size - in->offset ||
        memcmp(in->data + in->offset, literal, n) != 0)
        return false;
    in->offset += n;
    return true;
}
static bool read_decimal(recovery_input *in, uint64_t *out) {
    uint64_t value = 0u;
    size_t n = 0u;
    if (!in || !out || in->offset >= in->size) return false;
    if (in->data[in->offset] < '0' || in->data[in->offset] > '9')
        return false;
    if (in->data[in->offset] == '0') {
        ++in->offset;
        *out = 0u;
        return true;
    }
    while (in->offset < in->size) {
        uint64_t digit;
        const uint8_t ch = in->data[in->offset];
        if (ch < '0' || ch > '9') break;
        digit = (uint64_t)(ch - '0');
        if (value > ((uint64_t)INT64_MAX - digit) / UINT64_C(10))
            return false;
        value = value * UINT64_C(10) + digit;
        ++in->offset;
        ++n;
    }
    if (!n) return false;
    *out = value;
    return true;
}

/* Prevent a malformed or unterminated service policy identifier from being
 * compared with a short prefix in a forged response. This is admission
 * checking, not user-controlled JSON name parsing. */
static bool valid_identity(const char *name, size_t *out_length) {
    size_t n;
    if (!name || !out_length) return false;
    for (n = 0u; n < MYGAMES_RECOVERY_NAME_CAPACITY; ++n) {
        const uint8_t c = (uint8_t)name[n];
        if (c == 0u) {
            if (n == 0u) return false;
            *out_length = n;
            return true;
        }
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == ':'))
            return false;
    }
    return false;
}

static mygames_recovery_wire_status read_principal(
    recovery_input *in, const char *expected, size_t expected_size) {
    const uint8_t *start, *end;
    size_t found;
    if (!in || in->offset > in->size)
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    start = in->data + in->offset;
    end = (const uint8_t *)memchr(start, '"', in->size - in->offset);
    if (!end)
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    found = (size_t)(end - start);
    if (found != expected_size ||
        memcmp(start, expected, expected_size) != 0)
        return MYGAMES_RECOVERY_WIRE_WRONG_PRINCIPAL;
    in->offset += found + 1u;
    return MYGAMES_RECOVERY_WIRE_OK;
}

mygames_recovery_wire_status mygames_recovery_decode_page(
    const void *response_bytes, size_t response_size,
    const char *expected_source_namespace, const char *expected_consumer_id,
    uint64_t after_committed_seq,
    mygames_recovery_public_event_validate_fn validate_event,
    mygames_store_archive_page *out_page) {
    mygames_store_archive_page page = {0};
    recovery_input input;
    mygames_recovery_wire_status status;
    uint64_t version = 0u, retained = 0u, resume = 0u;
    uint64_t cursor = after_committed_seq;
    size_t ns_len = 0u, consumer_len = 0u;
    if (!response_bytes || response_size == 0u ||
        response_size > MYGAMES_RECOVERY_RESPONSE_MAX ||
        !out_page || !validate_event ||
        !valid_identity(expected_source_namespace, &ns_len) ||
        !valid_identity(expected_consumer_id, &consumer_len) ||
        after_committed_seq > (uint64_t)INT64_MAX)
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    input.data = (const uint8_t *)response_bytes;
    input.size = response_size;
    input.offset = 0u;

    if (!read_literal(&input, "{\"v\":") ||
        !read_decimal(&input, &version) || version != 1u ||
        !read_literal(&input, ",\"source_namespace\":\""))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    status = read_principal(&input, expected_source_namespace, ns_len);
    if (status != MYGAMES_RECOVERY_WIRE_OK) return status;
    if (!read_literal(&input, ",\"consumer_id\":\""))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    status = read_principal(&input, expected_consumer_id, consumer_len);
    if (status != MYGAMES_RECOVERY_WIRE_OK) return status;
    if (!read_literal(&input, ",\"owner_epoch\":") ||
        !read_decimal(&input, &page.epoch) || page.epoch == 0u ||
        !read_literal(&input, ",\"checkpoint_seq\":") ||
        !read_decimal(&input, &page.checkpoint_sequence) ||
        after_committed_seq > page.checkpoint_sequence ||
        !read_literal(&input, ",\"retained_from_seq\":") ||
        !read_decimal(&input, &retained) || retained != 1u ||
        !read_literal(&input, ",\"events\":["))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    page.retained_from_sequence = retained;

    if (!read_literal(&input, "]")) {
        for (;;) {
            mygames_store_archive_item *event;
            uint64_t sequence = 0u;
            const uint8_t *payload_start, *payload_end;
            size_t payload_length;
            if (page.count >= MYGAMES_STORE_ARCHIVE_PAGE_MAX ||
                !read_literal(&input, "{\"seq\":") ||
                !read_decimal(&input, &sequence) ||
                cursor == (uint64_t)INT64_MAX ||
                sequence != cursor + 1u ||
                sequence > page.checkpoint_sequence ||
                !read_literal(&input, ",\"payload\":"))
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
            if (input.offset >= input.size ||
                input.data[input.offset] != '{')
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
            payload_start = input.data + input.offset;
            payload_end = (const uint8_t *)memchr(
                payload_start, '}', input.size - input.offset);
            if (!payload_end)
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
            payload_length = (size_t)(payload_end - payload_start) + 1u;
            if (payload_length > MYGAMES_STORE_EVENT_MAX ||
                !validate_event(payload_start, payload_length,
                                page.epoch, sequence))
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
            event = &page.items[page.count];
            event->sequence = sequence;
            event->size = payload_length;
            memcpy(event->payload, payload_start, payload_length);
            ++page.count;
            cursor = sequence;
            input.offset += payload_length;
            if (!read_literal(&input, "}"))
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
            if (read_literal(&input, "]")) break;
            if (!read_literal(&input, ","))
                return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
        }
    }

    if (!read_literal(&input, ",\"resume_after\":") ||
        !read_decimal(&input, &resume) || resume != cursor ||
        !read_literal(&input, ",\"has_more\":"))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    if (read_literal(&input, "true"))
        page.has_more = 1;
    else if (read_literal(&input, "false"))
        page.has_more = 0;
    else return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;
    if (!read_literal(&input, "}") || input.offset != input.size ||
        page.has_more != (cursor < page.checkpoint_sequence) ||
        (page.has_more && page.count == 0u))
        return MYGAMES_RECOVERY_WIRE_INVALID_PAGE;

    page.resume_after_sequence = cursor;
    *out_page = page;
    return MYGAMES_RECOVERY_WIRE_OK;
}
