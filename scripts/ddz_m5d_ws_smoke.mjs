// Real installed CHttp WebSocket -> one CFlow Owner -> TurboDB WAL/FULL.
// Test-only loopback protocol. Never log or archive the secret member hand,
// original CSPRNG shuffle, or plaintext session credential.
import assert from 'node:assert/strict';
import {readFileSync, writeFileSync} from 'node:fs';

const BASE = 'ws://127.0.0.1:1780/ws';
const auditPath = process.env.MYGAMES_GAME_M5D_AUDIT;
const phase = process.argv[2] ?? 'first';
assert.ok(auditPath && (phase === 'first' || phase === 'restart'));

class Client {
  constructor() {
    this.ws = new WebSocket(BASE);
    this.messages = [];
    this.waiters = [];
    this.ws.addEventListener('message', e => {
      const m = JSON.parse(e.data);
      const i = this.waiters.findIndex(w => w.predicate(m));
      if (i === -1) this.messages.push(m);
      else {
        const [w] = this.waiters.splice(i, 1);
        clearTimeout(w.timer);
        w.resolve(m);
      }
    });
  }
  receive(predicate, ms=10000) {
    const found=this.messages.findIndex(predicate);
    if(found !== -1) return Promise.resolve(this.messages.splice(found,1)[0]);
    return new Promise((resolve,reject)=>{
      const w={predicate,resolve,reject,timer:null};
      w.timer=setTimeout(()=>{
        this.waiters=this.waiters.filter(e=>e!==w);
        reject(new Error('Timed out waiting for protocol response'));
      },ms);
      this.waiters.push(w);
    });
  }
  send(type, id, extra={}) {
    this.ws.send(JSON.stringify({v:1,type,request_id:id,...extra}));
  }
  close(){
    for(const w of this.waiters){clearTimeout(w.timer);w.reject(new Error('closed'));}
    this.waiters=[];
    this.ws.close();
  }
}
const clients=[];
async function connect(){
  const c = new Client();clients.push(c);
  const hello=await c.receive(m=>m.type==='server.hello');
  assert.equal(hello.v,1);
  assert.match(hello.player_id,/^[1-9][0-9]*$/);
  c.player=hello.player_id;
  c.ticket=hello.resume_token;
  return c;
}
const ack=id=>m=>m.type==='game.ack' && m.request_id===id;
const err=id=>m=>m.type==='game.error' && m.request_id===id;
const rack=id=>m=>m.type==='room.ack' && m.request_id===id;
const rerr=id=>m=>m.type==='room.error' && m.request_id===id;
const gameSnapshot=id=>m=>m.type==='game.snapshot' && m.request_id===id;
const hand=id=>m=>m.type==='game.hand' && m.request_id===id;
const cardCount=mask=>{let n=0;for(let k=BigInt('0x'+mask);k>0n;k&=k-1n)n++;return n;};
const lowBit=hex=>{const x=BigInt('0x'+hex);return (x & -x).toString(16).padStart(14,'0');};

