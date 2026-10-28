#include "mygames/ddz_game_wire.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct game_json {
    const unsigned char *data;
    size_t size, index;
} game_json;

enum {
    GAME_V = 1u, GAME_TYPE = 2u, GAME_REQ = 4u,
    GAME_ID = 8u, GAME_TURN = 16u, GAME_SCORE = 32u, GAME_CARDS = 64u
};

static void game_skip_space(game_json *p) {
    while (p->index < p->size &&
           (p->data[p->index] == ' ' || p->data[p->index] == '\n' ||
            p->data[p->index] == '\t' || p->data[p->index] == '\r'))
        ++p->index;
}

static bool game_take(game_json *p, unsigned char expected) {
    game_skip_space(p);
    if (p->index == p->size || p->data[p->index] != expected) return false;
    ++p->index;
    return true;
}

static bool game_ascii(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '.' || c == ':' ||
           c == '-' || c == '_';
}

static bool game_read_string(game_json *p, char *out, size_t cap) {
    size_t length = 0u;
    if (!cap || !game_take(p, '"')) return false;
    while (p->index < p->size && p->data[p->index] != '"') {
        unsigned char ch = p->data[p->index++];
        if (!game_ascii(ch) || length + 1u >= cap) return false;
        out[length++] = (char)ch;
    }
    if (!length || p->index >= p->size) return false;
    out[length] = '\0';
    ++p->index;
    return true;
}

/* Exact quoted base-10 uint64; disallow zero and leading zeroes. */
static bool game_read_u64_string(game_json *p, uint64_t *result) {
    uint64_t n = UINT64_C(0);
    size_t digits = 0u;
    if (!game_take(p, '"')) return false;
    while (p->index < p->size && p->data[p->index] != '"') {
        unsigned char c = p->data[p->index++];
        unsigned int digit;
        if (c < '0' || c > '9' || digits >= 20u) return false;
        if (digits == 0u && c == '0') return false;
        digit = (unsigned int)(c - '0');
        if (n > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10))
            return false;
        n = n * UINT64_C(10) + (uint64_t)digit;
        ++digits;
    }
    if (!digits || !n || p->index >= p->size) return false;
    ++p->index;
    *result = n;
    return true;
}

static bool game_read_score(game_json *p, uint8_t *result) {
    unsigned char c;
    game_skip_space(p);
    if (p->index == p->size) return false;
    c = p->data[p->index++];
    if (c < '0' || c > '3') return false;
    /* A trailing integer digit, '.', exponent or sign will fail later at
     * the comma/closing brace as an unknown extra token. */
    *result = (uint8_t)(c - '0');
    return true;
}

static bool game_read_card_mask(game_json *p, uint64_t *result) {
    uint64_t cards = UINT64_C(0);
    size_t i;
    if (!game_take(p, '"')) return false;
    for (i = 0u; i < 14u; ++i) {
        unsigned char c;
        uint64_t digit;
        if (p->index >= p->size) return false;
        c = p->data[p->index++];
        if (c >= '0' && c <= '9') digit = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint64_t)(10u + c - 'a');
        else return false; /* no upper-case aliases */
        cards = (cards << 4u) | digit;
    }
    if (p->index == p->size || p->data[p->index++] != '"' ||
        !cards || (cards & ~((UINT64_C(1) << 54u) - UINT64_C(1))) != 0u)
        return false;
    *result = cards;
    return true;
}

static mygames_wire_status game_classify(const char *type, unsigned fields,
                                        mygames_ddz_game_wire_command *cmd) {
    if (!strcmp(type, "game.start")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ))
            return MYGAMES_WIRE_INVALID;
        cmd->kind = MYGAMES_DDZ_GAME_START;
    } else if (!strcmp(type, "game.bid")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ | GAME_ID |
                       GAME_TURN | GAME_SCORE)) return MYGAMES_WIRE_INVALID;
        cmd->kind = MYGAMES_DDZ_GAME_BID;
    } else if (!strcmp(type, "game.play")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ | GAME_ID |
                       GAME_TURN | GAME_CARDS)) return MYGAMES_WIRE_INVALID;
        cmd->kind = MYGAMES_DDZ_GAME_PLAY;
    } else if (!strcmp(type, "game.pass")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ | GAME_ID | GAME_TURN))
            return MYGAMES_WIRE_INVALID;
        cmd->kind = MYGAMES_DDZ_GAME_PASS;
    } else if (!strcmp(type, "game.snapshot.get") ||
               !strcmp(type, "game.hand.get") ||
               !strcmp(type, "game.result.get")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ | GAME_ID))
            return MYGAMES_WIRE_INVALID;
        cmd->kind = !strcmp(type, "game.snapshot.get") ?
            MYGAMES_DDZ_GAME_SNAPSHOT :
            !strcmp(type, "game.hand.get") ?
            MYGAMES_DDZ_GAME_HAND : MYGAMES_DDZ_GAME_RESULT;
    } else if (!strcmp(type, "game.standings.get")) {
        if (fields != (GAME_V | GAME_TYPE | GAME_REQ))
            return MYGAMES_WIRE_INVALID;
        cmd->kind = MYGAMES_DDZ_GAME_STANDINGS;
    } else return MYGAMES_WIRE_UNSUPPORTED_TYPE;
    return MYGAMES_WIRE_OK;
}

