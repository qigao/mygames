// M5f REAL three-player full-length match, winner and rematch.
// CHttp WebSocket -> one CFlow Owner -> TurboDB WAL/FULL. No mock broker.
// Original user bearer capability appears only inside a 0600 temp file;
// never print hand/deck/tokens or archive this file as a CI artifact.
import assert from 'node:assert/strict';
import {readFileSync, writeFileSync} from 'node:fs';

const URL='ws://127.0.0.1:1780/ws';
const manifest=process.env.MYGAMES_M5F_PRIVATE_AUDIT;
const stage=process.argv[2] ?? 'first';
assert.ok(manifest && ['first','restart','second-restart'].includes(stage));

const connected=[];
class Client {
  constructor(){
    this.ws=new WebSocket(URL);
    this.queue=[];
    this.waiters=[];
    this.ws.addEventListener('message',e=>{
      const m=JSON.parse(e.data);
      const idx=this.waiters.findIndex(w=>w.match(m));
      if(idx<0)this.queue.push(m);
      else{
        const [w]=this.waiters.splice(idx,1);
        clearTimeout(w.timeout);
        w.resolve(m);
      }
    });
  }
  receive(match,timeoutMs=15000){
    const i=this.queue.findIndex(match);
    if(i>=0)return Promise.resolve(this.queue.splice(i,1)[0]);
    return new Promise((resolve,reject)=>{
      const w={match,resolve,reject,timeout:null};
      w.timeout=setTimeout(()=>{
        this.waiters=this.waiters.filter(x=>x!==w);
        reject(new Error('Actual CHttp Game Owner response timeout'));
      },timeoutMs);
      this.waiters.push(w);
    });
  }
  send(type,id,fields={}){
    this.ws.send(JSON.stringify({v:1,type,request_id:id,...fields}));
  }
  close(){
    for(const w of this.waiters){
      clearTimeout(w.timeout);
      w.reject(new Error('closed'));
    }
    this.waiters.length=0;
    this.ws.close();
  }
}
async function connect(){
  const client=new Client();connected.push(client);
  const hello=await client.receive(x=>x.type==='server.hello');
  assert.equal(hello.v,1);
  assert.ok(hello.capabilities.includes('games'));
  assert.match(hello.player_id,/^[1-9][0-9]*$/);
  client.player=hello.player_id;
  client.token=hello.resume_token;
  return client;
}
const ack=id=>x=>x.type==='game.ack' && x.request_id===id;
const roomAck=id=>x=>x.type==='room.ack' && x.request_id===id;
const error=id=>x=>x.type==='game.error' && x.request_id===id;
const pub=id=>x=>x.type==='game.snapshot' && x.request_id===id;
const priv=id=>x=>x.type==='game.hand' && x.request_id===id;
const count=x=>{let n=0;for(let k=BigInt('0x'+x);k>0n;k&=k-1n)n++;return n;};
const bits=x=>{let k=BigInt('0x'+x),cards=[];for(let i=0;i<54;i++)if(k&(1n<<BigInt(i)))cards.push(i);return cards;};
const oneCard=id=>(1n<<BigInt(id)).toString(16).padStart(14,'0');

function publicOnly(snapshot){
  assert.ok(!Object.hasOwn(snapshot,'cards_hex'));
  assert.ok(!Object.hasOwn(snapshot,'secret_deck'));
  assert.ok(!Object.hasOwn(snapshot,'hands'));
  assert.ok(!Object.hasOwn(snapshot,'resume_token'));
}
function privateFile(object){
  writeFileSync(manifest,JSON.stringify(object),{encoding:'utf8',mode:0o600});
}
async function memberResume(ticket,expectedPlayer,expectedGameId){
  const client=await connect();
  client.ws.send(JSON.stringify({v:1,type:'session.resume',resume_token:ticket}));
  const session=await client.receive(x=>x.type==='session.ready');
  assert.equal(session.resumed,true);
  assert.equal(session.player_id,expectedPlayer);
  assert.notEqual(session.resume_token,ticket);
  client.player=session.player_id;
  client.token=session.resume_token;
  const automatic=await client.receive(x=>x.type==='game.snapshot' &&
    x.request_id==='resume-public-game');
  publicOnly(automatic);
  assert.equal(automatic.game_id,expectedGameId);
  return {client,automatic};
}

