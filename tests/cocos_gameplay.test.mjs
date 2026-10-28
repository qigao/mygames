import {after, test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createRequire} from 'node:module';
import ts from 'typescript';

const dir=mkdtempSync(join(tmpdir(),'mygames-cocos-game-'));
for(const [name,source] of [
  ['Protocol','net/Protocol.ts'],
  ['GameSocket','net/GameSocket.ts'],
  ['GameStore','state/GameStore.ts']
]){
  const content=readFileSync(new URL('../apps/cocos/assets/scripts/'+source,import.meta.url),'utf8');
  const emitted=ts.transpileModule(content,{
    fileName:source,
    reportDiagnostics:true,
    compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.CommonJS,
                     strict:true,experimentalDecorators:true}
  });
  const errors=(emitted.diagnostics??[]).filter(d=>d.category===ts.DiagnosticCategory.Error);
  assert.deepEqual(errors.map(d=>ts.flattenDiagnosticMessageText(d.messageText,'\n')),[]);
  const output = join(dir,source.replace(/\.ts$/,'.js'));
  mkdirSync(join(dir,source.split('/')[0]),{recursive:true});
  writeFileSync(output,emitted.outputText,'utf8');
}
const require=createRequire(import.meta.url);
const {
  parseServerFrame,encodeGameCommand,cardMaskFromIds,cardsFromMask,
  canonicalU64,cardMaskHex
}=require(join(dir,'net','Protocol.js'));
const {GameStore}=require(join(dir,'state','GameStore.js'));
const {GameSocket}=require(join(dir,'net','GameSocket.js'));

const originalWebSocket=globalThis.WebSocket;
after(()=>{globalThis.WebSocket=originalWebSocket;rmSync(dir,{recursive:true,force:true});});

const gameId='9007199254740993';
const epoch='18446744073709551615';
const base={v:1,room_id:'room-1',room_epoch:epoch,game_id:gameId};
const startAck={...base,type:'game.ack',request_id:'g-start-1',action:'start',
                phase:1,turn_id:'1',seq:'18446744073709551600'};
const bidding=(step='1')=>({...base,type:'game.snapshot',
  request_id:'auto-sync',phase:'bidding',bid_turn_id:step,
  bid_turn_seat:0,highest_bid:0,highest_seat:3});
const playing=(turn='1',count=20)=>({...base,type:'game.snapshot',request_id:'broadcaster',
  phase:'playing',highest_bid:3,landlord_seat:0,
  revealed_kitty_hex:'00700000000000',turn_id:turn,turn_seat:0,
  last_cards_hex:'00000000000000',last_seat:3,
  remaining:[count,17,17],winner_seat:3
});
const springScore={base:3,bombs:0,spring:'landlord',multiplier:2,
                   unit:6,delta:[12,-6,-6]};
const historicalResult={
  v:1,type:'game.result',request_id:'result-query',room_id:'room-1',
  game_id:gameId,finish_seq:'18446744073709551609',
  players:['101','202','303'],landlord_seat:0,winner_seat:0,
  score:springScore,
};
const historicalStandings={
  v:1,type:'game.standings',request_id:'standings-query',
  room_id:'room-1',room_epoch:epoch,
  checkpoint_seq:'18446744073709551610',
  players:[
    {player_id:'101',points:'12',games:'1'},
    {player_id:'202',points:'-6',games:'1'},
    {player_id:'303',points:'-6',games:'1'}
  ]
};
const privateHand=(id,game=gameId,mask='00000000000003')=>({
  v:1,type:'game.hand',request_id:id,room_id:'room-1',
  game_id:game,cards_hex:mask
});

