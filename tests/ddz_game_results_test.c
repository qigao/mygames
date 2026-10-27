#include "mygames/room_store.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_store(mygames_store_status actual, mygames_store_status expected) {
    if (actual != expected) {
        fprintf(stderr, "Game results status: got %d expected %d\n",
                (int)actual, (int)expected);
        abort();
    }
}
static void erase_store(const char *path) {
    char filename[1024];
    int length;
    (void)remove(path);
    length = snprintf(filename, sizeof(filename), "%s-wal", path);
    if (length > 0 && (size_t)length < sizeof(filename)) (void)remove(filename);
    length = snprintf(filename, sizeof(filename), "%s-shm", path);
    if (length > 0 && (size_t)length < sizeof(filename)) (void)remove(filename);
}
static void make_started_room(mygames_room_directory *rooms) {
    const mygames_room *room = NULL;
    unsigned int seat = 99u;
    mygames_room_directory_init(rooms);
    assert(mygames_room_directory_create(rooms, 101u, &room) == MYGAMES_ROOM_OK);
    assert(room && strcmp(room->id, "room-1") == 0);
    assert(mygames_room_directory_join(rooms,"room-1",202u,&seat)==MYGAMES_ROOM_OK);
    assert(mygames_room_directory_join(rooms,"room-1",303u,&seat)==MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms,101u,true)==MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms,202u,true)==MYGAMES_ROOM_OK);
    assert(mygames_room_directory_ready(rooms,303u,true)==MYGAMES_ROOM_OK);
    assert(mygames_room_directory_start(rooms,101u)==MYGAMES_ROOM_OK);
}
static uint64_t mask_lowest(uint64_t cards) {
    assert(cards);
    return cards & (~cards + UINT64_C(1));
}
static void assert_total(mygames_room_store *store, mygames_player_id player,
                         int64_t points, uint64_t games) {
    int64_t actual = INT64_C(0x123456);
    uint64_t counted = UINT64_C(777);
    expect_store(mygames_room_store_player_game_points(
        store,"room-1",player,&actual,&counted),MYGAMES_STORE_OK);
    assert(actual == points && counted == games);
}
static uint64_t commit_game(mygames_room_store *store,
                            const mygames_room_directory *rooms,
                            const mygames_ddz_match *before,
                            const mygames_ddz_match *candidate,
                            mygames_player_id player, uint64_t epoch,
                            uint64_t seq, const char *id, uint8_t fill) {
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    static const char ack[] = "{\"v\":1,\"type\":\"game.ack\"}";
    uint64_t next_seq = UINT64_C(0);
    memset(digest, fill ? fill : 1u, sizeof(digest));
    expect_store(mygames_room_store_commit_game(
        store,player,rooms,"room-1",before,candidate,epoch,seq,
        id,digest,ack,sizeof(ack)-1u,&next_seq),MYGAMES_STORE_OK);
    assert(next_seq == seq+UINT64_C(1));
    return next_seq;
}
int main(int argc, char **argv) {
    mygames_room_directory rooms;
    mygames_room_store *store = NULL;
    const mygames_room *room;
    mygames_ddz_match match = {0}, next = {0}, last_before = {0}, win = {0};
    mygames_ddz_match rematch = {0}, restarted = {0};
    mygames_ddz_bid_result bid = {0};
    mygames_ddz_action_result move = {0};
    mygames_store_game_result result = {0}, sentinel = {0};
    uint8_t digest[MYGAMES_STORE_COMMAND_DIGEST_BYTES];
    char request[48];
    static const char room_ack[] = "{\"v\":1,\"type\":\"room.ack\"}";
    static const char room_event[] = "{\"v\":1,\"type\":\"room.command_accepted\"}";
    static const char game_ack[] = "{\"v\":1,\"type\":\"game.ack\"}";
    uint64_t seq = UINT64_C(0), final_seq = UINT64_C(0), returned_seq = UINT64_C(0);
    uint64_t claimed_epoch = 0u, loaded_game_seq = 0u;
    unsigned int i;
    if (argc != 3) return 2;
    erase_store(argv[2]);
    make_started_room(&rooms);
    room = mygames_room_directory_get(&rooms,"room-1");
    assert(room && room->phase == MYGAMES_ROOM_STARTING);
    expect_store(mygames_room_store_open(argv[1],argv[2],&store),MYGAMES_STORE_OK);
    memset(digest,0x11,sizeof(digest));
    expect_store(mygames_room_store_commit(store,101u,&rooms,1u,0u,
        "room-start",digest,room_ack,sizeof(room_ack)-1u,
        room_event,sizeof(room_event)-1u,&seq),MYGAMES_STORE_OK);
    assert(seq == 1u);
    sentinel.game_id = UINT64_C(0xdead);
    result = sentinel;
    expect_store(mygames_room_store_game_result(store,"room-1",8u,&result),
                 MYGAMES_STORE_NOT_FOUND);
    assert(memcmp(&result,&sentinel,sizeof(result)) == 0);
    assert_total(store,101u,0,0);
    assert_total(store,202u,0,0);
    assert_total(store,303u,0,0);

    assert(mygames_ddz_match_start(&match,room,1u,8u)==MYGAMES_DDZ_MATCH_OK);
    seq = commit_game(store,&rooms,NULL,&match,101u,1u,seq,"game-start",0x21u);
    assert(mygames_ddz_match_bid(&match,1u,8u,101u,1u,3u,&next,&bid)==
           MYGAMES_DDZ_MATCH_OK);
    seq = commit_game(store,&rooms,&match,&next,101u,1u,seq,"bid3",0x22u);
    match = next;
    /* Twenty REAL independent landlord card plays. Each first 19 cards
     * are followed by two authentic farmer PASS responses. */
    for (i=0u;i<20u;++i) {
        const uint64_t turn=match.round.turn_id;
        const uint64_t single=mask_lowest(match.round.hands[0]);
        const int n=snprintf(request,sizeof(request),"play-%02u",i);
        assert(n>0 && (size_t)n<sizeof(request));
        assert(mygames_ddz_match_play(&match,1u,8u,101u,turn,
                                     single,&next,&move)==MYGAMES_DDZ_MATCH_OK);
        if(i==19u){
            last_before = match;
            win = next;
            final_seq = seq+1u;
            /* Failure AFTER private state and per-player receipt, BEFORE
             * public Outbox means all facts incl ledger must rollback. */
            mygames_room_store_fail_after_command_once(store);
            uint64_t rejected_seq = UINT64_C(0x1234);
            memset(digest,0x77,sizeof(digest));
            expect_store(mygames_room_store_commit_game(
                store,101u,&rooms,"room-1",&match,&next,1u,seq,
                request,digest,game_ack,sizeof(game_ack)-1u,&rejected_seq),
                MYGAMES_STORE_BACKEND_ERROR);
            assert(rejected_seq == UINT64_C(0x1234));
            result = sentinel;
            expect_store(mygames_room_store_game_result(
                store,"room-1",8u,&result),MYGAMES_STORE_NOT_FOUND);
            assert(memcmp(&result,&sentinel,sizeof(result))==0);
            assert_total(store,101u,0,0);
            seq = commit_game(store,&rooms,&match,&next,101u,1u,seq,request,0x77u);
            match = next;
            assert(match.phase == MYGAMES_DDZ_MATCH_FINISHED);
            break;
        }
        seq = commit_game(store,&rooms,&match,&next,101u,1u,seq,request,
                          (uint8_t)(i+0x30u));
        match=next;
        assert(mygames_ddz_match_play(&match,1u,8u,202u,match.round.turn_id,
                                     UINT64_C(0),&next,&move)==MYGAMES_DDZ_MATCH_OK);
        {
            const int n=snprintf(request,sizeof(request),"pass-b-%02u",i);
            assert(n>0 && (size_t)n<sizeof(request));
        }
        seq=commit_game(store,&rooms,&match,&next,202u,1u,seq,request,
                        (uint8_t)(i+0x50u));
        match=next;
        assert(mygames_ddz_match_play(&match,1u,8u,303u,match.round.turn_id,
                                     UINT64_C(0),&next,&move)==MYGAMES_DDZ_MATCH_OK);
        {
            const int n=snprintf(request,sizeof(request),"pass-c-%02u",i);
            assert(n>0 && (size_t)n<sizeof(request));
        }
        seq=commit_game(store,&rooms,&match,&next,303u,1u,seq,request,
                        (uint8_t)(i+0x70u));
        match=next;
    }
    assert(seq==final_seq);
    expect_store(mygames_room_store_game_result(store,"room-1",8u,&result),
                 MYGAMES_STORE_OK);
    assert(result.game_id==8u && result.finish_sequence==final_seq &&
           result.players[0]==101u && result.players[1]==202u &&
           result.players[2]==303u);
    assert(result.score.bid_base==3u &&
           result.score.bombs_rockets==0u &&
           result.score.spring==MYGAMES_DDZ_SPRING_LANDLORD &&
           result.score.unit==6u &&
           result.score.delta[0]==12 && result.score.delta[1]==-6 &&
           result.score.delta[2]==-6);
    assert_total(store,101u,12,1);
    assert_total(store,202u,-6,1);
    assert_total(store,303u,-6,1);
    assert_total(store,404u,0,0);

    /* Do not credit again on exact request replay or conflicting digest. */
    returned_seq=0u;
    memset(digest,0x77,sizeof(digest));
    expect_store(mygames_room_store_commit_game(
        store,101u,&rooms,"room-1",&last_before,&win,1u,final_seq-1u,
        "play-19",digest,game_ack,sizeof(game_ack)-1u,&returned_seq),
        MYGAMES_STORE_REPLAY);
    assert(returned_seq==final_seq);
    assert_total(store,101u,12,1);
    digest[0]^=0xffu;
    expect_store(mygames_room_store_commit_game(
        store,101u,&rooms,"room-1",&last_before,&win,1u,final_seq-1u,
        "play-19",digest,game_ack,sizeof(game_ack)-1u,&returned_seq),
        MYGAMES_STORE_REQUEST_CONFLICT);
    assert_total(store,101u,12,1);

    assert(mygames_ddz_match_start(&rematch,room,1u,9u)==MYGAMES_DDZ_MATCH_OK);
    seq=commit_game(store,&rooms,&match,&rematch,101u,1u,seq,"rematch-9",0x88u);
    expect_store(mygames_room_store_game_result(store,"room-1",9u,&result),
                 MYGAMES_STORE_NOT_FOUND);
    assert_total(store,101u,12,1);

    /* SECOND real completed match with a DIFFERENT landlord: owner passes,
     * seat 1 bids 3, and seat 1 now plays all 20 cards while the two
     * actual farmers return two PASS commands per nonfinal trick. */
    match = rematch;
    assert(mygames_ddz_match_bid(&match,1u,9u,101u,1u,0u,&next,&bid)==
           MYGAMES_DDZ_MATCH_OK);
    seq=commit_game(store,&rooms,&match,&next,101u,1u,seq,"g2-bid-pass",0x89u);
    match=next;
    assert(mygames_ddz_match_bid(&match,1u,9u,202u,2u,3u,&next,&bid)==
           MYGAMES_DDZ_MATCH_OK);
    seq=commit_game(store,&rooms,&match,&next,202u,1u,seq,"g2-bid-three",0x8au);
    match=next;
    assert(match.round.landlord_seat==1u && match.round.remaining[1]==20u);
    for(i=0u;i<20u;++i) {
        const uint64_t single=mask_lowest(match.round.hands[1]);
        int n=snprintf(request,sizeof(request),"g2-play-%02u",i);
        assert(n>0 && (size_t)n<sizeof(request));
        assert(mygames_ddz_match_play(&match,1u,9u,202u,match.round.turn_id,
                                     single,&next,&move)==MYGAMES_DDZ_MATCH_OK);
        seq=commit_game(store,&rooms,&match,&next,202u,1u,seq,request,
                        (uint8_t)(i+0x21u));
        match=next;
        if (i==19u) break;
        assert(mygames_ddz_match_play(&match,1u,9u,303u,match.round.turn_id,
                                     UINT64_C(0),&next,&move)==MYGAMES_DDZ_MATCH_OK);
        n=snprintf(request,sizeof(request),"g2-c-pass-%02u",i);
        assert(n>0 && (size_t)n<sizeof(request));
        seq=commit_game(store,&rooms,&match,&next,303u,1u,seq,request,
                        (uint8_t)(i+0x42u));
        match=next;
        assert(mygames_ddz_match_play(&match,1u,9u,101u,match.round.turn_id,
                                     UINT64_C(0),&next,&move)==MYGAMES_DDZ_MATCH_OK);
        n=snprintf(request,sizeof(request),"g2-a-pass-%02u",i);
        assert(n>0 && (size_t)n<sizeof(request));
        seq=commit_game(store,&rooms,&match,&next,101u,1u,seq,request,
                        (uint8_t)(i+0x64u));
        match=next;
    }
    assert(match.phase==MYGAMES_DDZ_MATCH_FINISHED &&
           match.round.winner_seat==1u && match.round.remaining[1]==0u);
    expect_store(mygames_room_store_game_result(store,"room-1",9u,&result),
                 MYGAMES_STORE_OK);
    assert(result.finish_sequence==seq && result.game_id==9u &&
           result.players[0]==101u && result.players[1]==202u &&
           result.players[2]==303u);
    assert(result.score.bid_base==3u &&
           result.score.spring==MYGAMES_DDZ_SPRING_LANDLORD &&
           result.score.delta[0]==-6 && result.score.delta[1]==12 &&
           result.score.delta[2]==-6);
    /* Two different landlords truly FINISHED, no duplicate terminal
     * credit, and every seat's total is derived from exactly two rows. */
    assert_total(store,101u,6,2);
    assert_total(store,202u,6,2);
    assert_total(store,303u,-12,2);
    assert_total(store,404u,0,0);
    expect_store(mygames_room_store_game_result(store,"room-1",8u,&result),
                 MYGAMES_STORE_OK);
    assert(result.finish_sequence==final_seq &&
           result.score.delta[0]==12 && result.score.delta[1]==-6 &&
           result.score.delta[2]==-6);

    mygames_room_store_close(store);
    store=NULL;
    expect_store(mygames_room_store_open(argv[1],argv[2],&store),MYGAMES_STORE_OK);
    expect_store(mygames_room_store_load_game(store,"room-1",
                 &restarted,&loaded_game_seq),MYGAMES_STORE_OK);
    assert(restarted.game_id==9u && loaded_game_seq==seq &&
           restarted.phase==MYGAMES_DDZ_MATCH_FINISHED &&
           restarted.round.winner_seat==1u);
    expect_store(mygames_room_store_game_result(store,"room-1",8u,&result),
                 MYGAMES_STORE_OK);
    assert(result.finish_sequence==final_seq);
    assert_total(store,101u,6,2);
    assert_total(store,202u,6,2);
    assert_total(store,303u,-12,2);
    expect_store(mygames_room_store_game_result(store,"room-1",9u,&result),
                 MYGAMES_STORE_OK);
    assert(result.finish_sequence==seq &&
           result.score.delta[1]==12);
    expect_store(mygames_room_store_claim_epoch(store,1u,&claimed_epoch),
                 MYGAMES_STORE_OK);
    assert(claimed_epoch==2u);
    assert_total(store,101u,6,2);
    assert_total(store,202u,6,2);
    assert_total(store,303u,-12,2);
    expect_store(mygames_room_store_game_result(store,"room-1",8u,&result),
                 MYGAMES_STORE_OK);
    assert(result.finish_sequence==final_seq);
    mygames_room_store_close(store);
    puts("mygames.game_results: PASS");
    return 0;
}
