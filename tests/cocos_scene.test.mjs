// Structure gate only; final Editor import/build remains a separate qualification.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';

const ROOT = new URL('../../apps/cocos/assets/', import.meta.url);
const read = (path) => JSON.parse(readFileSync(new URL(path, ROOT), 'utf8'));

function compressedUuid(uuid) {
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    const hex = uuid.replaceAll('-', '');
    assert.match(hex, /^[a-f0-9]{32}$/i);
    let result = hex.slice(0, 5);
    for (let offset = 5; offset < 32; offset += 3) {
        const n = Number.parseInt(hex.slice(offset, offset + 3), 16);
        result += alphabet[n >> 6] + alphabet[n & 63];
    }
    return result;
}

test('Cocos script UUID encoding matches official Creator serialization', () => {
    assert.equal(compressedUuid('319405d1-5414-47bd-84a0-b2fa4a35ae76'),
                 '31940XRVBRHvYSgsvpKNa52');
});

test('Lobby.scene has a stable script and scene metadata graph', () => {
    const scene = read('scenes/Lobby.scene');
    const meta = read('scenes/Lobby.scene.meta');
    const scriptMeta = read('scripts/ui/LobbyBootstrap.ts.meta');
    assert.ok(Array.isArray(scene) && scene.length >= 10);
    assert.equal(scene[0].__type__, 'cc.SceneAsset');
    assert.equal(scene[0].scene.__id__, 1);
    assert.equal(scene[1].__type__, 'cc.Scene');
    assert.equal(scene[1]._id, meta.uuid);
    assert.equal(meta.importer, 'scene');
    assert.equal(scriptMeta.importer, 'typescript');
    const canvas = scene.find((x) => x.__type__ === 'cc.Canvas');
    const bootstrap = scene.find((x) => x.__type__ === compressedUuid(scriptMeta.uuid));
    assert.ok(canvas, 'Canvas component missing');
    assert.ok(bootstrap, 'LobbyBootstrap component missing');
    assert.equal(bootstrap.node.__id__, canvas.node.__id__);
    assert.equal(bootstrap.autoConnect, true);
    assert.equal(bootstrap.serverUrl, 'ws://127.0.0.1:1780/ws');
    assert.ok(scene.some((x) => x.__type__ === 'cc.Camera'));
    assert.ok(scene.some((x) => x.__type__ === 'cc.Widget'));
    assert.ok(scene.some((x) => x.__type__ === 'cc.UITransform' &&
        x._contentSize?.width === 1280 && x._contentSize?.height === 720));
});

test('scene __id__ references resolve and roots are consistently owned', () => {
    const scene = read('scenes/Lobby.scene');
    const references = [];
    const visit = (data) => {
        if (Array.isArray(data)) return data.forEach(visit);
        if (!data || typeof data !== 'object') return;
        if (typeof data.__id__ === 'number') {
            references.push(data.__id__);
            assert.ok(data.__id__ >= 0 && data.__id__ < scene.length);
            return;
        }
        Object.values(data).forEach(visit);
    };
    scene.forEach(visit);
    assert.ok(references.length > 20);
    const canvasNode = scene.find((x) => x.__type__ === 'cc.Node' && x._name === 'Canvas');
    assert.ok(canvasNode);
    assert.equal(canvasNode._parent.__id__, 1);
    for (const reference of canvasNode._components) {
        assert.equal(scene[reference.__id__].node.__id__, scene.indexOf(canvasNode));
    }
});

test('project targets Creator 3.8 and a 2D runtime scene', () => {
    const packageJson = JSON.parse(readFileSync(new URL('../../apps/cocos/package.json', import.meta.url),'utf8'));
    assert.equal(packageJson.type, '2d');
    assert.equal(packageJson.creator.version, '3.8.8');
});
