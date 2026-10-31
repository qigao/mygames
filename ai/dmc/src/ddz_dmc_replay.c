#include "mygames/ddz_dmc_replay.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define DMC_HEADER MYGAMES_DDZ_DMC_REPLAY_HEADER_BYTES
#define DMC_RECORD MYGAMES_DDZ_DMC_REPLAY_RECORD_BYTES
#define DMC_TRAILER MYGAMES_DDZ_DMC_REPLAY_TRAILER_BYTES

static const uint8_t dmc_magic[8] = {
    'M', 'G', 'D', 'M', 'C', '0', '0', '1'
};

static void put16(uint8_t *out, uint16_t n) {
    out[0] = (uint8_t)n;
    out[1] = (uint8_t)(n >> 8u);
}
static void put32(uint8_t *out, uint32_t n) {
    unsigned i;
    for (i = 0u; i < 4u; ++i) out[i] = (uint8_t)(n >> (i * 8u));
}
static void put64(uint8_t *out, uint64_t n) {
    unsigned i;
    for (i = 0u; i < 8u; ++i) out[i] = (uint8_t)(n >> (i * 8u));
}
static uint16_t get16(const uint8_t *src) {
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8u));
}
static uint32_t get32(const uint8_t *src) {
    uint32_t n = 0u;
    unsigned i;
    for (i = 0u; i < 4u; ++i) n |= (uint32_t)src[i] << (8u * i);
    return n;
}
static uint64_t get64(const uint8_t *src) {
    uint64_t n = 0u;
    unsigned i;
    for (i = 0u; i < 8u; ++i) n |= (uint64_t)src[i] << (8u * i);
    return n;
}
static uint32_t crc32_ieee(const uint8_t *src, size_t bytes) {
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    for (i = 0u; i < bytes; ++i) {
        unsigned bit;
        crc ^= src[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) ?
                      UINT32_C(0xedb88320) : UINT32_C(0));
    }
    return ~crc;
}
static bool canonical_binary(const int8_t *data, size_t bytes) {
    size_t i;
    for (i = 0u; i < bytes; ++i)
        if (data[i] != 0 && data[i] != 1) return false;
    return true;
}
static bool canonical_action(uint64_t cards, const int8_t *encoded) {
    int8_t expected[MYGAMES_DDZ_AI_CARD_FEATURES] = {0};
    unsigned counts[15] = {0};
    unsigned id, rank, k;
    mygames_ddz_move move = {0};
    if ((cards & ~MYGAMES_DDZ_CARD_MASK) != 0u ||
        (cards != 0u &&
         mygames_ddz_classify(cards, &move) != MYGAMES_DDZ_OK))
        return false;
    for (id = 0u; id < MYGAMES_DDZ_CARDS; ++id) {
        if ((cards & (UINT64_C(1) << id)) == 0u) continue;
        rank = id < 52u ? id / 4u : id - 39u;
        ++counts[rank];
    }
    for (rank = 0u; rank < 13u; ++rank) {
        if (counts[rank] > 4u) return false;
        for (k = 0u; k < counts[rank]; ++k)
            expected[4u * rank + k] = 1;
    }
    expected[52] = (int8_t)counts[13];
    expected[53] = (int8_t)counts[14];
    return memcmp(expected, encoded, sizeof(expected)) == 0;
}
static bool allowed_unit(double value) {
    unsigned base, exponent;
    if (!isfinite(value) || value <= 0.0 || value > 98304.0)
        return false;
    for (base = 1u; base <= 3u; ++base)
        for (exponent = 0u; exponent <= 15u; ++exponent)
            if (value == (double)((uint32_t)base << exponent))
                return true;
    return false;
}
static bool sample_valid(const mygames_ddz_dmc_sample *s,
                         uint64_t episode_id, uint64_t model_version,
                         uint32_t rules, uint8_t landlord, uint8_t winner,
                         mygames_ddz_dmc_objective objective, size_t step,
                         double unit) {
    unsigned actor;
    mygames_ddz_ai_role expected_role;
    double expected_target;
    size_t i, x_count;
    if (!s || s->episode_id != episode_id || !model_version ||
        s->model_version != model_version ||
        s->rules_version != rules || rules == 0u ||
        s->source_turn_id != (uint64_t)step + UINT64_C(1) ||
        s->actor_seat >= MYGAMES_DDZ_SEATS ||
        s->features.role != s->role || !isfinite(s->target))
        return false;
    actor = s->actor_seat;
    expected_role = actor == landlord ? MYGAMES_DDZ_AI_LANDLORD :
        actor == (landlord + 1u) % MYGAMES_DDZ_SEATS ?
          MYGAMES_DDZ_AI_LANDLORD_DOWN : MYGAMES_DDZ_AI_LANDLORD_UP;
    if (s->role != expected_role) return false;
    x_count = expected_role == MYGAMES_DDZ_AI_LANDLORD ?
        MYGAMES_DDZ_AI_LANDLORD_FEATURES : MYGAMES_DDZ_AI_FARMER_FEATURES;
    if (s->features.x_count != x_count ||
        !canonical_binary(s->features.x_no_action, x_count) ||
        !canonical_binary(s->features.z, MYGAMES_DDZ_AI_HISTORY_FEATURES) ||
        !canonical_action(s->chosen_cards, s->action_features))
        return false;
    for (i = x_count; i < MYGAMES_DDZ_AI_MAX_FEATURES; ++i)
        if (s->features.x_no_action[i] != 0) return false;
    if (objective == MYGAMES_DDZ_DMC_WIN_SIGN) {
        const bool winner_team = (actor == landlord) == (winner == landlord);
        expected_target = winner_team ? 1.0 : -1.0;
    } else {
        const double sign = (winner == landlord) ? 1.0 : -1.0;
        expected_target = actor == landlord ? sign * 2.0 * unit : -sign * unit;
    }
    return s->target == expected_target;
}
static bool replay_metadata_valid(
    uint64_t episode_id, uint64_t model_version, uint32_t rules_version,
    uint32_t objective, size_t count, uint8_t landlord, uint8_t winner,
    double first_target, double *out_unit) {
    double unit;
    if (!episode_id || !model_version || !rules_version ||
        landlord >= MYGAMES_DDZ_SEATS || winner >= MYGAMES_DDZ_SEATS ||
        count == 0u || count > MYGAMES_DDZ_DMC_MAX_STEPS ||
        (objective != MYGAMES_DDZ_DMC_WIN_SIGN &&
         objective != MYGAMES_DDZ_DMC_GAME_POINTS))
        return false;
    if (objective == MYGAMES_DDZ_DMC_WIN_SIGN) {
        unit = 1.0;
    } else {
        /* First card-play turn must be landlord's. Landlord's terminal
         * signed value is exactly twice a farmer's per-seat unit. */
        unit = fabs(first_target) / 2.0;
        if (!allowed_unit(unit)) return false;
    }
    *out_unit = unit;
    return true;
}
static size_t replay_size(size_t count) {
    if (count == 0u || count > MYGAMES_DDZ_DMC_MAX_STEPS) return 0u;
    return DMC_HEADER + count * DMC_RECORD + DMC_TRAILER;
}
static void write_record(uint8_t *dst, const mygames_ddz_dmc_sample *s) {
    uint64_t bits;
    put64(dst, s->source_turn_id);
    put64(dst + 8u, s->chosen_cards);
    dst[16] = s->actor_seat;
    dst[17] = (uint8_t)s->role;
    put16(dst + 18u, (uint16_t)s->features.x_count);
    put32(dst + 20u, 0u);
    memcpy(&bits, &s->target, sizeof(bits));
    put64(dst + 24u, bits);
    memcpy(dst + 32u, s->features.x_no_action, MYGAMES_DDZ_AI_MAX_FEATURES);
    memcpy(dst + 32u + MYGAMES_DDZ_AI_MAX_FEATURES,
           s->features.z, MYGAMES_DDZ_AI_HISTORY_FEATURES);
    memcpy(dst + 32u + MYGAMES_DDZ_AI_MAX_FEATURES +
           MYGAMES_DDZ_AI_HISTORY_FEATURES,
           s->action_features, MYGAMES_DDZ_AI_CARD_FEATURES);
}
static bool read_record(const uint8_t *src,
                        mygames_ddz_dmc_sample *s,
                        uint64_t episode_id, uint64_t model_version,
                        uint32_t rules) {
    uint64_t bits;
    if (get32(src + 20u) != 0u) return false;
    memset(s, 0, sizeof(*s));
    s->source_turn_id = get64(src);
    s->chosen_cards = get64(src + 8u);
    s->actor_seat = src[16];
    s->role = (mygames_ddz_ai_role)src[17];
    s->features.role = s->role;
    s->features.x_count = get16(src + 18u);
    bits = get64(src + 24u);
    memcpy(&s->target, &bits, sizeof(bits));
    s->episode_id = episode_id;
    s->model_version = model_version;
    s->rules_version = rules;
    memcpy(s->features.x_no_action, src + 32u,
           MYGAMES_DDZ_AI_MAX_FEATURES);
    memcpy(s->features.z, src + 32u + MYGAMES_DDZ_AI_MAX_FEATURES,
           MYGAMES_DDZ_AI_HISTORY_FEATURES);
    memcpy(s->action_features, src + 32u + MYGAMES_DDZ_AI_MAX_FEATURES +
           MYGAMES_DDZ_AI_HISTORY_FEATURES, MYGAMES_DDZ_AI_CARD_FEATURES);
    return true;
}

