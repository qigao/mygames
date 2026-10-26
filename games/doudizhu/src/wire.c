#include "mygames/wire.h"

#include <string.h>

typedef struct cursor {
    const unsigned char *data;
    size_t len;
    size_t offset;
} cursor;

static void skip_ws(cursor *p) {
    while (p->offset < p->len) {
        unsigned char ch = p->data[p->offset];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        ++p->offset;
    }
}
static bool consume(cursor *p, unsigned char want) {
    skip_ws(p);
    if (p->offset >= p->len || p->data[p->offset] != want) return false;
    ++p->offset;
    return true;
}
static bool safe_char(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == ':';
}
static bool read_string(cursor *p, char *buffer, size_t capacity) {
    size_t count = 0u;
    if (!consume(p, '"') || !capacity) return false;
    while (p->offset < p->len && p->data[p->offset] != '"') {
        unsigned char ch = p->data[p->offset++];
        if (!safe_char(ch) || count + 1u >= capacity) return false;
        buffer[count++] = (char)ch;
    }
    if (count == 0u || p->offset >= p->len) return false;
    ++p->offset;
    buffer[count] = '\0';
    return true;
}
static bool read_version(cursor *p, uint32_t *out) {
    skip_ws(p);
    if (p->offset == p->len || p->data[p->offset] != '1') return false;
    ++p->offset;
    if (p->offset < p->len) {
        const unsigned char next = p->data[p->offset];
        if (next >= '0' && next <= '9') return false;
        if (next == '.') return false;
    }
    *out = 1u;
    return true;
}
static bool read_boolean(cursor *p, bool *out) {
    static const char true_text[] = "true";
    static const char false_text[] = "false";
    skip_ws(p);
    if (p->len - p->offset >= sizeof(true_text) - 1u &&
        memcmp(p->data + p->offset, true_text, sizeof(true_text) - 1u) == 0) {
        p->offset += sizeof(true_text) - 1u;
        *out = true;
        return true;
    }
    if (p->len - p->offset >= sizeof(false_text) - 1u &&
        memcmp(p->data + p->offset, false_text, sizeof(false_text) - 1u) == 0) {
        p->offset += sizeof(false_text) - 1u;
        *out = false;
        return true;
    }
    return false;
}
static mygames_wire_status finish(const char *type, unsigned int fields,
                                   const mygames_wire_command *cmd) {
    enum {V = 1u, T = 2u, ID = 4u, ROOM = 8u, READY = 16u, TOKEN = 32u};
    if ((fields & (V | T)) != (V | T)) return MYGAMES_WIRE_INVALID;
    if (strcmp(type, "client.ping") == 0) {
        return fields == (V | T) ? MYGAMES_WIRE_OK : MYGAMES_WIRE_INVALID;
    }
    if (strcmp(type, "session.resume") == 0) {
        size_t i;
        if (fields != (V | T | TOKEN) ||
            strlen(cmd->resume_token) != MYGAMES_WIRE_RESUME_TICKET_CAPACITY - 1u)
            return MYGAMES_WIRE_INVALID;
        for (i = 0u; i < MYGAMES_WIRE_RESUME_TICKET_CAPACITY - 1u; ++i) {
            char ch = cmd->resume_token[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                return MYGAMES_WIRE_INVALID;
        }
        return MYGAMES_WIRE_OK;
    }
    if (strcmp(type, "room.create") == 0 ||
        strcmp(type, "room.leave") == 0 ||
        strcmp(type, "room.snapshot.get") == 0) {
        return fields == (V | T | ID) && cmd->request_id[0]
                   ? MYGAMES_WIRE_OK : MYGAMES_WIRE_INVALID;
    }
    if (strcmp(type, "room.join") == 0) {
        return fields == (V | T | ID | ROOM) && cmd->room_id[0]
                   ? MYGAMES_WIRE_OK : MYGAMES_WIRE_INVALID;
    }
    if (strcmp(type, "room.ready") == 0) {
        return fields == (V | T | ID | READY)
                   ? MYGAMES_WIRE_OK : MYGAMES_WIRE_INVALID;
    }
    return MYGAMES_WIRE_UNSUPPORTED_TYPE;
}
mygames_wire_status mygames_wire_decode_command(const void *bytes, size_t size,
                                                 mygames_wire_command *out) {
    enum {V = 1u, T = 2u, ID = 4u, ROOM = 8u, READY = 16u, TOKEN = 32u};
    cursor p;
    char type[32] = {0};
    unsigned int fields = 0u;
    mygames_wire_status status;
    uint32_t version = 0u;
    if (!out || (!bytes && size != 0u)) return MYGAMES_WIRE_INVALID;
    memset(out, 0, sizeof(*out));
    if (size > MYGAMES_WIRE_MAX_BYTES) return MYGAMES_WIRE_TOO_LARGE;
    if (!bytes) return MYGAMES_WIRE_INVALID;
    p = (cursor){(const unsigned char *)bytes, size, 0u};
    if (!consume(&p, '{')) return MYGAMES_WIRE_INVALID;
    for (;;) {
        char key[24] = {0};
        unsigned int bit = 0u;
        skip_ws(&p);
        if (p.offset < p.len && p.data[p.offset] == '}') break;
        if (!read_string(&p, key, sizeof(key)) || !consume(&p, ':'))
            return MYGAMES_WIRE_INVALID;
        if (strcmp(key, "v") == 0) bit = V;
        else if (strcmp(key, "type") == 0) bit = T;
        else if (strcmp(key, "request_id") == 0) bit = ID;
        else if (strcmp(key, "room_id") == 0) bit = ROOM;
        else if (strcmp(key, "ready") == 0) bit = READY;
        else if (strcmp(key, "resume_token") == 0) bit = TOKEN;
        else return MYGAMES_WIRE_INVALID;
        if (fields & bit) return MYGAMES_WIRE_INVALID;
        fields |= bit;
        if (bit == V) {
            if (!read_version(&p, &version)) return MYGAMES_WIRE_INVALID;
        } else if (bit == T) {
            if (!read_string(&p, type, sizeof(type))) return MYGAMES_WIRE_INVALID;
        } else if (bit == ID) {
            if (!read_string(&p, out->request_id, sizeof(out->request_id)))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == ROOM) {
            if (!read_string(&p, out->room_id, sizeof(out->room_id)))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == TOKEN) {
            if (!read_string(&p, out->resume_token, sizeof(out->resume_token)))
                return MYGAMES_WIRE_INVALID;
        } else {
            if (!read_boolean(&p, &out->ready)) return MYGAMES_WIRE_INVALID;
        }
        skip_ws(&p);
        if (p.offset < p.len && p.data[p.offset] == ',') {
            ++p.offset;
            skip_ws(&p);
            if (p.offset == p.len || p.data[p.offset] == '}')
                return MYGAMES_WIRE_INVALID; /* trailing comma */
        } else break;
    }
    if (!consume(&p, '}')) return MYGAMES_WIRE_INVALID;
    skip_ws(&p);
    if (p.offset != p.len) return MYGAMES_WIRE_INVALID;
    if (version != 1u) return MYGAMES_WIRE_UNSUPPORTED_VERSION;
    status = finish(type, fields, out);
    if (status != MYGAMES_WIRE_OK) return status;
    if (!strcmp(type, "client.ping")) out->kind = MYGAMES_COMMAND_PING;
    else if (!strcmp(type, "room.create")) out->kind = MYGAMES_COMMAND_CREATE_ROOM;
    else if (!strcmp(type, "room.join")) out->kind = MYGAMES_COMMAND_JOIN_ROOM;
    else if (!strcmp(type, "room.ready")) out->kind = MYGAMES_COMMAND_READY;
    else if (!strcmp(type, "room.leave")) out->kind = MYGAMES_COMMAND_LEAVE_ROOM;
    else if (!strcmp(type, "session.resume")) out->kind = MYGAMES_COMMAND_RESUME_SESSION;
    else out->kind = MYGAMES_COMMAND_GET_SNAPSHOT;
    return MYGAMES_WIRE_OK;
}
