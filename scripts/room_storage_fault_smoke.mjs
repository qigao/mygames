import assert from 'node:assert/strict';

const socket = new WebSocket('ws://127.0.0.1:1780/ws');
let completed = 0;
let greeting = false;

try {
    await new Promise((resolve, reject) => {
        const deadline = setTimeout(
            () => reject(new Error('Durable fault WS smoke timeout')), 6000);
        const fail = (error) => {
            clearTimeout(deadline);
            reject(error);
        };
        socket.addEventListener('error', () => fail(new Error('WS transport failure')));
        socket.addEventListener('close', () => {
            if (completed !== 2) fail(new Error('Connection closed before fault proof'));
        });
        socket.addEventListener('message', (event) => {
            try {
                const message = JSON.parse(event.data);
                if (message.type === 'server.hello') {
                    assert.equal(greeting, false);
                    greeting = true;
                    socket.send(JSON.stringify({
                        v: 1, type: 'room.create', request_id: 'fail-1'
                    }));
                    return;
                }
                if (message.type !== 'room.error')
                    throw new Error('Unexpected ACK or snapshot after injected fault: ' +
                                    message.type);
                assert.equal(message.code, 'storage_unavailable');
                if (completed === 0) {
                    assert.equal(message.request_id, 'fail-1');
                    completed = 1;
                    socket.send(JSON.stringify({
                        v: 1, type: 'room.create', request_id: 'fail-2'
                    }));
                } else {
                    assert.equal(message.request_id, 'fail-2');
                    completed = 2;
                    clearTimeout(deadline);
                    resolve();
                }
            } catch (error) {
                fail(error);
            }
        });
    });
    assert.equal(completed, 2);
    console.log('mygames.gateway_live_transaction_rejection: PASS');
} finally {
    socket.close();
}
