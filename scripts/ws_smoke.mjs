// Run with Node.js >= 22 against an already-running development gateway.
import assert from 'node:assert/strict';

const root = 'http://127.0.0.1:1780';
const response = await fetch(root + '/health');
assert.equal(response.status, 200);
const health = await response.json();
assert.equal(health.protocol, 1);
assert.equal(health.mode, 'development');
assert.equal(health.room_owner, 'cflow_serial');

const ws = new WebSocket('ws://127.0.0.1:1780/ws');
const start = Date.now();
let done = false;
try {
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('WebSocket timeout')), 5000);
    ws.addEventListener('message', (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === 'server.hello') {
          assert.equal(msg.v, 1);
          assert.ok(msg.capabilities.includes('ping'));
          ws.send('{"v":1,"type":"client.ping"}');
        } else if (msg.type === 'server.pong') {
          clearTimeout(timeout);
          done = true;
          resolve();
        } else {
          clearTimeout(timeout);
          reject(new Error('Unexpected frame ' + msg.type));
        }
      } catch (error) {
        clearTimeout(timeout);
        reject(error);
      }
    });
    ws.addEventListener('error', () => { clearTimeout(timeout); reject(new Error('WebSocket error')); });
  });
} finally {
  ws.close();
}
assert.equal(done, true);
console.log('gateway smoke OK (' + (Date.now() - start) + 'ms)');