mygames_wire_status mygames_ddz_game_wire_decode(
    const void *bytes, size_t size, mygames_ddz_game_wire_command *out) {
    game_json p;
    mygames_ddz_game_wire_command value = {0};
    char type[40] = {0};
    unsigned fields = 0u;
    uint8_t version = 0u;
    if (!out || !bytes || size == 0u) return MYGAMES_WIRE_INVALID;
    if (size > MYGAMES_WIRE_MAX_BYTES) return MYGAMES_WIRE_TOO_LARGE;
    p = (game_json){(const unsigned char *)bytes, size, 0u};
    if (!game_take(&p, '{')) return MYGAMES_WIRE_INVALID;
    for (;;) {
        char key[24] = {0};
        unsigned bit;
        game_skip_space(&p);
        if (p.index == p.size) return MYGAMES_WIRE_INVALID;
        if (p.data[p.index] == '}') break;
        if (!game_read_string(&p, key, sizeof(key)) ||
            !game_take(&p, ':')) return MYGAMES_WIRE_INVALID;
        if (!strcmp(key, "v")) bit = GAME_V;
        else if (!strcmp(key, "type")) bit = GAME_TYPE;
        else if (!strcmp(key, "request_id")) bit = GAME_REQ;
        else if (!strcmp(key, "game_id")) bit = GAME_ID;
        else if (!strcmp(key, "turn_id")) bit = GAME_TURN;
        else if (!strcmp(key, "bid")) bit = GAME_SCORE;
        else if (!strcmp(key, "cards_hex")) bit = GAME_CARDS;
        else return MYGAMES_WIRE_INVALID;
        if ((fields & bit) != 0u) return MYGAMES_WIRE_INVALID;
        fields |= bit;
        if (bit == GAME_V) {
            uint8_t ver;
            game_skip_space(&p);
            if (p.index >= p.size || p.data[p.index++] != '1')
                return MYGAMES_WIRE_INVALID;
            ver = 1u; version = ver;
        } else if (bit == GAME_TYPE) {
            if (!game_read_string(&p, type, sizeof(type)))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == GAME_REQ) {
            if (!game_read_string(&p, value.request_id,
                                  sizeof(value.request_id)))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == GAME_ID) {
            if (!game_read_u64_string(&p, &value.game_id))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == GAME_TURN) {
            if (!game_read_u64_string(&p, &value.turn_id))
                return MYGAMES_WIRE_INVALID;
        } else if (bit == GAME_SCORE) {
            if (!game_read_score(&p, &value.bid_score))
                return MYGAMES_WIRE_INVALID;
        } else if (!game_read_card_mask(&p, &value.cards_mask)) {
            return MYGAMES_WIRE_INVALID;
        }
        game_skip_space(&p);
        if (p.index < p.size && p.data[p.index] == ',') {
            ++p.index;
            game_skip_space(&p);
            if (p.index == p.size || p.data[p.index] == '}')
                return MYGAMES_WIRE_INVALID;
        } else break;
    }
    if (!game_take(&p, '}')) return MYGAMES_WIRE_INVALID;
    game_skip_space(&p);
    if (p.index != p.size || version != 1u)
        return MYGAMES_WIRE_INVALID;
    {
        const mygames_wire_status st = game_classify(type, fields, &value);
        if (st != MYGAMES_WIRE_OK) return st;
    }
    *out = value;
    return MYGAMES_WIRE_OK;
}