async function first(){
  const a=await connect(),b=await connect(),c=await connect();
  a.send('room.create','m5f-room-created');
  const r=await a.receive(roomAck('m5f-room-created'));
  assert.match(r.room_id,/^room-[1-9][0-9]*$/);
  for(const [client,player,id] of [
    [b,b.player,'m5f-join-b'],[c,c.player,'m5f-join-c']
  ]){
    client.send('room.join',id,{room_id:r.room_id});
    const join=await client.receive(roomAck(id));
    assert.equal(join.room_id,r.room_id);
    assert.ok(player);
  }
  for(const [client,id] of [[a,'m5f-ready-a'],[b,'m5f-ready-b'],[c,'m5f-ready-c']]){
    client.send('room.ready',id,{ready:true});
    assert.equal((await client.receive(roomAck(id))).type,'room.ack');
  }
  a.send('room.start','m5f-room-start');
  await a.receive(roomAck('m5f-room-start'));
  a.send('game.start','m5f-first-game');
  const started=await a.receive(ack('m5f-first-game'));
  assert.equal(started.phase,1);
  const gameId=started.game_id;
  a.send('game.bid','m5f-landlord-bid',{game_id:gameId,turn_id:'1',bid:3});
  const bid=await a.receive(ack('m5f-landlord-bid'));
  assert.equal(bid.phase,3);
  a.send('game.hand.get','m5f-landlord-hand',{game_id:gameId});
  const cards=await a.receive(priv('m5f-landlord-hand'));
  assert.equal(count(cards.cards_hex),20);
  const landlordCards=bits(cards.cards_hex);
  assert.equal(landlordCards.length,20);

  // Real 20-card finished game, every action is a separate SQLite COMMIT.
  // Single-card lead is always legal, and both farmers pass in turn.
  let turn=BigInt(bid.turn_id);
  let sequence=BigInt(bid.seq);
  let finalAck=null;
  let firstPlayAck=null;
  let firstCard=null;
  for(let i=0;i<landlordCards.length;i++){
    const card=oneCard(landlordCards[i]);
    const id='m5f-play-'+i;
    a.send('game.play',id,{game_id:gameId,turn_id:String(turn),cards_hex:card});
    const played=await a.receive(ack(id));
    assert.equal(played.action,'play');
    assert.equal(BigInt(played.turn_id),turn+1n);
    assert.equal(BigInt(played.seq),sequence+1n);
    turn=BigInt(played.turn_id);
    sequence=BigInt(played.seq);
    if(i===0){firstCard=card;firstPlayAck=played;}
    if(i===19){
      assert.equal(played.phase,4,'Last physical card must end the round');
      finalAck=played;
    }else{
      assert.equal(played.phase,3);
      b.send('game.pass','m5f-b-pass-'+i,{game_id:gameId,turn_id:String(turn)});
      const passB=await b.receive(ack('m5f-b-pass-'+i));
      assert.equal(passB.action,'pass');
      assert.equal(BigInt(passB.turn_id),turn+1n);
      assert.equal(BigInt(passB.seq),sequence+1n);
      sequence=BigInt(passB.seq);turn=BigInt(passB.turn_id);
      c.send('game.pass','m5f-c-pass-'+i,{game_id:gameId,turn_id:String(turn)});
      const passC=await c.receive(ack('m5f-c-pass-'+i));
      assert.equal(passC.action,'pass');
      assert.equal(BigInt(passC.turn_id),turn+1n);
      assert.equal(BigInt(passC.seq),sequence+1n);
      sequence=BigInt(passC.seq);turn=BigInt(passC.turn_id);
    }
  }
  assert.ok(finalAck && firstCard && firstPlayAck);
  a.send('game.snapshot.get','m5f-finished',{game_id:gameId});
  const finished=await a.receive(pub('m5f-finished'));
  publicOnly(finished);
  assert.equal(finished.phase,'finished');
  assert.equal(finished.game_id,gameId);
  assert.equal(finished.winner_seat,0);
  assert.equal(finished.landlord_seat,0);
  assert.equal(finished.turn_seat,3);
  assert.equal(finished.turn_id,String(turn));
  assert.deepEqual(finished.remaining,[0,17,17]);
  assert.deepEqual(finished.score,{
    base:3,bombs:0,spring:'landlord',multiplier:2,unit:6,
    delta:[12,-6,-6]
  },'20 real single-card leads with no farmer play must be a landlord spring');

  a.send('game.hand.get','m5f-finished-landlord',{game_id:gameId});
  assert.equal((await a.receive(priv('m5f-finished-landlord'))).cards_hex,
               '00000000000000');
  b.send('game.hand.get','m5f-finished-farmer',{game_id:gameId});
  assert.equal(count((await b.receive(priv('m5f-finished-farmer'))).cards_hex),17);

  b.send('game.pass','m5f-reject-after-win',{game_id:gameId,turn_id:String(turn)});
  assert.equal((await b.receive(error('m5f-reject-after-win'))).code,
               'stale_generation_or_turn');
  a.send('game.play','m5f-play-19',{game_id:gameId,
                                  turn_id:String(turn-1n),
                                  cards_hex:oneCard(landlordCards[19])});
  assert.deepEqual(await a.receive(ack('m5f-play-19')),finalAck);
  a.send('game.snapshot.get','m5f-still-finished',{game_id:gameId});
  assert.equal((await a.receive(pub('m5f-still-finished'))).turn_id,String(turn));

  privateFile({
    room_id:r.room_id,game_id:gameId,started,firstPlayAck,
    firstCard,finalAck,winner_turn:String(turn),
    players:[a,b,c].map(client=>({player_id:client.player,token:client.token}))
  });
}

