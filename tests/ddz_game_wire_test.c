#include "mygames/ddz_game_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
    exit(1); \
} } while (0)

static void valid(const char *wire, mygames_ddz_game_command_kind kind,
                  uint64_t game, uint64_t turn, uint64_t cards, uint8_t bid) {
    mygames_ddz_game_wire_command command = {0};
    REQUIRE(mygames_ddz_game_wire_decode(wire, strlen(wire), &command)
            == MYGAMES_WIRE_OK);
    REQUIRE(command.kind == kind);
    REQUIRE(!strcmp(command.request_id, "req-1"));
    REQUIRE(command.game_id == game && command.turn_id == turn);
    REQUIRE(command.cards_mask == cards && command.bid_score == bid);
}
static void invalid(const char *wire) {
    mygames_ddz_game_wire_command original = {0}, cmd;
    original.kind = MYGAMES_DDZ_GAME_PASS;
    original.game_id = UINT64_C(0x1234);
    original.turn_id = UINT64_C(0xdeadbeef);
    cmd = original;
    REQUIRE(mygames_ddz_game_wire_decode(wire, strlen(wire), &cmd)
            != MYGAMES_WIRE_OK);
    REQUIRE(memcmp(&original, &cmd, sizeof(cmd)) == 0);
}

int main(void) {
    static const char max_game[] = "18446744073709551615";
    char buffer[200];
    int n;
    valid("{\"v\":1,\"type\":\"game.start\",\"request_id\":\"req-1\"}",
          MYGAMES_DDZ_GAME_START, 0u, 0u, 0u, 0u);
    valid(" { \"request_id\":\"req-1\",\"type\":\"game.bid\","
          "\"game_id\":\"1\",\"bid\":3,\"turn_id\":\"7\",\"v\":1 }",
          MYGAMES_DDZ_GAME_BID, 1u, 7u, 0u, 3u);
    valid("{\"v\":1,\"type\":\"game.bid\",\"request_id\":\"req-1\","
          "\"game_id\":\"2\",\"turn_id\":\"3\",\"bid\":0}",
          MYGAMES_DDZ_GAME_BID, 2u, 3u, 0u, 0u);
    valid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
          "\"game_id\":\"4294967298\",\"turn_id\":\"9007199254740993\","
          "\"cards_hex\":\"00000000000001\"}",
          MYGAMES_DDZ_GAME_PLAY, UINT64_C(4294967298),
          UINT64_C(9007199254740993), UINT64_C(1), 0u);
    valid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
          "\"game_id\":\"30\",\"turn_id\":\"9\"}",
          MYGAMES_DDZ_GAME_PASS, 30u, 9u, 0u, 0u);
    valid("{\"v\":1,\"type\":\"game.snapshot.get\","
          "\"request_id\":\"req-1\",\"game_id\":\"6\"}",
          MYGAMES_DDZ_GAME_SNAPSHOT, 6u, 0u, 0u, 0u);
    valid("{\"v\":1,\"type\":\"game.hand.get\","
          "\"request_id\":\"req-1\",\"game_id\":\"6\"}",
          MYGAMES_DDZ_GAME_HAND, 6u, 0u, 0u, 0u);
    n=snprintf(buffer, sizeof(buffer),
               "{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
               "\"game_id\":\"%s\",\"turn_id\":\"1\"}", max_game);
    REQUIRE(n > 0 && (size_t)n < sizeof(buffer));
    valid(buffer, MYGAMES_DDZ_GAME_PASS, UINT64_MAX, 1u, 0u, 0u);

    invalid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"cards_hex\":\"00000000000000\"}");
    invalid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"cards_hex\":\"ffffffffffffff\"}");
    invalid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"cards_hex\":\"0000000000000A\"}");
    invalid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"cards_hex\":\"1\"}");
    invalid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
            "\"game_id\":\"00\",\"turn_id\":\"1\"}");
    invalid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
            "\"game_id\":\"18446744073709551616\",\"turn_id\":\"1\"}");
    invalid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
            "\"game_id\":1,\"turn_id\":\"1\"}");
    invalid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"turn_id\":\"2\"}");
    invalid("{\"v\":1,\"type\":\"game.pass\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"1\",\"cards_hex\":\"00000000000001\"}");
    invalid("{\"v\":1,\"type\":\"game.start\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\"}");
    invalid("{\"v\":1,\"type\":\"game.hand.get\",\"request_id\":\"req-1\"}");
    invalid("{\"v\":1,\"type\":\"game.bid\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"2\",\"bid\":4}");
    invalid("{\"v\":1,\"type\":\"game.bid\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"2\",\"bid\":01}");
    invalid("{\"v\":1,\"type\":\"game.bid\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"2\",\"bid\":\"2\"}");
    invalid("{\"v\":1,\"type\":\"game.play\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"2\",\"cards_hex\":\"00000000000001\","
            "\"resume_token\":\"secret\"}");
    invalid("{\"v\":1,\"type\":\"game.bid\",\"request_id\":\"req-1\","
            "\"game_id\":\"1\",\"turn_id\":\"2\",\"bid\":1,}");
    invalid("{\"v\":2,\"type\":\"game.start\",\"request_id\":\"req-1\"}");
    invalid("{\"v\":1,\"type\":\"game.start\",\"request_id\":\"req\\-1\"}");
    invalid("{\"v\":1,\"type\":\"game.cheat\",\"request_id\":\"req-1\"}");
    {
        char oversized[MYGAMES_WIRE_MAX_BYTES+1u] = {0};
        mygames_ddz_game_wire_command result = {0};
        REQUIRE(mygames_ddz_game_wire_decode(oversized,sizeof(oversized),&result)
                == MYGAMES_WIRE_TOO_LARGE);
    }
    puts("mygames.ddz_game_wire: PASS");
    return 0;
}
