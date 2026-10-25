// Manual, real CHttp WebSocket integration: node scripts/room_ws_smoke.mjs
// Requires node >= 22, and a live MYGAMES_ENABLE_GATEWAY=ON server on loopback.
import assert from 'node:assert/strict';

const URL = 'ws://127.0.0.1:1780/ws';
const clients = [];

class Client {
    constructor() {
        this.socket = new WebSocket(URL);
        this.messages = [];
        this.waiters = [];
        this.socket.addEventListener('message', (event) => {
            const value = JSON.parse(event.data);
            const i = this.waiters.findIndex((w) => w.predicate(value));
            if (i === -1) this.messages.push(value);
            else {
                const [w] = this.waiters.splice(i, 1);
                clearTimeout(w.timer);
                w.resolve(value);
            }
        });
    }
    receive(predicate, milliseconds = 5000) {
        const i = this.messages.findIndex(predicate);
        if (i !== -1) return Promise.resolve(this.messages.splice(i, 1)[0]);
        return new Promise((resolve, reject) => {
            const item = {predicate, resolve, reject, timer: undefined};
            item.timer = setTimeout(() => {
                this.waiters = this.waiters.filter((w) => w !== item);
                reject(new Error('Timed out waiting for WS event'));
            }, milliseconds);
            this.waiters.push(item);
        });
    }
    send(type, request_id, fields = {}) {
        this.socket.send(JSON.stringify({v: 1, type, request_id, ...fields}));
    }
    close() {
        this.waiters.forEach((w) => {
            clearTimeout(w.timer);
            w.reject(new Error('Client closed'));
        });
        this.waiters = [];
        this.socket.close();
    }
}

const ack = (request_id) => (m) => m.type === 'room.ack' && m.request_id === request_id;
const error = (request_id) => (m) => m.type === 'room.error' && m.request_id === request_id;
const snap = (revision) => (m) => m.type === 'room.snapshot' &&
    m.revision === revision;

async function connect() {
    const c = new Client();
    clients.push(c);
    const hello = await c.receive((m) => m.type === 'server.hello');
    assert.equal(hello.v, 1);
    assert.ok(hello.capabilities.includes('rooms'));
    assert.match(hello.player_id, /^[1-9][0-9]*$/);
    c.player = hello.player_id;
    c.ticket = hello.resume_token;
    return c;
}

try {
    const health = await fetch('http://127.0.0.1:1780/health');
    assert.equal(health.status, 200);
    const a = await connect();
    const b = await connect();
    const c = await connect();
    const d = await connect();

    a.send('room.create', 'create-1');
    const created = await a.receive(ack('create-1'));
    assert.equal(created.revision, 1);
    assert.equal(created.seat, 0);
    const id = created.room_id;
    assert.match(id, /^room-[1-9][0-9]*$/);
    assert.equal((await a.receive(snap(1))).room_id, id);

    b.send('room.join', 'join-2', {room_id: id});
    assert.equal((await b.receive(ack('join-2'))).seat, 1);
    assert.equal((await a.receive(snap(2))).revision, 2);
    await b.receive(snap(2));

    c.send('room.join', 'join-3', {room_id: id});
    assert.equal((await c.receive(ack('join-3'))).seat, 2);
    const full = await c.receive(snap(3));
    assert.equal(full.seats.filter(Boolean).length, 3);
    await a.receive(snap(3));
    await b.receive(snap(3));

    d.send('room.join', 'join-4', {room_id: id});
    assert.equal((await d.receive(error('join-4'))).code, 'full');

    a.send('room.ready', 'ready-1', {ready: true});
    const readyAck = await a.receive(ack('ready-1'));
    assert.equal(readyAck.revision, 4);
    await a.receive(snap(4));
    await b.receive(snap(4));
    await c.receive(snap(4));

    // At-least-once replay of exactly the same command is idempotent.
    a.send('room.ready', 'ready-1', {ready: true});
    assert.deepEqual(await a.receive(ack('ready-1')), readyAck);

    b.send('room.ready', 'ready-2', {ready: true});
    await b.receive(ack('ready-2'));
    await a.receive(snap(5));
    await b.receive(snap(5));
    await c.receive(snap(5));

    // The same request ID with a different command body is rejected.
    b.send('room.ready', 'ready-2', {ready: false});
    assert.equal((await b.receive((m) => m.type === 'server.error')).code,
                 'request_id_conflict');

    a.send('room.leave', 'leave-1');
    const left = await a.receive(ack('leave-1'));
    assert.equal(left.seat, 3);
    const handover = await b.receive(snap(6));
    assert.equal(handover.owner_id, handover.seats[1].player_id);
    await c.receive(snap(6));

    b.send('room.snapshot.get', 'snapshot-2');
    assert.equal((await b.receive(ack('snapshot-2'))).revision, 6);
    assert.equal((await b.receive(snap(6))).revision, 6);

    // Close retains seat and player identity during a bounded recovery grace.
    const bClosed = new Promise((resolve) => b.socket.addEventListener('close', resolve, {once: true}));
    b.close();
    await bClosed;
    const bResumed = await connect();
    bResumed.socket.send(JSON.stringify({
        v: 1, type: 'session.resume', resume_token: b.ticket
    }));
    const resume = await bResumed.receive((m) => m.type === 'session.ready');
    assert.equal(resume.player_id, b.player);
    assert.equal(resume.generation, 2);
    assert.equal(resume.resumed, true);
    assert.notEqual(resume.resume_token, b.ticket);
    assert.equal((await bResumed.receive(snap(6))).owner_id, b.player);

    // Retransmission across a connection generation is idempotent: the
    // original acknowledgement must be replayed, not a new room revision.
    bResumed.send('room.ready', 'ready-2', {ready: true});
    const cached = await bResumed.receive(ack('ready-2'));
    assert.equal(cached.revision, 5);

    // The old credential was consumed and cannot claim the same identity.
    const attacker = await connect();
    attacker.socket.send(JSON.stringify({
        v: 1, type: 'session.resume', resume_token: b.ticket
    }));
    assert.equal((await attacker.receive((m) => m.type === 'session.error')).code,
                 'resume_rejected');

    // Room has a free seat after A left; a new player must not replace B.
    const e = await connect();
    e.send('room.join', 'join-after-resume', {room_id: id});
    assert.equal((await e.receive(ack('join-after-resume'))).seat, 0);
    const afterJoin = await c.receive(snap(7));
    assert.equal(afterJoin.seats.filter(Boolean).length, 3);
    assert.equal(afterJoin.seats[1].player_id, b.player);
    await bResumed.receive(snap(7));
    await e.receive(snap(7));

    // While B is still online, its current ticket must not be taken over.
    attacker.socket.send(JSON.stringify({
        v: 1, type: 'session.resume', resume_token: resume.resume_token
    }));
    assert.equal((await attacker.receive((m) => m.type === 'session.error')).code,
                 'resume_rejected');

    console.log('MyGames CHttp room WebSocket integration: PASS');
} finally {
    for (const c of clients) c.close();
}