mygames_ddz_dmc_replay_status mygames_ddz_dmc_replay_encode(
    const mygames_ddz_dmc_episode *e, void *out_bytes,
    size_t capacity, size_t *out_size) {
    const size_t count = e ? e->count : 0u;
    const size_t bytes = replay_size(count);
    double unit = 0.0;
    size_t i;
    uint8_t *dst = (uint8_t *)out_bytes;
    if (!e || !e->samples || !out_size || !bytes ||
        (!out_bytes && capacity != 0u) ||
        !e->terminated || !e->sealed || e->capacity < count ||
        e->next_turn_id != (uint64_t)count + UINT64_C(1) ||
        e->terminal_winner != e->samples[count - 1u].actor_seat ||
        sizeof(double) != 8u || FLT_RADIX != 2 || DBL_MANT_DIG != 53 ||
        !replay_metadata_valid(e->episode_id, e->model_version,
            e->rules_version, (uint32_t)e->objective, count,
            e->landlord_seat, e->terminal_winner,
            e->samples[0].target, &unit) ||
        e->samples[0].actor_seat != e->landlord_seat)
        return MYGAMES_DDZ_REPLAY_INVALID;
    for (i = 0u; i < count; ++i)
        if (!sample_valid(&e->samples[i], e->episode_id, e->model_version,
              e->rules_version, e->landlord_seat, e->terminal_winner,
              e->objective, i, unit))
            return MYGAMES_DDZ_REPLAY_INVALID;
    if (!dst) {
        *out_size = bytes;
        return MYGAMES_DDZ_REPLAY_OK;
    }
    if (capacity < bytes) {
        *out_size = bytes;
        return MYGAMES_DDZ_REPLAY_CAPACITY;
    }
    memset(dst, 0, DMC_HEADER);
    memcpy(dst, dmc_magic, sizeof(dmc_magic));
    put32(dst + 8u, MYGAMES_DDZ_DMC_REPLAY_VERSION);
    put32(dst + 12u, 1u); /* sealed/terminal */
    put64(dst + 16u, e->episode_id);
    put64(dst + 24u, e->model_version);
    put32(dst + 32u, e->rules_version);
    put32(dst + 36u, (uint32_t)e->objective);
    put32(dst + 40u, (uint32_t)count);
    dst[44] = e->landlord_seat;
    dst[45] = e->terminal_winner;
    put32(dst + 48u, DMC_RECORD);
    put32(dst + 52u, (uint32_t)(count * DMC_RECORD));
    for (i = 0u; i < count; ++i)
        write_record(dst + DMC_HEADER + i * DMC_RECORD, &e->samples[i]);
    put32(dst + bytes - DMC_TRAILER,
          crc32_ieee(dst, bytes - DMC_TRAILER));
    *out_size = bytes;
    return MYGAMES_DDZ_REPLAY_OK;
}