async function restarted(){
  const saved=JSON.parse(readFileSync(manifest,'utf8'));
  const members=[];
  for(let i=0;i<3;i++){
    const result=await memberResume(saved.players[i].token,
                                    saved.players[i].player_id,saved.game_id);
    members.push(result.client);
    assert.equal(result.automatic.phase,'finished');
    assert.equal(result.automatic.winner_seat,0);
    assert.equal(result.automatic.turn_id,saved.winner_turn);
    assert.deepEqual(result.automatic.score,{
      base:3,bombs:0,spring:'landlord',multiplier:2,unit:6,
      delta:[12,-6,-6]
    });
  }
  const [a,b,c]=members;
  a.send('game.play','m5f-play-19',{game_id:saved.game_id,
    turn_id:String(BigInt(saved.winner_turn)-1n),
    cards_hex:saved.finalAck.action==='play' ?
      // The card is not stored in public state; use a safe deterministic
      // retry of the known FIRST action instead. Never store full hands.
      saved.firstCard : '00000000000001'});
  // Wrong semantic body with an existing request ID must conflict, not
  // fabricate another committed turn. The previous winning ACK is checked
  // through the final game state and original FIRST receipt below.
  assert.equal((await a.receive(error('m5f-play-19'))).code,
               'stale_generation_or_turn');
  a.send('game.play','m5f-play-0',{game_id:saved.game_id,
                                  turn_id:'1',cards_hex:saved.firstCard});
  assert.deepEqual(await a.receive(ack('m5f-play-0')),saved.firstPlayAck);
  a.send('game.start','m5f-first-game');
  assert.deepEqual(await a.receive(ack('m5f-first-game')),saved.started);
  a.send('game.start','m5f-rematch');
  const rematch=await a.receive(ack('m5f-rematch'));
  assert.equal(rematch.phase,1);
  assert.ok(BigInt(rematch.game_id)>BigInt(saved.game_id));
  assert.ok(BigInt(rematch.seq)>BigInt(saved.finalAck.seq));

  a.send('game.snapshot.get','m5f-rematch-bidding',
         {game_id:rematch.game_id});
  const bidding=await a.receive(pub('m5f-rematch-bidding'));
  assert.equal(bidding.phase,'bidding');
  publicOnly(bidding);
  assert.ok(!Object.hasOwn(bidding,'revealed_kitty_hex'));
  assert.ok(!Object.hasOwn(bidding,'score'));
  for(let i=0;i<3;i++){
    const id='m5f-rematch-hand-'+i;
    members[i].send('game.hand.get',id,{game_id:rematch.game_id});
    assert.equal(count((await members[i].receive(priv(id))).cards_hex),17);
  }

  a.send('game.bid','m5f-rematch-pass-a',
         {game_id:rematch.game_id,turn_id:'1',bid:0});
  assert.equal((await a.receive(ack('m5f-rematch-pass-a'))).phase,1);
  b.send('game.bid','m5f-rematch-bid-b',
         {game_id:rematch.game_id,turn_id:'2',bid:2});
  assert.equal((await b.receive(ack('m5f-rematch-bid-b'))).phase,1);
  c.send('game.bid','m5f-rematch-bid-c',
         {game_id:rematch.game_id,turn_id:'3',bid:3});
  assert.equal((await c.receive(ack('m5f-rematch-bid-c'))).phase,3);
  b.send('game.snapshot.get','m5f-rematch-awarded',{game_id:rematch.game_id});
  const awarded=await b.receive(pub('m5f-rematch-awarded'));
  publicOnly(awarded);
  assert.equal(awarded.phase,'playing');
  assert.ok(!Object.hasOwn(awarded,'score'));
  assert.equal(awarded.landlord_seat,2);
  assert.deepEqual(awarded.remaining,[17,17,20]);
  assert.equal(count(awarded.revealed_kitty_hex),3);
  c.send('game.hand.get','m5f-rematch-landlord-hand',{game_id:rematch.game_id});
  assert.equal(count((await c.receive(priv('m5f-rematch-landlord-hand'))).cards_hex),20);

  privateFile({
    ...saved,rematch,
    players:members.map(client=>({player_id:client.player_id??client.player,
                                  token:client.token}))
  });
}