async function initial() {
  const a=await connect(),b=await connect(),c=await connect();
  a.send('room.create','m5d-r-create');
  const created=await a.receive(rack('m5d-r-create'));
  const roomId=created.room_id;
  assert.match(roomId,/^room-[1-9][0-9]*$/);

  a.send('room.start','m5d-too-early');
  assert.equal((await a.receive(rerr('m5d-too-early'))).code,'not_ready');
  b.send('room.join','m5d-r-b',{room_id:roomId});
  assert.equal((await b.receive(rack('m5d-r-b'))).seat,1);
  c.send('room.join','m5d-r-c',{room_id:roomId});
  assert.equal((await c.receive(rack('m5d-r-c'))).seat,2);

  for(const [client,id] of [[a,'m5d-ready-a'],[b,'m5d-ready-b'],[c,'m5d-ready-c']]){
    client.send('room.ready',id,{ready:true});
    assert.equal((await client.receive(rack(id))).type,'room.ack');
  }
  b.send('room.start','m5d-not-owner');
  assert.equal((await b.receive(rerr('m5d-not-owner'))).code,'not_owner');
  a.send('room.start','m5d-room-start');
  const roomAck=await a.receive(rack('m5d-room-start'));
  assert.equal(roomAck.room_id,roomId);

  b.send('game.start','m5d-start-foreign');
  assert.equal((await b.receive(err('m5d-start-foreign'))).code,'not_authorized');
  a.send('game.start','m5d-game-start');
  const started=await a.receive(ack('m5d-game-start'));
  assert.equal(started.action,'start');
  const gid=String(started.game_id);
  assert.match(gid,/^[1-9][0-9]*$/);
  assert.ok(started.seq>roomAck.seq || started.seq>0);
  a.send('game.snapshot.get','m5d-snap-bidding',{game_id:gid});
  const before=await a.receive(gameSnapshot('m5d-snap-bidding'));
  assert.equal(before.phase,'bidding');
  assert.ok(!Object.hasOwn(before,'revealed_kitty_hex'));
  assert.ok(!Object.hasOwn(before,'cards_hex'));
  assert.ok(!Object.hasOwn(before,'secret_deck'));

  a.send('game.hand.get','m5d-hand-a',{game_id:gid});
  const beforeA=await a.receive(hand('m5d-hand-a'));
  b.send('game.hand.get','m5d-hand-b',{game_id:gid});
  const beforeB=await b.receive(hand('m5d-hand-b'));
  assert.match(beforeA.cards_hex,/^[0-3][0-9a-f]{13}$/);
  assert.equal(cardCount(beforeA.cards_hex),17);
  assert.equal(cardCount(beforeB.cards_hex),17);
  assert.equal(BigInt('0x'+beforeA.cards_hex)&BigInt('0x'+beforeB.cards_hex),0n);

  a.send('game.hand.get','m5d-injection',{game_id:gid,player_id:b.player});
  const malformed=await a.receive(m=>m.type==='server.error');
  assert.ok(['invalid_command','unsupported_command'].includes(malformed.code));

  a.send('game.bid','m5d-bid-three',{game_id:gid,turn_id:'1',bid:3});
  const bid=await a.receive(ack('m5d-bid-three'));
  assert.equal(bid.action,'bid');
  b.send('game.snapshot.get','m5d-snap-playing',{game_id:gid});
  const playing=await b.receive(gameSnapshot('m5d-snap-playing'));
  assert.equal(playing.phase,'playing');
  assert.match(playing.revealed_kitty_hex,/^[0-3][0-9a-f]{13}$/);
  assert.deepEqual(playing.remaining,[20,17,17]);
  assert.equal(playing.turn_seat,0);

  a.send('game.hand.get','m5d-hand-award',{game_id:gid});
  const awarded=await a.receive(hand('m5d-hand-award'));
  assert.equal(cardCount(awarded.cards_hex),20);
  assert.equal((BigInt('0x'+awarded.cards_hex)&BigInt('0x'+playing.revealed_kitty_hex)),
               BigInt('0x'+playing.revealed_kitty_hex));
  const first=lowBit(awarded.cards_hex);
  b.send('game.pass','m5d-pass-out-of-turn',{game_id:gid,turn_id:'1'});
  assert.equal((await b.receive(err('m5d-pass-out-of-turn'))).code,
               'stale_generation_or_turn');

  a.send('game.play','m5d-first-play',{game_id:gid,turn_id:'1',cards_hex:first});
  const play=await a.receive(ack('m5d-first-play'));
  assert.equal(play.action,'play');
  assert.ok(play.seq>bid.seq);
  b.send('game.snapshot.get','m5d-snap-after-play',{game_id:gid});
  const snap=await b.receive(gameSnapshot('m5d-snap-after-play'));
  assert.equal(snap.last_cards_hex,first);
  assert.equal(snap.turn_id,'2');
  assert.equal(snap.remaining[0],19);

  b.send('game.pass','m5d-b-pass',{game_id:gid,turn_id:'2'});
  const passB=await b.receive(ack('m5d-b-pass'));
  assert.equal(passB.action,'pass');
  c.send('game.pass','m5d-c-pass',{game_id:gid,turn_id:'3'});
  const passC=await c.receive(ack('m5d-c-pass'));
  assert.equal(passC.action,'pass');
  a.send('game.snapshot.get','m5d-snap-cleared',{game_id:gid});
  const cleared=await a.receive(gameSnapshot('m5d-snap-cleared'));
  assert.equal(cleared.turn_id,'4');
  assert.equal(cleared.last_cards_hex,'00000000000000');
  assert.equal(cleared.turn_seat,0);

  a.send('game.play','m5d-first-play',{game_id:gid,turn_id:'1',cards_hex:first});
  const replay=await a.receive(ack('m5d-first-play'));
  assert.deepEqual(replay,play);
  a.send('game.start','m5d-game-start');
  const startReplay=await a.receive(ack('m5d-game-start'));
  assert.deepEqual(startReplay,started);

  // Store only the minimum private recovery capability in a 0600 temp file.
  // No opponent's hand, physical shuffled deck or player card mask is logged.
  writeFileSync(auditPath,JSON.stringify({
    room_id:roomId,player_id:a.player,resume_token:a.ticket,
    game_id:gid,first_card_hex:first,
    first_play_ack:play,start_ack:started,last_seq:passC.seq
  }),{encoding:'utf8',mode:0o600});
}

