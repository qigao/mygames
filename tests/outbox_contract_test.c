#include "mygames/outbox_dispatch.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool accept(const char *text, uint64_t epoch, uint64_t seq) {
    return mygames_outbox_public_event_validate(
        (const uint8_t *)text, strlen(text), epoch, seq);
}
int main(void) {
    static const char command[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":2,\"seq\":3}";
    static const char expired[] =
        "{\"v\":1,\"type\":\"room.session_expired\",\"room_id\":\"room-23\","
        "\"player_id\":\"101\",\"room_epoch\":2,\"seq\":4}";
    static const char injected[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":2,\"seq\":3,"
        "\"resume_token\":\"SECRET\"}";
    static const char hand[] =
        "{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
        "\"room_epoch\":1,\"revision\":2,\"seq\":3,\"cards\":[1,2,3]}";
    assert(accept(command, 1u, 3u));
    assert(accept(command, 2u, 3u)); /* past Owner epoch after recovery */
    assert(accept(expired, 2u, 4u));
    assert(!accept(command, 1u, 2u)); /* DB seq mismatch */
    assert(!accept(command, 0u, 3u));
    assert(!accept(command, 1u, 0u));
    assert(!accept(expired, 1u, 4u)); /* future Owner epoch */
    assert(!accept(injected, 1u, 3u));
    assert(!accept(hand, 1u, 3u));
    assert(!accept("{\"v\":1,\"type\":\"room.unapproved\",\"room_id\":\"room-1\","
                   "\"room_epoch\":1,\"revision\":2,\"seq\":3}",1u,3u));
    assert(!accept("{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-0\","
                   "\"room_epoch\":1,\"revision\":2,\"seq\":3}",1u,3u));
    assert(!accept("{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
                   "\"room_epoch\":1,\"revision\":2,\"seq\":3}x",1u,3u));
    assert(!accept("{\"v\":1,\"type\":\"room.command_accepted\",\"room_id\":\"room-1\","
                   "\"room_epoch\":1,\"revision\":2,\"seq\":3,\"seq\":3}",1u,3u));
    assert(!accept("{\"v\":1,\"type\":\"room.session_expired\",\"room_id\":\"room-1\","
                   "\"player_id\":\"0\",\"room_epoch\":1,\"seq\":4}",1u,4u));
    puts("mygames.outbox_contract: PASS");
    return 0;
}