test('physical card identity, BigInt mask and exact C11 game command fields',()=>{
  assert.equal(cardMaskFromIds([0,1,53]),'20000000000003');
  assert.deepEqual(cardsFromMask('20000000000003'),[53,1,0]);
  assert.deepEqual(cardsFromMask('00000000000000'),[]);
  assert.throws(()=>cardMaskFromIds([]),/Select/);
  assert.throws(()=>cardMaskFromIds([0,0]),/Duplicate/);
  assert.throws(()=>cardMaskFromIds([54]),/Invalid/);
  assert.throws(()=>cardMaskFromIds([1.5]),/Invalid/);
  assert.equal(canonicalU64('18446744073709551615'),true);
  assert.equal(canonicalU64('18446744073709551616'),false);
  assert.equal(canonicalU64('9007199254740993'),true);
  for(const wrong of ['0','01','+1','1.0','-1',1])assert.equal(canonicalU64(wrong),false);
  assert.equal(cardMaskHex('3fffffffffffff'),true);
  assert.equal(cardMaskHex('4fffffffffffff'),false);
  assert.equal(cardMaskHex('3FFFFFFFFFFFFF'),false);
  assert.deepEqual(JSON.parse(encodeGameCommand('game.bid','bid-1',{
    game_id:gameId,turn_id:epoch,bid:3
  })),{v:1,type:'game.bid',request_id:'bid-1',
       game_id:gameId,turn_id:epoch,bid:3});
  assert.deepEqual(JSON.parse(encodeGameCommand('game.play','play-1',{
    game_id:gameId,turn_id:'1',cards_hex:'00000000000001'
  })).cards_hex,'00000000000001');
  assert.throws(()=>encodeGameCommand('game.start','new',{game_id:'8'}),/server-minted/);
  assert.throws(()=>encodeGameCommand('game.bid','bid-2',{game_id:'1',turn_id:'1',bid:4}),/bid/);
  assert.throws(()=>encodeGameCommand('game.play','bad',{
    game_id:'1',turn_id:'1',cards_hex:'40000000000000'
  }),/move/);
  assert.throws(()=>encodeGameCommand('game.pass','bad',{
    game_id:'1',turn_id:'1',cards_hex:'00000000000001'
  }),/pass/);
  assert.deepEqual(JSON.parse(encodeGameCommand('game.hand.get','hand-1',{
    game_id:gameId
  })).game_id,gameId);
});

test('public game frames reject secret/extra fields and imprecise identifiers',()=>{
  assert.deepEqual(parseServerFrame(JSON.stringify(startAck)),startAck);
  assert.equal(parseServerFrame(JSON.stringify(bidding())).phase,'bidding');
  assert.equal(parseServerFrame(JSON.stringify(playing())).turn_id,'1');
  const terminal={...playing('59',0),phase:'finished',
                  turn_seat:3,last_seat:0,winner_seat:0,score:springScore};
  assert.equal(parseServerFrame(JSON.stringify(terminal)).score.unit,6);
  assert.equal(parseServerFrame(JSON.stringify(privateHand('x'))).cards_hex,'00000000000003');
  assert.equal(parseServerFrame(JSON.stringify({
    v:1,type:'game.error',request_id:'err-1',code:'stale_generation_or_turn'
  })).type,'game.error');
  const invalid=[
    {...startAck,game_id:9007199254740992},
    {...startAck,seq:'18446744073709551616'},
    {...startAck,private_hand:'00000000000003'},
    {...bidding(),secret_deck:'caller should never receive'},
    {...bidding(),highest_seat:2},
    {...playing(),remaining:[17,17]},
    {...playing(),revealed_kitty_hex:'00000000000001'},
    {...playing(),last_cards_hex:'40000000000000'},
    {...playing(),turn_id:'01'},
    {...playing(),phase:'finished',turn_seat:0,winner_seat:0},
    {...terminal,score:{...springScore,delta:[13,-6,-6]}},
    {...terminal,score:{...springScore,multiplier:4}},
    {...terminal,score:{...springScore,spring:'farmers'}},
    {...terminal,score:{...springScore,unit:100000}},
    {...terminal,secret_hand:'private'},
    {...playing(),score:springScore},
    {...privateHand('x'),room_epoch:epoch},
    {...privateHand('x'),cards_hex:'ffffffffffffff'},
    {v:1,type:'game.hand',request_id:'a',room_id:'room-1',
      game_id:gameId,cards_hex:'00000000000001',player_id:'303'},
    {v:1,type:'game.evil',request_id:'a'}
  ];
  for(const frame of invalid)
    assert.throws(()=>parseServerFrame(JSON.stringify(frame)),Error,JSON.stringify(frame));
});