async function restarted(){
  const audit=JSON.parse(readFileSync(auditPath,'utf8'));
  const resumed=await connect();
  resumed.ws.send(JSON.stringify({v:1,type:'session.resume',resume_token:audit.resume_token}));
  const ready=await resumed.receive(m=>m.type==='session.ready');
  assert.equal(ready.player_id,audit.player_id);
  assert.notEqual(ready.resume_token,audit.resume_token);
  assert.equal(ready.resumed,true);

  resumed.send('game.snapshot.get','m5d-after-restart',{game_id:audit.game_id});
  const state=await resumed.receive(gameSnapshot('m5d-after-restart'));
  assert.equal(state.phase,'playing');
  assert.equal(state.turn_id,'4');
  assert.equal(state.turn_seat,0);
  assert.equal(state.last_cards_hex,'00000000000000');
  assert.deepEqual(state.remaining,[19,17,17]);

  resumed.send('game.hand.get','m5d-hand-restored',{game_id:audit.game_id});
  const localHand=await resumed.receive(hand('m5d-hand-restored'));
  assert.equal(cardCount(localHand.cards_hex),19);
  assert.ok((BigInt('0x'+localHand.cards_hex)&BigInt('0x'+audit.first_card_hex))===0n);

  resumed.send('game.play','m5d-first-play',
              {game_id:audit.game_id,turn_id:'1',cards_hex:audit.first_card_hex});
  const replay=await resumed.receive(ack('m5d-first-play'));
  assert.deepEqual(replay,audit.first_play_ack);
  resumed.send('game.start','m5d-game-start');
  const oldStart=await resumed.receive(ack('m5d-game-start'));
  assert.deepEqual(oldStart,audit.start_ack);

  resumed.send('game.pass','m5d-invalid-lead',{game_id:audit.game_id,turn_id:'4'});
  assert.equal((await resumed.receive(err('m5d-invalid-lead'))).code,
               'stale_generation_or_turn');
  resumed.send('game.start','m5d-other-start');
  assert.equal((await resumed.receive(err('m5d-other-start'))).code,'wrong_phase');
  resumed.send('game.snapshot.get','m5d-durable-unchanged',{game_id:audit.game_id});
  const stable=await resumed.receive(gameSnapshot('m5d-durable-unchanged'));
  assert.equal(stable.turn_id,'4');
  // The original ticket has been retired. Another connection cannot
  // impersonate the reattached authenticated player.
  const attacker=await connect();
  attacker.ws.send(JSON.stringify({v:1,type:'session.resume',resume_token:audit.resume_token}));
  assert.equal((await attacker.receive(m=>m.type==='session.error')).code,'resume_rejected');
}

try {
  if(phase==='first')await initial();else await restarted();
  console.log('MyGames real CFlow WebSocket Game Owner '+phase+': PASS');
} finally {
  for(const c of clients)c.close();
}
