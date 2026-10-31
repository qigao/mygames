#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
/* Opt-in Linux OFFLINE training corpus producer; never a CFlow/HTTP owner.
 * Exact v1 C11 rules, true OS CSPRNG decks, own-seat-only policy callback,
 * private complete-episode Replay v1 and atomic NO-CLOBBER publication.
 * No second rules engine, network access or central GameStore writer. */
#include "mygames/ddz_dmc_selfplay.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(__linux__)
#error "The offline corpus publishing CLI currently requires Linux openat()"
#endif

#define DMC_GENERATOR_MAX_EPISODES 96u
#define DMC_GENERATOR_MAX_LEGAL 65536u

/* External source identity is NOT Replay-v1's numeric model_version.
 * All these heuristics are untrained behavior model_version=1; a verified
 * private source manifest must preserve this separately selected policy. */
typedef enum dmc_baseline_mode {
    DMC_BASELINE_MIXED_V1 = 0,
    DMC_BASELINE_SHED_MOST_V1,
    DMC_BASELINE_LEGAL_INDEX_V1
} dmc_baseline_mode;

typedef struct dmc_baseline {
    uint64_t episode_id;
    dmc_baseline_mode behavior;
    uint64_t decisions;
    uint64_t role_decisions[MYGAMES_DDZ_SEATS];
} dmc_baseline;

static unsigned count_bits(uint64_t mask) {
    unsigned count = 0u;
    while (mask != UINT64_C(0)) {
        mask &= mask - UINT64_C(1);
        ++count;
    }
    return count;
}

static uint32_t scramble(uint32_t n) {
    n ^= n >> 16u;
    n *= UINT32_C(0x7feb352d);
    n ^= n >> 15u;
    n *= UINT32_C(0x846ca68b);
    n ^= n >> 16u;
    return n;
}

static bool parse_behavior(const char *name, dmc_baseline_mode *out) {
    if (!name || !out) return false;
    if (strcmp(name, "mixed-v1") == 0)
        *out = DMC_BASELINE_MIXED_V1;
    else if (strcmp(name, "shed-most") == 0)
        *out = DMC_BASELINE_SHED_MOST_V1;
    else if (strcmp(name, "legal-index") == 0)
        *out = DMC_BASELINE_LEGAL_INDEX_V1;
    else
        return false;
    return true;
}

/* Explicit non-learning behavior source: the acting player's own hand and
 * PUBLIC observation are the ONLY policy inputs. The legacy default
 * mixture is retained bit-for-bit when --behavior is omitted. The hashed
 * legal-index seed is episode identity, never another seat's hidden cards. */
static bool select_baseline(
    void *context, const mygames_ddz_ai_observation *observation,
    const uint64_t *legal, size_t count, uint64_t *out) {
    dmc_baseline *baseline = (dmc_baseline *)context;
    unsigned actor, i;
    if (!baseline || !observation || !legal || !out || count == 0u)
        return false;
    actor = observation->public_history.current.turn_seat;
    if (actor >= MYGAMES_DDZ_SEATS ||
        observation->public_history.current.remaining[actor] == 0u)
        return false;
    if (baseline->behavior != DMC_BASELINE_MIXED_V1 &&
        baseline->behavior != DMC_BASELINE_SHED_MOST_V1 &&
        baseline->behavior != DMC_BASELINE_LEGAL_INDEX_V1)
        return false;
    if (baseline->behavior == DMC_BASELINE_SHED_MOST_V1 ||
        (baseline->behavior == DMC_BASELINE_MIXED_V1 &&
         ((baseline->episode_id + (uint64_t)actor) & UINT64_C(1)) == 0u)) {
        size_t best = 0u;
        unsigned most = 0u;
        for (i = 0u; i < count; ++i) {
            const unsigned cards = count_bits(legal[i]);
            if (cards > most || (cards == most && legal[i] < legal[best])) {
                best = i;
                most = cards;
            }
        }
        *out = legal[best]; /* shed-most baseline */
    } else {
        const uint64_t hand = observation->own_hand;
        const uint64_t turn = observation->public_history.current.turn_id;
        const uint32_t key = (uint32_t)hand ^ (uint32_t)(hand >> 32u) ^
            (uint32_t)turn ^ (uint32_t)(baseline->episode_id) ^
            (uint32_t)(baseline->episode_id >> 32u) ^
            ((uint32_t)actor * UINT32_C(0x9e3779b9));
        *out = legal[(size_t)scramble(key) % count]; /* legal-index baseline */
    }
    ++baseline->decisions;
    ++baseline->role_decisions[observation->role];
    return true;
}