test('historical results/standings are exact, signed, member-only and zero-sum',()=>{
  assert.deepEqual(parseServerFrame(JSON.stringify(historicalResult)),historicalResult);
  assert.deepEqual(parseServerFrame(JSON.stringify(historicalStandings)),historicalStandings);
  assert.deepEqual(JSON.parse(encodeGameCommand('game.result.get','x',{
      game_id:gameId
  })),{v:1,type:'game.result.get',request_id:'x',game_id:gameId});
  assert.deepEqual(JSON.parse(encodeGameCommand('game.standings.get','y')),
      {v:1,type:'game.standings.get',request_id:'y'});
  assert.throws(()=>encodeGameCommand('game.standings.get','x',{
      game_id:gameId
  }),/server-owned/);
  for(const tampered of [
    {...historicalResult,players:['101','101','303']},
    {...historicalResult,players:['101','202','303'],secret_deck:'PRIVATE'},
    {...historicalResult,score:{...springScore,delta:[13,-6,-6]}},
    {...historicalResult,landlord_seat:3},
    {...historicalResult,finish_seq:'18446744073709551616'},
    {...historicalStandings,players:[
      {player_id:'101',points:'12',games:'1'},
      {player_id:'202',points:'-6',games:'1'},
      {player_id:'303',points:'-7',games:'1'}
    ]},
    {...historicalStandings,players:[
      {player_id:'101',points:'12',games:'1'},
      {player_id:'202',points:'-6',games:'0'},
      {player_id:'303',points:'-6',games:'1'}
    ]},
    {...historicalStandings,players:[
      {player_id:'101',points:'12',games:'1'},
      {player_id:'202',points:'-6',games:'1'},
      {player_id:'101',points:'-6',games:'1'}
    ]},
    {...historicalStandings,players:[
      {player_id:'101',points:'+12',games:'1'},
      {player_id:'202',points:'-6',games:'1'},
      {player_id:'303',points:'-6',games:'1'}
    ]},
    {...historicalStandings,secret_hand:'forbidden'},
    {...historicalStandings,checkpoint_seq:'00'}
  ])assert.throws(()=>parseServerFrame(JSON.stringify(tampered)),Error,
                    'historical result must reject corrupted/unauthorized content');
});

test('room history accepts ONLY current authenticated seat and exact request',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  store.noteAck(startAck);
  assert.equal(store.applySnapshot(playing('2',19)),true);
  store.expectStandings('standings-query');
  assert.equal(store.applyStandings({...historicalStandings,request_id:'stale'}),false);
  assert.equal(store.applyStandings({...historicalStandings,room_id:'room-2'}),false);
  assert.equal(store.applyStandings(historicalStandings),true);
  assert.equal(store.standings.players[0].points,'12');
  store.expectHistory('result-query',gameId);
  assert.equal(store.applyHistoricalResult({...historicalResult,request_id:'old'}),false);
  assert.equal(store.applyHistoricalResult({...historicalResult,players:['999','202','303']}),false);
  assert.equal(store.applyHistoricalResult(historicalResult),true);
  assert.equal(store.historicalResult.finish_seq,historicalResult.finish_seq);
  store.suspend();
  assert.equal(store.standings,null);
  assert.equal(store.historicalResult,null);
  store.expectStandings('standings-recovered');
  assert.equal(store.applyStandings({
    ...historicalStandings,request_id:'standings-recovered',
    checkpoint_seq:'99'
  }),false,'standings preceding last durable Game ACK must not regress');
  store.bindRoom('room-2','202',0);
  store.expectHistory('result-query',gameId);
  assert.equal(store.applyHistoricalResult(historicalResult),false);
  assert.equal(store.historicalResult,null);
});

test('history cannot bind an unseated client or survive seat reassignment',()=>{
  const unseated=new GameStore();
  unseated.bindRoom('room-1','101',null);
  unseated.expectStandings('standings-query');
  assert.equal(unseated.applyStandings(historicalStandings),false,
               'unseated clients must not accept room totals');
  unseated.expectHistory('result-query',gameId);
  assert.equal(unseated.applyHistoricalResult(historicalResult),false,
               'unseated clients must not accept historical results');

  const seated=new GameStore();
  seated.bindRoom('room-1','101',0);
  assert.equal(seated.noteAck(startAck),true);
  assert.equal(seated.applySnapshot(playing('2',19)),true);
  seated.expectStandings('standings-query');
  assert.equal(seated.applyStandings(historicalStandings),true);
  seated.expectHistory('result-query',gameId);
  assert.equal(seated.applyHistoricalResult(historicalResult),true);

  // An in-room seat transition is a new authority binding even when the
  // room and principal IDs remain unchanged.
  seated.bindRoom('room-1','101',1);
  assert.equal(seated.localSeat,1);
  assert.equal(seated.standings,null);
  assert.equal(seated.historicalResult,null);
  assert.equal(seated.snapshot,null);
  assert.equal(seated.lastAck,null);
  assert.equal(seated.gameId,null);
  seated.expectStandings('standings-query');
  assert.equal(seated.applyStandings(historicalStandings),false);
  seated.expectHistory('result-query',gameId);
  assert.equal(seated.applyHistoricalResult(historicalResult),false);
});

