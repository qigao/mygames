// The test manifest is a private 0600 file in the CI temporary directory.
// Never print or upload the plaintext 256-bit recovery token.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const path = process.env.MYGAMES_AUDIT_OUTPUT;
assert.ok(path, 'MYGAMES_AUDIT_OUTPUT not set');
const audit = JSON.parse(readFileSync(path, 'utf8'));
assert.match(audit.recover_ticket, /^[0-9a-f]{64}$/);
assert.match(audit.recover_player_id, /^[1-9][0-9]*$/);
assert.match(audit.room_id, /^room-[1-9][0-9]*$/);

class Client {
    constructor() {
        this.ws = new WebSocket('ws://127.0.0.1:1780/ws');
        this.pending = [];
        this.waiters = [];
        this.ws.addEventListener('message', (event) => {
            try {
                const frame = JSON.parse(event.data);
                const i = this.waiters.findIndex((w) => w.predicate(frame));
                if (i === -1) this.pending.push(frame);
                else {
                    const [waiter] = this.waiters.splice(i, 1);
                    clearTimeout(waiter.timeout);
                    waiter.resolve(frame);
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
    next(predicate, milliseconds = 5000) {
        const index = this.pending.findIndex(predicate);
        if (index !== -1) return Promise.resolve(this.pending.splice(index, 1)[0]);
        return new Promise((resolve, reject) => {
            const item = {predicate, resolve, reject, timeout: null};
            item.timeout = setTimeout(() => {
                this.waiters = this.waiters.filter((x) => x !== item);
                reject(new Error('Timed out waiting for durable WS frame'));
            }, milliseconds);
            this.waiters.push(item);
        });
    }
    send(msg) { this.ws.send(JSON.stringify(msg)); }
    close() { this.ws.close(); }
}
const connections = [];

try {
    const c = new Client();
    connections.push(c);
    const hello = await c.next((m) => m.type === 'server.hello');
    assert.equal(hello.v, 1);
    assert.ok(hello.capabilities.includes('resume'));
    assert.notEqual(hello.player_id, audit.recover_player_id);
    c.send({v: 1, type: 'session.resume', resume_token: audit.recover_ticket});
    const restored = await c.next((m) => m.type === 'session.ready' ||
                                          m.type === 'session.error');
    assert.equal(restored.type, 'session.ready', 'ticket recovery rejected');
    assert.equal(restored.player_id, audit.recover_player_id);
    assert.equal(restored.resumed, true);
    assert.ok(restored.generation >= 3);
    assert.notEqual(restored.resume_token, audit.recover_ticket);
    const room = await c.next((m) => m.type === 'room.snapshot');
    assert.equal(room.room_id, audit.room_id);
    assert.equal(room.room_epoch, 3);
    assert.equal(room.seats[1]?.player_id, audit.recover_player_id);
    assert.equal(room.seats.filter(Boolean).length, 3);

    c.send({v: 1, type: 'room.snapshot.get', request_id: 'resume-audit-1'});
    const ack = await c.next((m) => m.type === 'room.ack' ||
                                     m.type === 'room.error');
    assert.equal(ack.type, 'room.ack');
    assert.equal(ack.room_id, audit.room_id);
    assert.equal((await c.next((m) => m.type === 'room.snapshot')).room_epoch, 3);

    // Exact same old credential must be useless after durable rotation.
    const attacker = new Client();
    connections.push(attacker);
    await attacker.next((m) => m.type === 'server.hello');
    attacker.send({v: 1, type: 'session.resume',
                   resume_token: audit.recover_ticket});
    const rejected = await attacker.next((m) => m.type === 'session.error' ||
                                           m.type === 'session.ready');
    assert.equal(rejected.type, 'session.error');
    assert.equal(rejected.code, 'resume_rejected');
    console.log('mygames.gateway_restart_resume: PASS '
                + '(original player, same seat, epoch fencing, stale ticket refused)');
} finally {
    for (const c of connections) c.close();
}