async function secondRestart(){
  const saved=JSON.parse(readFileSync(manifest,'utf8'));
  const a=await memberResume(saved.players[0].token,
                             saved.players[0].player_id,saved.rematch.game_id);
  assert.equal(a.automatic.phase,'playing');
  assert.equal(a.automatic.landlord_seat,2);
  a.client.send('game.snapshot.get','m5f-final-db-view',
                {game_id:saved.rematch.game_id});
  const current=await a.client.receive(pub('m5f-final-db-view'));
  assert.equal(current.phase,'playing');
  assert.equal(current.landlord_seat,2);
  a.client.send('game.start','m5f-rematch');
  assert.deepEqual(await a.client.receive(ack('m5f-rematch')),saved.rematch);
  a.client.send('game.play','m5f-play-0',{game_id:saved.game_id,
    turn_id:'1',cards_hex:saved.firstCard});
  assert.deepEqual(await a.client.receive(ack('m5f-play-0')),saved.firstPlayAck);
  a.client.send('game.start','m5f-first-game');
  assert.deepEqual(await a.client.receive(ack('m5f-first-game')),saved.started);
  a.client.send('game.snapshot.get','m5f-still-active',
                {game_id:saved.rematch.game_id});
  assert.equal((await a.client.receive(pub('m5f-still-active'))).game_id,
               saved.rematch.game_id);
  a.client.send('game.snapshot.get','m5f-old-game-id',
                {game_id:saved.game_id});
  assert.equal((await a.client.receive(error('m5f-old-game-id'))).code,
               'stale_generation_or_turn');
}

try {
  if(stage==='first')await first();
  else if(stage==='restart')await restarted();
  else await secondRestart();
  console.log('MyGames M5f full real match + rematch '+stage+': PASS');
} finally {
  for(const client of connected)client.close();
}