test('game UI ignores stale snapshots, stale hand responses and foreign room context',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  assert.equal(store.applySnapshot(bidding()),true);
  assert.equal(store.applySnapshot(bidding()),false);
  assert.equal(store.applySnapshot({...bidding('2'),room_id:'room-2'}),false);
  assert.equal(store.applySnapshot({...bidding(),room_epoch:'1'}),false);
  const playingNow=playing();
  assert.equal(store.applySnapshot(playingNow),true);
  assert.equal(store.applySnapshot(bidding('3')),false);
  assert.equal(store.isMyTurn,false); // private hand not yet confirmed
  store.expectHand('hand-new',gameId);
  assert.equal(store.applyPrivateHand(privateHand('hand-old')),false);
  assert.equal(store.applyPrivateHand(privateHand('hand-new', '9')),false);
  assert.equal(store.applyPrivateHand(privateHand('hand-new')),true);
  assert.equal(store.isMyTurn,true);
  assert.deepEqual(store.hand,[1,0]);
  assert.equal(store.toggle(0),true);
  assert.equal(store.toggle(1),true);
  assert.equal(store.selectedMask(),'00000000000003');
  assert.equal(store.toggle(53),false);
  assert.equal(store.selected.length,2);
  assert.equal(store.applySnapshot({...playing('2',19),turn_seat:1,
    last_cards_hex:'00000000000001',last_seat:0}),true);
  assert.deepEqual(store.hand,[]);
  assert.equal(store.isMyTurn,false);
  assert.equal(store.selected.length,0);
  assert.equal(store.applySnapshot(playing()),false);
  store.suspend();
  assert.equal(store.hydrated,false);
  assert.equal(store.applySnapshot({...playing('2',19),turn_seat:1,
    last_cards_hex:'00000000000001',last_seat:0,room_epoch:epoch}),true);
  store.bindRoom('room-2','202',1);
  assert.equal(store.snapshot,null);
  assert.equal(store.applySnapshot(playing()),false);
});

test('private hand can be displayed only for an EXACT bound request, not an old game',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  store.noteAck(startAck);
  assert.equal(store.gameId,gameId);
  assert.equal(store.applySnapshot({...playing(),game_id:'1'}),false);
  assert.equal(store.applySnapshot(playing()),true);
  store.expectHand('fresh-1',gameId);
  assert.equal(store.applyPrivateHand(privateHand('stale-1')),false);
  assert.equal(store.applyPrivateHand(privateHand('fresh-1')),true);
  assert.equal(store.hand.length,2);
  store.suspend();
  assert.equal(store.hand.length,0);
  assert.equal(store.applyPrivateHand(privateHand('fresh-1')),false);
  const old=store.lastAck;
  assert.equal(store.noteAck({...startAck,seq:'99'}),false);
  assert.equal(store.lastAck,old);
});


test('confirmed end-game ACK fences out-of-order turns and authentic rematch',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  const laterAck={...startAck,request_id:'play-late',action:'play',
                  phase:3,turn_id:'50',seq:'18446744073709551605'};
  assert.equal(store.noteAck(laterAck),true);
  assert.equal(store.applySnapshot(playing('49',3)),false,
               'An older public play must never overwrite confirmed durable turn');
  assert.equal(store.applySnapshot(playing('50',2)),true);
  store.expectHand('my-fresh-hand',gameId);
  assert.equal(store.applyPrivateHand(privateHand('my-fresh-hand')),true);
  assert.equal(store.isMyTurn,true);
  assert.equal(store.toggle(0),true);
  const finishedAck={...laterAck,request_id:'play-victory',phase:4,
                     turn_id:'59',seq:'18446744073709551607'};
  assert.equal(store.noteAck(finishedAck),true);
  assert.equal(store.applySnapshot(playing('58',1)),false);
  const finish={...playing('59',0),phase:'finished',turn_seat:3,
                last_seat:0,last_cards_hex:'00000000000002',
                winner_seat:0,score:springScore};
  assert.equal(store.applySnapshot(finish),true);
  assert.equal(store.isMyTurn,false);
  assert.equal(store.selected.length,0);
  assert.equal(store.snapshot.phase,'finished');
  assert.equal(store.applySnapshot(playing('60',0)),false,
               'A completed match must never revert into PLAYING');
  store.suspend();
  assert.equal(store.applySnapshot({...finish,turn_id:'58'}),false,
               'Reconnect cannot roll public view backwards');
  assert.equal(store.applySnapshot(finish),true,
               'Verified reconnect may rehydrate equal current turn');
  const nextGame='9007199254740995';
  assert.equal(store.noteAck({...startAck,request_id:'rematch',seq:'18446744073709551608',
                              game_id:nextGame,room_epoch:epoch}),true);
  const newBidding={...bidding(),game_id:nextGame,bid_turn_id:'1'};
  assert.equal(store.applySnapshot(newBidding),true);
  assert.equal(store.snapshot.phase,'bidding');
  assert.equal(store.hand.length,0,'old 0-card endgame hand cannot leak to rematch');
  assert.equal(store.applySnapshot(finish),false,'old game never resurfaces');
});

