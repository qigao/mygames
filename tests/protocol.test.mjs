import {test} from 'node:test';
import assert from 'node:assert/strict';
import {encodePing, encodeResume, encodeRoomCommand, parseServerFrame, PROTOCOL_VERSION} from '../apps/cocos/assets/scripts/net/Protocol.ts';
import {RoomStore} from '../apps/cocos/assets/scripts/state/RoomStore.ts';

const snapshot = (revision, epoch = 1) => ({
  v: 1, type: 'room.snapshot', room_id: 'room-1', room_epoch: epoch, revision,
  phase: 'waiting', owner_id: '1',
  seats: [{player_id: '1', ready: false}, null, null]
});

test('development ping/hello contract', () => {
  assert.equal(PROTOCOL_VERSION, 1);
  assert.equal(encodePing(), '{"v":1,"type":"client.ping"}');
  assert.equal(parseServerFrame('{"v":1,"type":"server.pong"}').type, 'server.pong');
  const hello = parseServerFrame('{"v":1,"type":"server.hello","capabilities":["ping","rooms"],"player_id":"1"}');
  assert.deepEqual(hello.capabilities, ['ping','rooms']);
  assert.equal(hello.player_id, '1');
});

test('C11 strict room wire format', () => {
  assert.equal(encodeRoomCommand('room.create', 'req-1'),
    '{"v":1,"type":"room.create","request_id":"req-1"}');
  assert.equal(encodeRoomCommand('room.join', 'req-2', {room_id:'room-1'}),
    '{"v":1,"type":"room.join","request_id":"req-2","room_id":"room-1"}');
  assert.equal(encodeRoomCommand('room.ready', 'req-3', {ready:true}),
    '{"v":1,"type":"room.ready","request_id":"req-3","ready":true}');
  assert.throws(() => encodeRoomCommand('room.join', 'x', {room_id:'room/escape'}));
  assert.throws(() => encodeRoomCommand('room.ready', 'x'));
});

test('validates server room ACK, error and snapshot', () => {
  const ack = parseServerFrame('{"v":1,"type":"room.ack","request_id":"req-1","room_id":"room-1","revision":1,"seat":0}');
  assert.equal(ack.type, 'room.ack');
  assert.equal(parseServerFrame('{"v":1,"type":"room.error","request_id":"r","code":"full"}').type, 'room.error');
  assert.equal(parseServerFrame(JSON.stringify(snapshot(3))).type, 'room.snapshot');
});

test('rejects malformed, stale type or structurally invalid frames', () => {
  for (const bad of [
    '{', '[]',
    '{"type":"server.hello","capabilities":["ping"]}',
    '{"v":2,"type":"server.hello","capabilities":["ping"]}',
    '{"v":1,"type":"server.hello","capabilities":["ping",2]}',
    '{"v":1,"type":"server.error","code":1}',
    '{"v":1,"type":"game.hand.private","cards":[3]}',
    '{"v":1,"type":"room.ack","room_id":"room-1","revision":1,"seat":0}',
    JSON.stringify({...snapshot(2), seats: []}),
    JSON.stringify({...snapshot(2), owner_id: 'oops'}),
    JSON.stringify({...snapshot(2), revision: -1}),
    JSON.stringify({...snapshot(2), room_epoch: 0}),
  ]) assert.throws(() => parseServerFrame(bad), Error, bad);
});

test('validates opaque session lease recovery and rotated generation', () => {
  const token = 'a'.repeat(64);
  const rotated = 'b'.repeat(64);
  assert.equal(encodeResume(token),
    '{"v":1,"type":"session.resume","resume_token":"' + token + '"}');
  assert.throws(() => encodeResume('short'));
  assert.throws(() => encodeResume('G'.repeat(64)));
  const hello = parseServerFrame(JSON.stringify({
    v:1, type:'server.hello',
    capabilities:['ping','rooms','resume'],
    player_id:'7', resume_token:token
  }));
  assert.equal(hello.resume_token, token);
  const ready = parseServerFrame(JSON.stringify({
    v:1,type:'session.ready',player_id:'7',
    generation:2, resume_token:rotated, resumed:true
  }));
  assert.equal(ready.generation, 2);
  assert.equal(ready.resume_token, rotated);
  assert.equal(parseServerFrame(
    '{"v":1,"type":"session.error","code":"resume_rejected"}').code,
    'resume_rejected');
  for (const bad of [
    {v:1,type:'server.hello',capabilities:['ping'],resume_token:'bad'},
    {v:1,type:'session.ready',player_id:'7',generation:1,
     resume_token:token,resumed:true},
    {v:1,type:'session.ready',player_id:'7',generation:2,
     resume_token:token,resumed:false},
    {v:1,type:'session.ready',player_id:'7',generation:2,
     resume_token:'ABC',resumed:true},
  ]) assert.throws(() => parseServerFrame(JSON.stringify(bad)));
});

test('room presentation store rejects stale and unrelated snapshots', () => {
  const store = new RoomStore();
  assert.equal(store.applySnapshot(snapshot(1)), false);
  store.onAck({v:1,type:'room.ack',request_id:'r1',room_id:'room-1',revision:1,seat:0});
  assert.equal(store.applySnapshot(snapshot(3)), true);
  assert.equal(store.applySnapshot(snapshot(2)), false);
  assert.equal(store.applySnapshot(snapshot(3)), false);
  assert.equal(store.applySnapshot(snapshot(1,2)), true);
  assert.equal(store.applySnapshot(snapshot(9,1)), false);
  store.onAck({v:1,type:'room.ack',request_id:'r2',room_id:'room-1',revision:0,seat:3});
  assert.equal(store.current, null);
  assert.equal(store.applySnapshot(snapshot(10,2)), false);
});
