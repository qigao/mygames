import {after, test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createRequire} from 'node:module';
import ts from 'typescript';

const previousWebSocket = globalThis.WebSocket;
const generated = mkdtempSync(join(tmpdir(), 'mygames-ws-test-'));
for (const name of ['Protocol', 'GameSocket']) {
    const source = readFileSync(
        new URL('../apps/cocos/assets/scripts/net/' + name + '.ts', import.meta.url), 'utf8');
    const js = ts.transpileModule(source, {
        compilerOptions: {
            target: ts.ScriptTarget.ES2022,
            module: ts.ModuleKind.CommonJS,
            strict: true
        }
    }).outputText;
    writeFileSync(join(generated, name + '.js'), js, 'utf8');
}
const require = createRequire(import.meta.url);
const {GameSocket} = require(join(generated, 'GameSocket.js'));

class FakeWebSocket {
    static OPEN = 1;
    static instances = [];
    constructor(url) {
        assert.match(url, /^ws:\/\/127\.0\.0\.1/);
        this.readyState = 0;
        this.sent = [];
        FakeWebSocket.instances.push(this);
    }
    send(json) {
        assert.equal(this.readyState, FakeWebSocket.OPEN);
        this.sent.push(JSON.parse(json));
    }
    close() { this.readyState = 3; }
    open() { this.readyState = FakeWebSocket.OPEN; this.onopen?.(); }
    receive(message) { this.onmessage?.({data: JSON.stringify(message)}); }
    disconnect() { this.readyState = 3; this.onclose?.(); }
}
globalThis.WebSocket = FakeWebSocket;
after(() => {
    globalThis.WebSocket = previousWebSocket;
    rmSync(generated, {recursive: true, force: true});
});

const token = (ch) => ch.repeat(64);
const hello = (id, ticket) => ({
    v: 1, type: 'server.hello', capabilities: ['ping','rooms','resume'],
    player_id: String(id), resume_token: ticket
});

test('in-memory ticket survives reconnect, rotates and restores same identity', () => {
    const states = [];
    const frames = [];
    const client = new GameSocket((state) => states.push(state),
                                  (frame) => frames.push(frame));
    client.connect('ws://127.0.0.1:1780/ws');
    const first = FakeWebSocket.instances.at(-1);
    first.open();
    first.receive(hello(10, token('a')));
    assert.equal(client.state, 'ready');
    assert.equal(client.canResume, true);
    assert.equal(client.currentPlayerId, '10');
    client.createRoom();
    assert.equal(first.sent[0].type, 'room.create');
    first.disconnect();
    assert.equal(client.state, 'offline');
    assert.equal(client.canResume, true);

    client.connect('ws://127.0.0.1:1780/ws');
    const second = FakeWebSocket.instances.at(-1);
    second.open();
    second.receive(hello(11, token('b')));
    assert.equal(client.state, 'restoring');
    assert.deepEqual(second.sent, [{
        v: 1, type: 'session.resume', resume_token: token('a')
    }]);
    assert.equal(client.currentPlayerId, '10');
    assert.throws(() => client.createRoom(), /Not connected/);

    second.receive({
        v: 1, type: 'session.ready', player_id: '10',
        generation: 2, resume_token: token('c'), resumed: true
    });
    assert.equal(client.state, 'ready');
    assert.equal(client.currentPlayerId, '10');
    assert.equal(frames.at(-1).type, 'session.ready');
    assert.ok(states.includes('restoring'));
    client.ping();
    assert.equal(second.sent.at(-1).type, 'client.ping');
    client.disconnect();
    assert.equal(client.canResume, false);
    assert.equal(client.currentPlayerId, null);
});

test('expired or refused credential resets to provisional fresh player', () => {
    const frames = [];
    const client = new GameSocket(() => {}, (frame) => frames.push(frame));
    client.connect('ws://127.0.0.1:1780/ws');
    let socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(20, token('d')));
    socket.disconnect();
    client.connect('ws://127.0.0.1:1780/ws');
    socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(21, token('e')));
    assert.equal(client.state, 'restoring');
    socket.receive({v: 1, type: 'session.error', code: 'resume_rejected'});
    assert.equal(client.state, 'ready');
    assert.equal(client.currentPlayerId, '21');
    assert.equal(frames.at(-2).type, 'session.error');
    assert.equal(frames.at(-1).player_id, '21');
    socket.disconnect();
    client.connect('ws://127.0.0.1:1780/ws');
    socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(22, token('f')));
    assert.equal(socket.sent[0].resume_token, token('e'));
    client.disconnect();
});

test('transient suspend retains ticket but explicit disconnect clears it', () => {
    const client = new GameSocket(() => {}, () => {});
    client.connect('ws://127.0.0.1:1780/ws');
    const socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(40, token('d')));
    client.suspend();
    assert.equal(client.canResume, true);
    socket.disconnect();
    assert.equal(client.state, 'offline');
    assert.equal(client.canResume, true);
    client.disconnect();
    assert.equal(client.canResume, false);
});

test('invalid server ticket fails closed without granting room access', () => {
    const client = new GameSocket(() => {}, () => {});
    client.connect('ws://127.0.0.1:1780/ws');
    const socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(30, 'wrong'));
    assert.equal(client.state, 'failed');
    assert.equal(client.canResume, false);
    assert.throws(() => client.requestSnapshot(), /Not connected/);
    client.disconnect();
});

test('reconnect to incompatible server discards old room identity', () => {
    const frames = [];
    const client = new GameSocket(() => {}, (frame) => frames.push(frame));
    client.connect('ws://127.0.0.1:1780/ws');
    let socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive(hello(50, token('a')));
    // Calling connect again while connected must be a no-op.
    const count = FakeWebSocket.instances.length;
    client.connect('ws://127.0.0.1:1780/ws');
    assert.equal(FakeWebSocket.instances.length, count);
    socket.disconnect();
    client.connect('ws://127.0.0.1:1780/ws');
    socket = FakeWebSocket.instances.at(-1);
    socket.open();
    socket.receive({v: 1, type: 'server.hello',
        capabilities: ['ping','rooms'],player_id: '51'});
    assert.equal(client.state, 'ready');
    assert.equal(client.canResume, false);
    assert.equal(client.currentPlayerId, '51');
    assert.equal(frames.at(-2).code, 'resume_unavailable');
    client.disconnect();
});
