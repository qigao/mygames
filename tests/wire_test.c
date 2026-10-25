#include "mygames/wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void valid(const char *text, mygames_command_kind kind) {
    mygames_wire_command cmd;
    assert(mygames_wire_decode_command(text, strlen(text), &cmd) == MYGAMES_WIRE_OK);
    assert(cmd.kind == kind);
}
static void invalid(const char *text) {
    mygames_wire_command cmd;
    assert(mygames_wire_decode_command(text, strlen(text), &cmd) != MYGAMES_WIRE_OK);
}
int main(void) {
    mygames_wire_command cmd;
    static const unsigned char binary[] = {'{','"', 'v', '"', ':', '1', '}', 0, 'x'};
    static const char big[MYGAMES_WIRE_MAX_BYTES + 1u] = {0};
    valid("{\"v\":1,\"type\":\"client.ping\"}", MYGAMES_COMMAND_PING);
    valid(" { \"type\" : \"room.create\" , \"request_id\" : \"req-1\" , \"v\" : 1 } ",
          MYGAMES_COMMAND_CREATE_ROOM);
    valid("{\"v\":1,\"type\":\"room.join\",\"room_id\":\"room-1\",\"request_id\":\"r2\"}",
          MYGAMES_COMMAND_JOIN_ROOM);
    valid("{\"v\":1,\"type\":\"room.ready\",\"ready\":false,\"request_id\":\"r3\"}",
          MYGAMES_COMMAND_READY);
    valid("{\"v\":1,\"type\":\"room.leave\",\"request_id\":\"r4\"}",
          MYGAMES_COMMAND_LEAVE_ROOM);
    valid("{\"v\":1,\"type\":\"room.snapshot.get\",\"request_id\":\"r5\"}",
          MYGAMES_COMMAND_GET_SNAPSHOT);
    assert(mygames_wire_decode_command(binary, sizeof(binary), &cmd) == MYGAMES_WIRE_INVALID);
    assert(mygames_wire_decode_command(big, sizeof(big), &cmd) == MYGAMES_WIRE_TOO_LARGE);
    assert(mygames_wire_decode_command(NULL, 0u, &cmd) == MYGAMES_WIRE_INVALID);
    assert(mygames_wire_decode_command("{}", 2u, NULL) == MYGAMES_WIRE_INVALID);
    invalid("{");
    invalid("{}");
    invalid("{\"v\":1,\"type\":\"client.ping\",}");
    invalid("{\"v\":1,\"type\":\"client.ping\"}x");
    invalid("{\"v\":2,\"type\":\"client.ping\"}");
    invalid("{\"v\":10,\"type\":\"client.ping\"}");
    invalid("{\"v\":1.0,\"type\":\"client.ping\"}");
    invalid("{\"v\":1,\"v\":1,\"type\":\"client.ping\"}");
    invalid("{\"v\":1,\"type\":\"client.ping\",\"unknown\":1}");
    invalid("{\"v\":1,\"type\":\"room.join\",\"request_id\":\"a\"}");
    invalid("{\"v\":1,\"type\":\"room.create\"}");
    invalid("{\"v\":1,\"type\":\"room.create\",\"request_id\":\"a\",\"ready\":true}");
    invalid("{\"v\":1,\"type\":\"room.ready\",\"request_id\":\"a\",\"ready\":null}");
    invalid("{\"v\":1,\"type\":\"room.ready\",\"request_id\":\"a\",\"ready\":truee}");
    invalid("{\"v\":1,\"type\":\"room.join\",\"request_id\":\"a\",\"room_id\":\"x/y\"}");
    invalid("{\"v\":1,\"type\":\"room.join\",\"request_id\":\"a\",\"room_id\":\"x\\\\y\"}");
    invalid("{\"v\":1,\"type\":\"room.create\",\"request_id\":\"a\\\"b\"}");
    invalid("{\"v\":1,\"type\":\"room.ready\",\"request_id\":\"a\",\"ready\":[true]}");
    invalid("{\"v\":1,\"type\":\"room.delete\",\"request_id\":\"r1\"}");
    {
        char json[160];
        char ticket[65];
        size_t i;
        for (i = 0u; i < 64u; ++i) ticket[i] = 'a';
        ticket[64] = '\\0';
        (void)snprintf(json, sizeof(json),
            "{\\\"v\\\":1,\\\"type\\\":\\\"session.resume\\\",\\\"resume_token\\\":\\\"%s\\\"}", ticket);
        valid(json, MYGAMES_COMMAND_RESUME_SESSION);
        ticket[0] = 'G';
        (void)snprintf(json, sizeof(json),
            "{\\\"v\\\":1,\\\"type\\\":\\\"session.resume\\\",\\\"resume_token\\\":\\\"%s\\\"}", ticket);
        invalid(json);
        invalid("{\\\"v\\\":1,\\\"type\\\":\\\"session.resume\\\"}");
    }
    puts("mygames.wire: PASS");
    return 0;
}
