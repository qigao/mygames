import {test} from 'node:test';
import assert from 'node:assert/strict';
import {encodePing, parseServerFrame, PROTOCOL_VERSION} from '../apps/cocos/assets/scripts/net/Protocol.ts';

test('matches CHttp development ping contract exactly', () => {
  assert.equal(PROTOCOL_VERSION, 1);
  assert.equal(encodePing(), '{"v":1,"type":"client.ping"}');
});

test('parses server hello with explicit capabilities', () => {
  const hello = parseServerFrame('{"v":1,"type":"server.hello","capabilities":["ping"]}');
  assert.equal(hello.type, 'server.hello');
  assert.deepEqual(hello.capabilities, ['ping']);
});

test('parses pong and explicit unsupported command', () => {
  assert.equal(parseServerFrame('{"v":1,"type":"server.pong"}').type, 'server.pong');
  const error = parseServerFrame('{"v":1,"type":"server.error","code":"unsupported_command"}');
  assert.equal(error.type, 'server.error');
  assert.equal(error.code, 'unsupported_command');
});

test('rejects malformed, versioned or unknown input', () => {
  for (const bad of [
    '{',
    '[]',
    '{"type":"server.hello","capabilities":["ping"]}',
    '{"v":2,"type":"server.hello","capabilities":["ping"]}',
    '{"v":1,"type":"server.hello","capabilities":["ping",2]}',
    '{"v":1,"type":"server.error","code":1}',
    '{"v":1,"type":"game.hand.private","cards":[3]}'
  ]) assert.throws(() => parseServerFrame(bad), Error, bad);
});
