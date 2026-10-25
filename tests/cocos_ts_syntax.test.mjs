import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import ts from 'typescript';

const root = new URL('../apps/cocos/assets/scripts/', import.meta.url);
const scripts = [
    'net/Protocol.ts',
    'net/GameSocket.ts',
    'state/RoomStore.ts',
    'ui/ConnectionPanel.ts',
    'ui/LobbyBootstrap.ts'
];
for (const name of scripts) {
    test('Cocos TypeScript parses: ' + name, () => {
        const source = readFileSync(new URL(name, root), 'utf8');
        const result = ts.transpileModule(source, {
            fileName: name,
            reportDiagnostics: true,
            compilerOptions: {
                target: ts.ScriptTarget.ES2022,
                module: ts.ModuleKind.ESNext,
                experimentalDecorators: true,
                strict: true
            }
        });
        const errors = (result.diagnostics ?? []).filter(
            (d) => d.category === ts.DiagnosticCategory.Error
        );
        assert.deepEqual(errors.map((d) => ts.flattenDiagnosticMessageText(d.messageText, '\n')), []);
        assert.ok(result.outputText.length > 0);
    });
}
