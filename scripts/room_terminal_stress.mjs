// Real CHttp/CFlow saturation: the network owner sees CLOSE while Room
// Owner is stalled and its normal bounded task mailbox is full.
import assert from 'node:assert/strict';

class Peer {
  constructor() {
    this.frames = [];
    this.waiters = [];
    this.ws = new WebSocket('ws://127.0.0.1:1780/ws');
    this.ws.addEventListener('message', ({data}) => {
      const frame = JSON.parse(data);
      const index = this.waiters.findIndex(w=>w.predicate(frame));
      if (index<0) this.frames.push(frame);
      else {
        const [w] = this.waiters.splice(index,1);
        clearTimeout(w.t);
        w.resolve(frame);
      }
    });
  }
  next(predicate,timeout=5500) {
    const index=this.frames.findIndex(predicate);
    if(index>=0) return Promise.resolve(this.frames.splice(index,1)[0]);
    return new Promise((resolve,reject)=>{
      const w={predicate,resolve,reject,t:null};
      w.t=setTimeout(()=>{
        this.waiters=this.waiters.filter(x=>x!==w);
        reject(new Error('terminal reserve smoke timeout'));
      },timeout);
      this.waiters.push(w);
    });
  }
  send(type,request_id,options={}) {
    if(this.ws.readyState===WebSocket.OPEN)
      this.ws.send(JSON.stringify({v:1,type,request_id,...options}));
  }
  close(){this.ws.close();}
}
const peers=[];
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const ack=id=>x=>x.type==='room.ack' && x.request_id===id;
const snapshot=n=>x=>x.type==='room.snapshot' && x.revision===n;
try {
  const a=new Peer(), b=new Peer();
  peers.push(a,b);
  const ah=await a.next(x=>x.type==='server.hello');
  const bh=await b.next(x=>x.type==='server.hello');
  a.send('room.create','s-create');
  const created=await a.next(ack('s-create'));
  await a.next(snapshot(1));
  b.send('room.join','s-join',{room_id:created.room_id});
  await b.next(ack('s-join'));
  await a.next(snapshot(2));
  await b.next(snapshot(2));

  // This particular request stalls the CFlow Owner for 900ms ONLY in CI.
  a.send('room.snapshot.get','stall-owner');
  await pause(90);
  for(let i=0;i<18;i++){
    a.send('room.snapshot.get','flood-'+i);
    await pause(8);
  }
  // The terminal is recorded independently of the saturated room mailbox.
  b.close();
  await pause(1350);

  const c=new Peer(); peers.push(c);
  await c.next(x=>x.type==='server.hello');
  c.ws.send(JSON.stringify({v:1,type:'session.resume',resume_token:bh.resume_token}));
  const restored=await c.next(x=>x.type==='session.ready' || x.type==='session.error');
  assert.equal(restored.type,'session.ready','CLOSE was not settled before resume');
  assert.equal(restored.player_id,bh.player_id);
  assert.notEqual(restored.resume_token,bh.resume_token);
  const recovered=await c.next(x=>x.type==='room.snapshot');
  assert.equal(recovered.room_id,created.room_id);
  assert.equal(recovered.seats[1]?.player_id,bh.player_id);
  assert.equal(recovered.seats.filter(Boolean).length,2);
  assert.equal(recovered.owner_id,ah.player_id);
  console.log('mygames.terminal_queue_stress: PASS '
    + '(saturated owner, CLOSE reserve, identity restored)');
} finally {
  for(const p of peers) p.close();
}
