// Tools test, run inside VS Code by runTest.js (NETYAROZE_TEST_ONLY=tools, the C/C++ extension installed): the tools
// are downloaded and installed by the extension in its storage (PCSX-Redux, the MIPS toolchain, PSn00bSDK, CMake,
// Ninja), then a PsyQ and a PSn00bSDK project (in psyq/ and psn00b/) are built with them, without any path in the
// settings, and the PSn00bSDK one debugged in the PCSX-Redux installed.
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

const folder = () => vscode.workspace.workspaceFolders[0];
const ws = (...p) => path.join(folder().uri.fsPath, ...p);
let storage;

async function runTask(name) {
    const tasks = await vscode.tasks.fetchTasks({ type: 'netyaroze' });
    const task = tasks.find(t => t.name === name);
    assert.ok(task, `${name} task (tasks: ${tasks.map(t => t.name).join(', ')})`);
    const execution = await vscode.tasks.executeTask(task);
    return new Promise(resolve => {
        const listener = vscode.tasks.onDidEndTaskProcess(e => {
            if (e.execution === execution) {
                listener.dispose();
                resolve(e.exitCode);
            }
        });
    });
}

test('the tools are missing in a new profile (but ps1transfer comes with the extension)', async () => {
    const ext = vscode.extensions.getExtension('logi-26.ps1-dev-cart');
    await ext.activate();
    storage = path.join(ext.extensionPath, '..');   // (not used: the storage is the test profile's)
    const status = await vscode.commands.executeCommand('netyaroze.toolStatus');
    const byId = Object.fromEntries(status.map(t => [t.id, t]));
    console.log('        ' + status.map(t => `${t.id}: ${t.ok ? 'found' : 'missing'}`).join(', '));
    assert.ok(byId.ps1transfer.ok, 'ps1transfer: ' + byId.ps1transfer.where);
    assert.ok(/ps1transfer[\\/]ps1transfer\.exe$/i.test(byId.ps1transfer.where), 'the bundled one: ' + byId.ps1transfer.where);
});

test('installs only the toolchains ticked (PSn00bSDK: not the emulator)', async () => {
    const installed = await vscode.commands.executeCommand('netyaroze.installTools', ['psn00bsdk']);
    console.log('        installed: ' + installed.join(', '));
    assert.ok(installed.includes('psn00bsdk'), 'PSn00bSDK installed');
    assert.ok(!installed.includes('redux'), 'not PCSX-Redux');
    const status = await vscode.commands.executeCommand('netyaroze.toolStatus');
    assert.ok(status.find(t => t.id === 'psn00bsdk').ok, 'PSn00bSDK found');
    assert.ok(!status.find(t => t.id === 'redux').ok, 'PCSX-Redux still missing');
    assert.deepStrictEqual(await vscode.commands.executeCommand('netyaroze.installTools', ['psn00bsdk']), [],
                           'nothing left to install in it');
});

for (const id of ['mips', 'cmake', 'ninja', 'redux']) {
    test(`installs ${id}`, async () => {
        const started = Date.now();
        assert.strictEqual(await vscode.commands.executeCommand('netyaroze.installTool', id), true, 'installed and found');
        const status = await vscode.commands.executeCommand('netyaroze.toolStatus');
        const tool = status.find(t => t.id === id);
        console.log(`        ${tool.name}: ${tool.where} (${Math.round((Date.now() - started) / 1000)} s)`);
        assert.ok(tool.ok, 'found');
        assert.ok(/globalStorage[\\/]logi-26\.ps1-dev-cart/i.test(tool.where), 'in the extension storage: ' + tool.where);
    });
}

test('builds the PsyQ project with the installed toolchain', async () => {
    assert.strictEqual(await runTask('Build (psyq)'), 0, 'build exit code');
    assert.ok(fs.existsSync(ws('psyq', 'cube_sample.ps-exe')), 'cube_sample.ps-exe');
});

test('builds the PSn00bSDK project with the installed PSn00bSDK, CMake and Ninja', async () => {
    assert.strictEqual(await runTask('Build (psn00b)'), 0, 'build exit code');
    assert.ok(fs.existsSync(ws('psn00b', 'build', 'template.exe')), 'template.exe');
});

test('debugs the PSn00bSDK project in the installed PCSX-Redux', async () => {
    const main = vscode.Uri.file(ws('psn00b', 'main.c'));
    const line = fs.readFileSync(main.fsPath, 'utf8').split('\n').findIndex(l => l.includes('DrawSync(0);'));
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(main, new vscode.Position(line, 0)))]);
    let resolveStop;
    const stopped = new Promise(r => { resolveStop = r; });
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(s) {
            return {
                onDidSendMessage(m) {
                    if (m.type === 'event' && m.event === 'stopped') {
                        resolveStop({ session: s, body: m.body });
                    }
                },
            };
        },
    });
    try {
        assert.ok(await vscode.debug.startDebugging(folder(), { type: 'netyaroze', request: 'launch', name: 'test',
                                                                 project: 'psn00b' }), 'debugging started');
        const timeout = new Promise((_, reject) => setTimeout(() => reject(new Error('no stop within 120 s')), 120000));
        const { session, body } = await Promise.race([stopped, timeout]);
        const top = (await session.customRequest('stackTrace', { threadId: body.threadId || 1, levels: 1 })).stackFrames[0];
        console.log(`        stopped in ${top.name} ${path.basename(top.source.path)}:${top.line}; gdb: ` +
                    session.configuration.miDebuggerPath);
        assert.strictEqual(top.line, line + 1, 'at the breakpoint');
        assert.ok(/globalStorage/i.test(session.configuration.miDebuggerPath), 'the installed gdb-multiarch');
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
        await vscode.debug.stopDebugging(session);
    } finally {
        tracker.dispose();
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    }
    await new Promise(r => setTimeout(r, 3000));
    const out = cp.execFileSync('tasklist', ['/FI', 'IMAGENAME eq pcsx-redux.main', '/NH'], { encoding: 'utf8' });
    assert.ok(!out.includes('pcsx-redux.main'), 'the emulator closed');
});

async function run() {
    let failed = 0;
    for (const t of tests) {
        try {
            await t.fn();
            console.log(`  ok    ${t.name}`);
        } catch (e) {
            failed++;
            console.log(`  FAIL  ${t.name}\n        ${e.message}`);
        }
    }
    console.log(`${tests.length - failed}/${tests.length} tests passed`);
    if (failed) {
        throw new Error(`${failed} test(s) failed`);
    }
}

module.exports = { run };
