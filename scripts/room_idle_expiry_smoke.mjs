// Real no-ingress timer proof: after the other player CLOSES, the
// surviving connection sends NO further command until its snapshot arrives.
import assert from 'node:assert/strict';
import {writeFileSync} from 'node:fs';

class Peer {
  constructor() {
    this.messages = [];
    this.waiters = [];
    this.ws = new WebSocket('ws://127.0.0.1:1780/ws');
    this.ws.addEventListener('message', ({data}) => {
      try {
        const value = JSON.parse(data);
        const index = this.waiters.findIndex(w => w.predicate(value));
        if (index < 0) this.messages.push(value);
        else {
          const [w] = this.waiters.splice(index, 1);
          clearTimeout(w.timeout);
          w.resolve(value);
        }
      } catch (error) {
        for (const w of this.waiters) {
          clearTimeout(w.timeout);
          w.reject(error);
        }
        this.waiters = [];
      }
    });
  }
  next(predicate, ms=6000) {
    const index = this.messages.findIndex(predicate);
    if (index !== -1) return Promise.resolve(this.messages.splice(index,1)[0]);
    return new Promise((resolve,reject) => {
      const waiter = {predicate, resolve, reject, timeout:null};
      waiter.timeout=setTimeout(()=>{
        this.waiters=this.waiters.filter(x=>x!==waiter);
        reject(new Error('no-ingress room timer deadline exceeded'));
      },ms);
      this.waiters.push(waiter);
    });
  }
  send(type, request_id, fields={}) {
    this.ws.send(JSON.stringify({v:1,type,request_id,...fields}));
  }
  close() { this.ws.close(); }
}
const clients = [];
const ack = id => x => x.type==='room.ack' && x.request_id===id;
const snap = revision => x => x.type==='room.snapshot' && x.revision===revision;
try {
  const a = new Peer(); clients.push(a);
  const b = new Peer(); clients.push(b);
  const aHello = await a.next(x=>x.type==='server.hello');
  const bHello = await b.next(x=>x.type==='server.hello');
  assert.equal(aHello.v, 1);
  assert.equal(bHello.v, 1);
  assert.notEqual(aHello.player_id,bHello.player_id);
  a.send('room.create','idle-create');
  const created = await a.next(ack('idle-create'));
  assert.equal(created.revision,1);
  const roomId = created.room_id;
  await a.next(snap(1));
  b.send('room.join','idle-join',{room_id:roomId});
  assert.equal((await b.next(ack('idle-join'))).seat,1);
  await a.next(snap(2));
  await b.next(snap(2));

  // No further outbound commands from either session after this point.
  b.close();
  const after = await a.next(snap(3));
  assert.equal(after.room_id,roomId);
  assert.equal(after.room_epoch,2);
  assert.equal(after.owner_id,aHello.player_id);
  assert.equal(after.seats.filter(Boolean).length,1);
  assert.equal(after.seats[1],null);
  const file=process.env.MYGAMES_IDLE_AUDIT_OUTPUT;
  assert.ok(file,'missing audit manifest path');
  writeFileSync(file,JSON.stringify({
    room_id:roomId, expired_player:bHello.player_id,
    remaining_player:aHello.player_id
  }),{encoding:'utf8',mode:0o600});
  console.log('mygames.idle_expiry: PASS (no new inbound message)');
} finally {
  for(const c of clients) c.close();
}