class FakeWebSocket {
  static OPEN=1;
  static instances=[];
  constructor(url){
    assert.match(url,/^ws:\/\/127\.0\.0\.1/);
    this.readyState=0;this.sent=[];
    FakeWebSocket.instances.push(this);
  }
  open(){this.readyState=1;this.onopen?.();}
  send(raw){assert.equal(this.readyState,1);this.sent.push(JSON.parse(raw));}
  receive(frame){this.onmessage?.({data:JSON.stringify(frame)});}
  disconnect(){this.readyState=3;this.onclose?.();}
  close(){this.readyState=3;}
}
const token=(ch)=>ch.repeat(64);
const hello=(id,ticket)=>({v:1,type:'server.hello',
  capabilities:['ping','rooms','resume','games'],
  player_id:String(id),resume_token:ticket});

test('only original game request_id is replayed after bearer recovery',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const messages=[],client=new GameSocket(()=>{},f=>messages.push(f));
  client.connect('ws://127.0.0.1:1780/ws');
  let ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(101,token('a')));
  assert.equal(client.canPlayGames,true);
  const id=client.startGame();
  assert.match(id,/^g-[a-f0-9]{16}-[0-9a-z]+$/);
  const original=ws.sent.at(-1);
  assert.equal(original.type,'game.start');
  assert.equal(original.request_id,id);
  assert.equal(client.pendingGameCount,1);
  ws.disconnect();
  assert.equal(client.pendingGameCount,1);
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(404,token('b')));
  assert.equal(client.state,'restoring');
  assert.equal(ws.sent.length,1);
  ws.receive({v:1,type:'session.ready',player_id:'101',generation:2,
    resume_token:token('c'),resumed:true});
  assert.equal(client.canPlayGames,true);
  assert.deepEqual(ws.sent.at(-1),original);
  ws.receive({...startAck,request_id:id});
  assert.equal(client.pendingGameCount,0);
  assert.equal(messages.at(-1).type,'game.ack');
  client.disconnect();
  assert.equal(client.canResume,false);
  globalThis.WebSocket=originalWebSocket;
});

test('failed session recovery never replays old player game commands',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const client=new GameSocket(()=>{},()=>{});
  client.connect('ws://127.0.0.1:1780/ws');
  let ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(101,token('a')));
  client.startGame();
  assert.equal(client.pendingGameCount,1);
  ws.disconnect();
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(202,token('b')));
  ws.receive({v:1,type:'session.error',code:'resume_rejected'});
  assert.equal(client.pendingGameCount,0);
  assert.equal(client.currentPlayerId,'202');
  assert.equal(ws.sent.length,1); // only the attempted resume token
  client.disconnect();
  globalThis.WebSocket=originalWebSocket;
});

test('GameSocket sends read-only room history requests without identity aliases',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const messages=[];
  const client=new GameSocket(()=>{},frame=>messages.push(frame));
  client.connect('ws://127.0.0.1:1780/ws');
  const ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(101,token('a')));
  const historyId=client.requestGameResult(gameId);
  const resultCommand=ws.sent.at(-1);
  assert.deepEqual(resultCommand,{
    v:1,type:'game.result.get',request_id:historyId,game_id:gameId
  });
  const standingsId=client.requestGameStandings();
  assert.deepEqual(ws.sent.at(-1),{
    v:1,type:'game.standings.get',request_id:standingsId
  });
  assert.equal(client.pendingGameCount,0,'read-only queries never journal a mutation');
  ws.receive({...historicalResult,request_id:historyId});
  ws.receive({...historicalStandings,request_id:standingsId});
  assert.equal(messages.at(-2).type,'game.result');
  assert.equal(messages.at(-1).type,'game.standings');
  client.disconnect();
  globalThis.WebSocket=originalWebSocket;
});