static bool parse_u64(const char *input, uint64_t *out) {
    unsigned long long value;
    char *end = NULL;
    if (!input || !*input || *input == '-') return false;
    errno = 0;
    value = strtoull(input, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0u)
        return false;
    *out = (uint64_t)value;
    return (unsigned long long)*out == value;
}

static bool prepare_room(mygames_room *room) {
    unsigned int seat = 99u;
    return mygames_room_init(room, "private-dmc-offline", 101u) ==
            MYGAMES_ROOM_OK &&
        mygames_room_join(room, 202u, &seat) == MYGAMES_ROOM_OK &&
        seat == 1u &&
        mygames_room_join(room, 303u, &seat) == MYGAMES_ROOM_OK &&
        seat == 2u &&
        mygames_room_set_ready(room, 101u, true) == MYGAMES_ROOM_OK &&
        mygames_room_set_ready(room, 202u, true) == MYGAMES_ROOM_OK &&
        mygames_room_set_ready(room, 303u, true) == MYGAMES_ROOM_OK &&
        mygames_room_start(room, 101u) == MYGAMES_ROOM_OK;
}

/* A full private directory must exist and be owned exclusively by the
 * current local identity. Do not follow symlinks for the directory. */
static int open_private_directory(const char *path) {
    struct stat info;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const uint8_t *bytes, size_t count) {
    size_t sent = 0u;
    while (sent < count) {
        const ssize_t amount = write(fd, bytes + sent, count - sent);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return false;
        sent += (size_t)amount;
    }
    return true;
}

/* No-clobber durable publication: private .partial is written+fsynced,
 * hard-linked as a NEW final name, then the temporary name is removed.
 * Interrupted process may leave a .partial file; never treat it as Replay.
 * Directory fsync makes the name transition durable on supported FS. */
static bool publish_private(int dirfd, uint64_t episode_id,
                            const uint8_t *bytes, size_t size) {
    char final_name[64], temp_name[72];
    int file;
    bool okay;
    const int n = snprintf(final_name, sizeof(final_name),
        "episode-%" PRIu64 ".mgdmc", episode_id);
    const int m = snprintf(temp_name, sizeof(temp_name),
        ".episode-%" PRIu64 ".mgdmc.partial", episode_id);
    if (n <= 0 || (size_t)n >= sizeof(final_name) ||
        m <= 0 || (size_t)m >= sizeof(temp_name))
        return false;
    file = openat(dirfd, temp_name,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) return false;
    okay = write_all(file, bytes, size) && fsync(file) == 0;
    if (close(file) != 0) okay = false;
    if (okay) {
        /* Atomic no-replace; includes the original bytes under a new name.
         * The destination might have appeared in a concurrent process:
         * linkat then fails, never replacing that older private Replay. */
        okay = linkat(dirfd, temp_name, dirfd, final_name, 0) == 0;
    }
    if (unlinkat(dirfd, temp_name, 0) != 0) okay = false;
    if (okay) okay = fsync(dirfd) == 0;
    return okay;
}

