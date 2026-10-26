#include "mygames/session_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void seed(uint8_t out[MYGAMES_SESSION_TICKET_BYTES], uint8_t value) {
    size_t i;
    for (i = 0u; i < MYGAMES_SESSION_TICKET_BYTES; ++i) out[i] = (uint8_t)(value + i);
}
static void main_contract(void) {
    mygames_session_registry sessions;
    uint8_t a[MYGAMES_SESSION_TICKET_BYTES];
    uint8_t b[MYGAMES_SESSION_TICKET_BYTES];
    uint8_t zeros[MYGAMES_SESSION_TICKET_BYTES] = {0};
    char old[MYGAMES_SESSION_TICKET_HEX + 1u];
    char rotated[MYGAMES_SESSION_TICKET_HEX + 1u];
    size_t original, claimed;
    mygames_player_id expired[MYGAMES_SESSION_CAPACITY] = {0};
    const mygames_session_record *r;
    seed(a, 1u); seed(b, 42u);
    mygames_sessions_init(&sessions);
    assert(mygames_sessions_issue(&sessions, zeros, &original) ==
           MYGAMES_SESSION_INVALID);
    assert(mygames_sessions_issue(&sessions, a, &original) == MYGAMES_SESSION_OK);
    r = mygames_sessions_at(&sessions, original);
    assert(r && r->player == 1u && r->generation == 1u && r->attached);
    memcpy(old, r->ticket, sizeof(old));
    assert(strlen(old) == 64u);
    assert(mygames_sessions_issue(&sessions, a, &claimed) ==
           MYGAMES_SESSION_ENTROPY_COLLISION);
    assert(mygames_sessions_resume(&sessions, old, b, 10u, &claimed) ==
           MYGAMES_SESSION_ACTIVE);
    assert(mygames_sessions_detach(&sessions, original, 2u, 100u, 5000u) ==
           MYGAMES_SESSION_NOT_FOUND);
    assert(mygames_sessions_detach(&sessions, original, 1u, 100u, 5000u) ==
           MYGAMES_SESSION_OK);
    assert(mygames_sessions_resume(&sessions, old, a, 101u, &claimed) ==
           MYGAMES_SESSION_ENTROPY_COLLISION);
    assert(mygames_sessions_resume(&sessions, old, b, 101u, &claimed) ==
           MYGAMES_SESSION_OK);
    assert(claimed == original);
    r = mygames_sessions_at(&sessions, original);
    assert(r && r->attached && r->player == 1u && r->generation == 2u);
    memcpy(rotated, r->ticket, sizeof(rotated));
    assert(strcmp(rotated, old) != 0);
    assert(mygames_sessions_resume(&sessions, old, a, 102u, &claimed) ==
           MYGAMES_SESSION_NOT_FOUND);
    assert(mygames_sessions_detach(&sessions, original, 1u, 102u, 5u) ==
           MYGAMES_SESSION_NOT_FOUND);
    assert(mygames_sessions_detach(&sessions, original, 2u, 102u, 5u) ==
           MYGAMES_SESSION_OK);
    assert(mygames_sessions_resume(&sessions, rotated, a, 107u, &claimed) ==
           MYGAMES_SESSION_EXPIRED);
    assert(mygames_sessions_collect_expired(&sessions, 106u, expired) == 0u);
    assert(mygames_sessions_collect_expired(&sessions, 107u, expired) == 1u);
    assert(expired[0] == 1u);
    assert(mygames_sessions_at(&sessions, original) == NULL);
    assert(mygames_sessions_issue(&sessions, b, &claimed) == MYGAMES_SESSION_OK);
    assert(mygames_sessions_at(&sessions, claimed)->player == 2u);
    assert(mygames_sessions_discard(&sessions, claimed) == MYGAMES_SESSION_OK);
    {
        size_t recovered = MYGAMES_SESSION_CAPACITY;
        const mygames_session_record *record;
        assert(mygames_sessions_import_active(&sessions, 101u, 3u, a,
                                              &recovered) == MYGAMES_SESSION_OK);
        record = mygames_sessions_at(&sessions, recovered);
        assert(record && record->player == 101u && record->generation == 3u);
        assert(record->attached && sessions.next_player == 101u);
        assert(mygames_sessions_import_active(&sessions, 101u, 4u, b,
                                              &claimed) == MYGAMES_SESSION_ACTIVE);
        assert(mygames_sessions_discard(&sessions, recovered) == MYGAMES_SESSION_OK);
        assert(mygames_sessions_import_active(&sessions, 0u, 1u, b,
                                              &claimed) == MYGAMES_SESSION_INVALID);
    }
    assert(mygames_sessions_at(&sessions, claimed) == NULL);
}
static void capacity_contract(void) {
    mygames_session_registry sessions;
    uint8_t random_bytes[MYGAMES_SESSION_TICKET_BYTES];
    size_t slot;
    unsigned i;
    mygames_sessions_init(&sessions);
    for (i = 0u; i < MYGAMES_SESSION_CAPACITY; ++i) {
        seed(random_bytes, (uint8_t)(i + 1u));
        assert(mygames_sessions_issue(&sessions, random_bytes, &slot) ==
               MYGAMES_SESSION_OK);
    }
    seed(random_bytes, 100u);
    assert(mygames_sessions_issue(&sessions, random_bytes, &slot) ==
           MYGAMES_SESSION_FULL);
    assert(mygames_sessions_discard(&sessions, MYGAMES_SESSION_CAPACITY) ==
           MYGAMES_SESSION_INVALID);
    assert(mygames_sessions_at(&sessions, MYGAMES_SESSION_CAPACITY) == NULL);
}
int main(void) {
    main_contract();
    capacity_contract();
    puts("mygames.sessions: PASS");
    return 0;
}