test('unknown COMMIT keeps the original game request, never guesses success',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const client=new GameSocket(()=>{},()=>{});
  client.connect('ws://127.0.0.1:1780/ws');
  let ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(101,token('a')));
  const id=client.startGame();
  const saved=ws.sent.at(-1);
  ws.receive({v:1,type:'game.error',request_id:id,code:'storage_unavailable'});
  assert.equal(client.pendingGameCount,1,
               'An uncertain TurboDB COMMIT cannot retire its client receipt');
  ws.disconnect();
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();ws.receive(hello(404,token('b')));
  ws.receive({v:1,type:'session.ready',player_id:'101',
              generation:2,resume_token:token('c'),resumed:true});
  assert.deepEqual(ws.sent.at(-1),saved);
  assert.equal(client.pendingGameCount,1);
  ws.receive({...startAck,request_id:id});
  assert.equal(client.pendingGameCount,0);
  client.disconnect();
  globalThis.WebSocket=originalWebSocket;
});

test('M5h two finished games, landlord rotation and rematch recovery never display stale settlement',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  // Game one ends with seat 0 landlord spring: this is the visible public
  // settlement, never a source for locally awarding or storing credits.
  const first={...playing('59',0),phase:'finished',turn_seat:3,
    last_seat:0,winner_seat:0,score:springScore};
  assert.equal(store.applySnapshot(first),true);
  assert.equal(store.snapshot.score.delta[0],12);
  assert.equal(store.isMyTurn,false);
  const firstTerminalAck={...startAck,action:'play',request_id:'first-finish',
    phase:4,turn_id:'59',seq:'18446744073709551607'};
  assert.equal(store.noteAck(firstTerminalAck),true);

  // Same room, new server-generated game ID and newer committed request.
  // Old points, old card selection and old terminal display must disappear.
  const gameTwo='9007199254740995';
  const newStart={...startAck,request_id:'second-start',
    game_id:gameTwo,seq:'18446744073709551608'};
  assert.equal(store.noteAck(newStart),true);
  const secondBid={...bidding(),game_id:gameTwo};
  assert.equal(store.applySnapshot(secondBid),true);
  assert.equal(store.snapshot.phase,'bidding');
  assert.equal('score' in store.snapshot,false);
  assert.equal(store.applySnapshot(first),false);
  assert.deepEqual(store.hand,[]);
  assert.equal(store.isMyTurn,false);

  // Seat 2 becomes landlord; the local seat 0 is now a farmer.
  const secondPlay={...playing('1',17),game_id:gameTwo,
    landlord_seat:2,turn_seat:0,remaining:[17,17,20]};
  assert.equal(store.applySnapshot(secondPlay),true);
  assert.equal('score' in store.snapshot,false);
  store.expectHand('second-private',gameTwo);
  assert.equal(store.applyPrivateHand(privateHand('second-private',gameId)),false,
    'First-game private response cannot repopulate a rematch hand');
  assert.equal(store.applyPrivateHand(privateHand('second-private',gameTwo)),true);
  assert.equal(store.toggle(0),true);
  assert.equal(store.selected.length,1);

  const secondScore={base:3,bombs:0,spring:'landlord',
    multiplier:2,unit:6,delta:[-6,-6,12]};
  const secondFinalAck={...startAck,game_id:gameTwo,action:'play',
    request_id:'second-finish',phase:4,turn_id:'59',
    seq:'18446744073709551614'};
  assert.equal(store.noteAck(secondFinalAck),true);
  const secondFinished={...secondPlay,phase:'finished',turn_id:'59',
    turn_seat:3,winner_seat:2,remaining:[17,17,0],score:secondScore};
  assert.equal(store.applySnapshot(secondFinished),true);
  assert.equal(store.snapshot.score.delta[0],-6);
  assert.equal(store.snapshot.score.delta[2],12);
  assert.equal(store.isMyTurn,false);
  assert.deepEqual(store.selected,[]);
  assert.equal(store.applySnapshot(first),false,
    'Delayed first-game terminal push may not replace second-game result');
  assert.equal(store.applySnapshot(secondPlay),false,
    'Delayed second-game PLAYING push may not undo terminal settlement');

  store.suspend();
  assert.deepEqual(store.hand,[]);
  assert.equal(store.hydrated,false);
  assert.equal(store.applySnapshot(secondFinished),true,
    'Cold recovery may rehydrate equal terminal game/turn');
  assert.equal(store.snapshot.score.delta[0],-6);
  assert.equal(store.applySnapshot(first),false);
  assert.equal(store.applyPrivateHand(privateHand('second-private',gameTwo)),false,
    'Unsolicited hand after recovery cannot bypass exact request binding');
});