int main(int argc, char **argv) {
    const char *path;
    uint64_t episodes_u64 = 0u, first_id = 0u;
    unsigned games, i;
    mygames_ddz_dmc_objective objective;
    dmc_baseline_mode behavior = DMC_BASELINE_MIXED_V1;
    const char *behavior_name = "mixed-v1";
    mygames_ddz_dmc_sample *samples = NULL;
    uint8_t *bytes = NULL;
    mygames_room room = {0};
    uint64_t total_moves = UINT64_C(0);
    uint64_t total_role[MYGAMES_DDZ_SEATS] = {0u};
    int dirfd;
    if ((argc != 9 && argc != 11) ||
        strcmp(argv[1], "--private-dir") != 0 ||
        strcmp(argv[3], "--episodes") != 0 ||
        strcmp(argv[5], "--first-id") != 0 ||
        strcmp(argv[7], "--objective") != 0 ||
        (argc == 11 && strcmp(argv[9], "--behavior") != 0) ||
        !parse_u64(argv[4], &episodes_u64) ||
        !parse_u64(argv[6], &first_id) ||
        episodes_u64 > DMC_GENERATOR_MAX_EPISODES ||
        first_id > (uint64_t)INT64_MAX - (episodes_u64 - 1u)) {
        fputs("usage: mygames_ddz_dmc_generate --private-dir PRIVATE_0700 "
              "--episodes 1..96 --first-id POSITIVE --objective win-sign|game-points "
              "[--behavior mixed-v1|shed-most|legal-index]\n",
              stderr);
        return 2;
    }
    path = argv[2];
    if (strcmp(argv[8], "win-sign") == 0)
        objective = MYGAMES_DDZ_DMC_WIN_SIGN;
    else if (strcmp(argv[8], "game-points") == 0)
        objective = MYGAMES_DDZ_DMC_GAME_POINTS;
    else {
        fputs("unsupported DMC objective\n", stderr);
        return 2;
    }
    if (argc == 11) {
        if (!parse_behavior(argv[10], &behavior)) {
            fputs("unsupported DMC source behavior; no implicit fallback\n", stderr);
            return 2;
        }
        behavior_name = argv[10];
    }
    dirfd = open_private_directory(path);
    if (dirfd < 0) {
        fputs("private Replay directory must exist, be owned by caller "
              "and deny group/world access; symlinks rejected\n", stderr);
        return 2;
    }
    samples = (mygames_ddz_dmc_sample *)calloc(
        MYGAMES_DDZ_DMC_MAX_STEPS, sizeof(*samples));
    bytes = (uint8_t *)malloc(MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES);
    if (!samples || !bytes || !prepare_room(&room)) {
        fputs("offline DMC initialization failed\n", stderr);
        goto failed;
    }
    games = (unsigned)episodes_u64;
    for (i = 0u; i < games; ++i) {
        const uint64_t episode_id = first_id + (uint64_t)i;
        mygames_ddz_dmc_selfplay_config cfg = {0};
        mygames_ddz_dmc_episode episode = {0};
        mygames_ddz_settlement settlement = {0};
        dmc_baseline policy = {0};
        size_t size = 0u;
        unsigned role;
        cfg.room_epoch = UINT64_C(1);
        cfg.game_id = episode_id;
        cfg.episode_id = episode_id;
        cfg.model_version = UINT64_C(1); /* known, UNTRAINED baseline v1 */
        cfg.rules_version = 1u;
        cfg.objective = objective;
        cfg.max_legal_actions = DMC_GENERATOR_MAX_LEGAL;
        cfg.offline_landlord_seat = (uint8_t)((episode_id - UINT64_C(1)) % 3u);
        policy.episode_id = episode_id;
        policy.behavior = behavior;
        if (mygames_ddz_dmc_selfplay_run(&room, &cfg,
                select_baseline, &policy, samples,
                MYGAMES_DDZ_DMC_MAX_STEPS, &episode, &settlement) !=
                    MYGAMES_DDZ_SELFPLAY_OK ||
            !episode.sealed || !episode.terminated ||
            policy.decisions != episode.count ||
            mygames_ddz_dmc_replay_encode(&episode, bytes,
                MYGAMES_DDZ_DMC_REPLAY_MAX_BYTES, &size) !=
                    MYGAMES_DDZ_REPLAY_OK) {
            fputs("native C11 Self-Play or Replay admission failed; "
                  "no incomplete episode published\n", stderr);
            goto failed;
        }
        if (!publish_private(dirfd, episode_id, bytes, size)) {
            fputs("private Replay publication failed; no overwrite allowed\n",
                  stderr);
            goto failed;
        }
        total_moves += (uint64_t)episode.count;
        for (role = 0u; role < MYGAMES_DDZ_SEATS; ++role)
            total_role[role] += policy.role_decisions[role];
    }
    if (argc == 11) {
        /* The explicit behavior label is public AGGREGATE provenance only.
         * Replay-v1's model_version stays 1 for all untrained baselines;
         * an independently verified private source manifest is still
         * REQUIRED before multi-source training admission. */
        printf("MYGAMES_PRIVATE_CORPUS_V2 behavior=%s episodes=%u steps=%" PRIu64
               " landlord=%" PRIu64 " landlord_up=%" PRIu64
               " landlord_down=%" PRIu64 " objective=%s\n",
               behavior_name, games, total_moves, total_role[0], total_role[1],
               total_role[2], argv[8]);
    } else {
        printf("MYGAMES_PRIVATE_CORPUS_V1 episodes=%u steps=%" PRIu64
               " landlord=%" PRIu64 " landlord_up=%" PRIu64
               " landlord_down=%" PRIu64 " objective=%s\n",
               games, total_moves, total_role[0], total_role[1],
               total_role[2], argv[8]);
    }
    free(samples);
    free(bytes);
    (void)close(dirfd);
    return 0;
failed:
    free(samples);
    free(bytes);
    (void)close(dirfd);
    return 1;
}