mygames_ddz_dmc_replay_status mygames_ddz_dmc_replay_decode(
    const void *bytes, size_t byte_count,
    mygames_ddz_dmc_episode *out_episode,
    mygames_ddz_dmc_sample *out_samples, size_t sample_capacity,
    size_t *out_required_samples) {
    const uint8_t *src = (const uint8_t *)bytes;
    uint64_t episode_id, model_version;
    uint32_t rules, objective, count;
    uint8_t landlord, winner, last_actor = MYGAMES_DDZ_NO_SEAT;
    double first_target = 0.0, unit = 0.0;
    size_t i;
    const bool query = !out_episode && !out_samples && sample_capacity == 0u;
    if (!src || !out_required_samples ||
        (!query && (!out_episode || !out_samples)) ||
        byte_count < DMC_HEADER + DMC_TRAILER ||
        byte_count > MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES ||
        sizeof(double) != 8u || FLT_RADIX != 2 || DBL_MANT_DIG != 53)
        return MYGAMES_DDZ_REPLAY_INVALID;
    if (memcmp(src, dmc_magic, sizeof(dmc_magic)) != 0 ||
        get32(src + 8u) != MYGAMES_DDZ_DMC_REPLAY_VERSION ||
        get32(src + 12u) != 1u ||
        get16(src + 46u) != 0u || get64(src + 56u) != 0u ||
        get32(src + 48u) != DMC_RECORD)
        return MYGAMES_DDZ_REPLAY_CORRUPT;
    episode_id = get64(src + 16u);
    model_version = get64(src + 24u);
    rules = get32(src + 32u);
    objective = get32(src + 36u);
    count = get32(src + 40u);
    landlord = src[44];
    winner = src[45];
    if (replay_size(count) != byte_count ||
        get32(src + 52u) != (uint32_t)(count * DMC_RECORD) ||
        get32(src + byte_count - DMC_TRAILER) !=
           crc32_ieee(src, byte_count - DMC_TRAILER))
        return MYGAMES_DDZ_REPLAY_CORRUPT;
    if (count) {
        uint64_t bits = get64(src + DMC_HEADER + 24u);
        memcpy(&first_target, &bits, sizeof(first_target));
    }
    if (!replay_metadata_valid(episode_id, model_version, rules,
        objective, count, landlord, winner, first_target, &unit))
        return MYGAMES_DDZ_REPLAY_CORRUPT;
    for (i = 0u; i < count; ++i) {
        mygames_ddz_dmc_sample sample;
        const uint8_t *record = src + DMC_HEADER + i * DMC_RECORD;
        if (!read_record(record, &sample, episode_id, model_version, rules) ||
            !sample_valid(&sample, episode_id, model_version,
                rules, landlord, winner,
                (mygames_ddz_dmc_objective)objective, i, unit))
            return MYGAMES_DDZ_REPLAY_CORRUPT;
        last_actor = sample.actor_seat;
    }
    if (last_actor != winner ||
        src[DMC_HEADER + 16u] != landlord)
        return MYGAMES_DDZ_REPLAY_CORRUPT;
    if (!query && sample_capacity < count) {
        *out_required_samples = count;
        return MYGAMES_DDZ_REPLAY_CAPACITY;
    }
    if (!query) {
        mygames_ddz_dmc_episode result = {0};
        for (i = 0u; i < count; ++i) {
            mygames_ddz_dmc_sample sample;
            (void)read_record(src + DMC_HEADER + i * DMC_RECORD,
                              &sample, episode_id, model_version, rules);
            out_samples[i] = sample;
        }
        result.samples = out_samples;
        result.capacity = sample_capacity;
        result.count = count;
        result.episode_id = episode_id;
        result.model_version = model_version;
        result.rules_version = rules;
        result.next_turn_id = (uint64_t)count + UINT64_C(1);
        result.landlord_seat = landlord;
        result.terminal_winner = winner;
        result.terminated = true;
        result.sealed = true;
        result.objective = (mygames_ddz_dmc_objective)objective;
        *out_episode = result;
    }
    *out_required_samples = count;
    return MYGAMES_DDZ_REPLAY_OK;
}