test('M5i verified reconnect keeps unique new game request IDs and exact old replay',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const client=new GameSocket(()=>{},()=>{});
  client.connect('ws://127.0.0.1:1780/ws');
  let ws=FakeWebSocket.instances.at(-1);
  ws.open();
  ws.receive(hello(101,token('a')));
  const firstId=client.startGame();
  const firstWire=ws.sent.at(-1);
  ws.receive({...startAck,request_id:firstId});
  assert.equal(client.pendingGameCount,0);
  // The command is physically sent and its outcome is not known yet.
  const pendingId=client.bid(gameId,'1',3);
  const pendingWire=ws.sent.at(-1);
  assert.notEqual(pendingId,firstId);
  ws.disconnect();
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();
  ws.receive(hello(404,token('b')));
  assert.equal(client.state,'restoring');
  assert.equal(ws.sent.length,1);
  ws.receive({v:1,type:'session.ready',player_id:'101',
    generation:2,resume_token:token('c'),resumed:true});
  assert.equal(client.state,'ready');
  assert.deepEqual(ws.sent[1],pendingWire,
    'Only the original unresolved command may be replayed verbatim');
  assert.equal(client.pendingGameCount,1);
  ws.receive({...startAck,request_id:pendingId,action:'bid',
    phase:3,turn_id:'2',seq:'18446744073709551601'});
  assert.equal(client.pendingGameCount,0);
  const newerId=client.play(gameId,'2','00000000000001');
  assert.ok(![firstId,pendingId].includes(newerId),
    'New command after resume must never collide with an old receipt');
  assert.notDeepEqual(ws.sent.at(-1),firstWire);
  const newerWire=ws.sent.at(-1);
  ws.disconnect();

  // Repeated cold transport reconnects must keep the same monotonic
  // session nonce/ordinal even when an unconfirmed new turn is pending.
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();
  ws.receive(hello(505,token('d')));
  ws.receive({v:1,type:'session.ready',player_id:'101',
    generation:3,resume_token:token('e'),resumed:true});
  assert.deepEqual(ws.sent[1],newerWire);
  assert.equal(client.pendingGameCount,1);
  ws.receive({...startAck,request_id:newerId,action:'play',
    phase:3,turn_id:'3',seq:'18446744073709551602'});
  const lastId=client.pass(gameId,'3');
  assert.equal(new Set([firstId,pendingId,newerId,lastId]).size,4);
  client.disconnect();
  globalThis.WebSocket=originalWebSocket;
});

test('M5i recovery cannot replay old-player game commands under a different identity',()=>{
  globalThis.WebSocket=FakeWebSocket;
  const client=new GameSocket(()=>{},()=>{});
  client.connect('ws://127.0.0.1:1780/ws');
  let ws=FakeWebSocket.instances.at(-1);
  ws.open();
  ws.receive(hello(101,token('a')));
  client.startGame();
  assert.equal(client.pendingGameCount,1);
  ws.disconnect();
  client.connect('ws://127.0.0.1:1780/ws');
  ws=FakeWebSocket.instances.at(-1);
  ws.open();
  ws.receive(hello(404,token('b')));
  assert.equal(client.state,'restoring');
  assert.equal(ws.sent.length,1);
  // Malformed recovery semantics, even though this frame passes shape
  // validation: player 202 is NOT the holder of player 101's commands.
  ws.receive({v:1,type:'session.ready',player_id:'202',
    generation:2,resume_token:token('c'),resumed:true});
  assert.equal(client.state,'failed');
  assert.equal(client.canResume,false);
  assert.equal(client.currentPlayerId,null);
  assert.equal(client.pendingGameCount,0);
  assert.equal(ws.sent.length,1,
    'Mismatched principal must NEVER see the pending request replay');
  assert.throws(()=>client.startGame(),/Not connected/);
  client.disconnect();
  globalThis.WebSocket=originalWebSocket;
});

