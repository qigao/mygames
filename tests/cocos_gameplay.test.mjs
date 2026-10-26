import {after, test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync, writeFileSync, rmSync} from 'node:fs';
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
  writeFileSync(join(dir,name+'.js'),emitted.outputText,'utf8');
}
const require=createRequire(import.meta.url);
const {
  parseServerFrame,encodeGameCommand,cardMaskFromIds,cardsFromMask,
  canonicalU64,cardMaskHex
}=require(join(dir,'Protocol.js'));
const {GameStore}=require(join(dir,'GameStore.js'));
const {GameSocket}=require(join(dir,'GameSocket.js'));

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
    {...privateHand('x'),room_epoch:epoch},
    {...privateHand('x'),cards_hex:'ffffffffffffff'},
    {v:1,type:'game.hand',request_id:'a',room_id:'room-1',
      game_id:gameId,cards_hex:'00000000000001',player_id:'303'},
    {v:1,type:'game.evil',request_id:'a'}
  ];
  for(const frame of invalid)
    assert.throws(()=>parseServerFrame(JSON.stringify(frame)),Error,JSON.stringify(frame));
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