test('M5i Owner epoch takeover cannot regress committed game, phase, turn or ACK',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  const firstFinished={...playing('59',0),room_epoch:'7',game_id:'11',
    phase:'finished',turn_seat:3,winner_seat:0,score:springScore};
  const finalAck={...startAck,room_epoch:'7',game_id:'11',
    phase:4,turn_id:'59',seq:'100',request_id:'g1-terminal',action:'play'};
  assert.equal(store.noteAck(finalAck),true);
  assert.equal(store.noteAck(finalAck),false,
    'Idempotent receipt replay must not reschedule a UI transition');
  assert.equal(store.applySnapshot(firstFinished),true);
  assert.equal(store.snapshot.phase,'finished');

  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'8',
    phase:'playing',turn_id:'58'}),false,
    'A new Owner epoch must not restore pre-terminal gameplay');
  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'8',
    game_id:'10'}),false,
    'Even a higher epoch cannot resurrect an older game');
  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'8',
    turn_id:'58'}),false,
    'Higher epoch must not roll a terminal turn backwards');

  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'8'}),true,
    'New Owner may rehydrate an equal confirmed terminal game');
  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'8'}),false,
    'Same-epoch duplicate public snapshot is not another update');
  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'9',
    phase:'playing'}),false,
    'A later epoch cannot downgrade FINISHED to PLAYING');

  const nextGame={...bidding('1'),room_epoch:'9',game_id:'12'};
  assert.equal(store.applySnapshot(nextGame),true);
  assert.equal(store.snapshot.phase,'bidding');
  assert.equal('score' in store.snapshot,false);
  assert.equal(store.noteAck({...finalAck,room_epoch:'10',seq:'101'}),false,
    'A higher-epoch replay of the previous game cannot replace new game');
  assert.equal(store.noteAck({...startAck,room_epoch:'9',game_id:'12',
    phase:1,turn_id:'1',seq:'101',request_id:'new-game'}),true);
  assert.equal(store.lastAck.game_id,'12');
  assert.equal(store.applySnapshot({...firstFinished,room_epoch:'10'}),false);
});

test('M5i a confirmed rematch ACK refreshes the NEW game, not the stale finished snapshot',()=>{
  const store=new GameStore();
  store.bindRoom('room-1','101',0);
  const finished={...playing('59',0),phase:'finished',turn_seat:3,
    winner_seat:0,score:springScore};
  assert.equal(store.applySnapshot(finished),true);
  store.expectHand('finished-private',gameId);
  assert.equal(store.applyPrivateHand(privateHand('finished-private')),true);
  assert.equal(store.hand.length,2);

  const firstFinishAck={...startAck,action:'play',request_id:'first-finish',
    phase:4,turn_id:'59',seq:'18446744073709551605'};
  assert.equal(store.noteAck(firstFinishAck),true);
  const rematchGame='9007199254740995';
  const rematchAck={...startAck,request_id:'rematch-start',
    game_id:rematchGame,phase:1,turn_id:'1',
    seq:'18446744073709551606'};
  assert.equal(store.noteAck(rematchAck),true);
  assert.equal(store.gameId,rematchGame,
    'After the durable rematch Game ACK, the next snapshot request must target the new game');
  assert.equal(store.snapshot,null,
    'The old terminal winner and score cannot remain on the displayed game');
  assert.deepEqual(store.hand,[],
    'The old private hand cannot be displayed before the new game rehydrates');
  assert.equal(store.hydrated,false);
  assert.equal(store.isMyTurn,false);
  assert.equal(store.applySnapshot(finished),false,
    'A delayed terminal snapshot from game one cannot undo the confirmed rematch');

  globalThis.WebSocket=FakeWebSocket;
  const socket=new GameSocket(()=>{},()=>{});
  try {
    socket.connect('ws://127.0.0.1:1780/ws');
    const ws=FakeWebSocket.instances.at(-1);
    ws.open();
    ws.receive(hello(101,token('a')));
    assert.equal(socket.canPlayGames,true);
    const request=socket.requestGameSnapshot(store.gameId);
    assert.deepEqual(ws.sent.at(-1),{
      v:1,type:'game.snapshot.get',request_id:request,game_id:rematchGame
    },'The production client must send the new committed game ID');
  } finally {
    socket.disconnect();
    globalThis.WebSocket=originalWebSocket;
  }

  const newBidding={...bidding(),game_id:rematchGame,
    bid_turn_id:'1'};
  assert.equal(store.applySnapshot(newBidding),true);
  assert.equal(store.snapshot.phase,'bidding');
  assert.equal(store.gameId,rematchGame);
  assert.equal('score' in store.snapshot,false);
  store.expectHand('rematch-member-hand',rematchGame);
  assert.equal(store.applyPrivateHand(
    privateHand('rematch-member-hand',gameId)),false);
  assert.equal(store.applyPrivateHand(
    privateHand('rematch-member-hand',rematchGame)),true);
  assert.equal(store.applySnapshot(finished),false);

  // If an unsolicited newer-game public snapshot arrives before an ACK,
  // snapshot identity must also outrank the older ACK as the refresh target.
  const peer=new GameStore();
  peer.bindRoom('room-1','202',1);
  assert.equal(peer.noteAck(startAck),true);
  assert.equal(peer.applySnapshot(newBidding),true);
  assert.equal(peer.gameId,rematchGame);
});
