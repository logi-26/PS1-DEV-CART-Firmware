// PsyQ / PSn00bSDK project test, run inside VS Code by runTest.js (with the
// C/C++ extension installed): a PSX.Dev PsyQ project (NETYAROZE_TEST_KIND=make)
// or the PSn00bSDK template (=cmake) is found, built on Windows, run on a PS1
// DEV CART and debugged on it and in PCSX-Redux (each when its environment
// variables are set).
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

const KIND = process.env.NETYAROZE_TEST_KIND;
const DEVCART = process.env.NETYAROZE_TEST_DEVCART;
const PS1TRANSFER = process.env.NETYAROZE_TEST_PS1TRANSFER;
const EMULATOR = process.env.NETYAROZE_TEST_EMULATOR;
const BREAK = KIND === 'cmake' ? 'DrawSync(0);' : 'FntPrint("Code compiled';

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

const folder = () => vscode.workspace.workspaceFolders[0];
const ws = (...p) => path.join(folder().uri.fsPath, ...p);

async function runTask(name) {
    const tasks = await vscode.tasks.fetchTasks({ type: 'netyaroze' });
    const task = tasks.find(t => t.name === name);
    assert.ok(task, `${name} task (tasks: ${tasks.map(t => `${t.source}: ${t.name}`).join(', ')})`);
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

// (the C/C++ extension's IntelliSense may hold the file for a moment on Windows)
async function writeFile(file, text) {
    for (let i = 0; ; i++) {
        try {
            fs.writeFileSync(file, text);
            return;
        } catch (e) {
            if (e.code !== 'EBUSY' || i >= 50) {
                throw e;
            }
            await new Promise(r => setTimeout(r, 200));
        }
    }
}

function running(image) {
    const out = cp.execFileSync('tasklist', ['/FI', `IMAGENAME eq ${image}`, '/NH'], { encoding: 'utf8' });
    return out.includes(image);
}

// Starts debugging (a command) and resolves the first stop: { session, top frame }
async function debugStop(command) {
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
        assert.ok(await vscode.commands.executeCommand(command), 'debugging started');
        const timeout = new Promise((_, reject) => setTimeout(() => reject(new Error('no stop within 120 s')), 120000));
        const { session, body } = await Promise.race([stopped, timeout]);
        const top = (await session.customRequest('stackTrace', { threadId: body.threadId || 1, levels: 1 })).stackFrames[0];
        return { session, top };
    } finally {
        tracker.dispose();
    }
}

test('the project is found, with its kind', async () => {
    await vscode.extensions.getExtension('logi-26.ps1-dev-cart').activate();
    const tasks = await vscode.tasks.fetchTasks({ type: 'netyaroze' });
    const build = tasks.find(t => t.name === 'Build');
    assert.ok(build, 'Build task');
    assert.strictEqual(build.source, KIND === 'cmake' ? 'PSn00bSDK' : 'PsyQ / nugget');
    assert.ok(!tasks.some(t => t.name === 'Build Models'), 'no Net Yaroze tasks');
});

test('Build makes the PS-X EXE and its ELF', async () => {
    assert.strictEqual(await runTask('Build'), 0, 'build exit code');
    const exe = KIND === 'cmake' ? ws('build', 'template.exe') : ws('cube_sample.ps-exe');
    assert.ok(fs.existsSync(exe), exe);
    assert.ok(fs.existsSync(exe.replace(/\.[^.\\/]*$/, '.elf')), 'ELF');
    assert.strictEqual(fs.readFileSync(exe).toString('latin1', 0, 8), 'PS-X EXE');
});

test('compiler errors go to the Problems panel', async () => {
    const main = ws('main.c');
    const original = fs.readFileSync(main, 'utf8');
    await writeFile(main, original + '\nint broken(void) { return undeclared_thing; }\n');
    try {
        assert.notStrictEqual(await runTask('Build'), 0, 'the build fails');
        const found = await new Promise(resolve => {
            const end = Date.now() + 10000;
            const check = () => {
                const all = vscode.languages.getDiagnostics().flatMap(([uri, ds]) => ds.map(d => ({ uri, d })));
                const hit = all.find(x => /undeclared_thing/.test(x.d.message));
                if (hit || Date.now() > end) {
                    resolve(hit);
                } else {
                    setTimeout(check, 300);
                }
            };
            check();
        });
        assert.ok(found, 'the error is in the Problems panel');
        assert.ok(/main\.c$/i.test(found.uri.fsPath), 'in main.c: ' + found.uri.fsPath);
    } finally {
        await writeFile(main, original);
    }
    assert.strictEqual(await runTask('Build'), 0, 'builds again');
});

test('Run on PS1 runs the program on the PS1 DEV CART', async () => {
    if (!DEVCART || !PS1TRANSFER) {
        console.log('        (skipped: set NETYAROZE_TEST_DEVCART and NETYAROZE_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('netyaroze');
    await config.update('devcartTty', 'bios', vscode.ConfigurationTarget.Workspace);  // (no console: ps1transfer ends)
    try {
        const execution = await vscode.commands.executeCommand('netyaroze.runOnPs1');
        assert.ok(execution, 'task started');
        const code = await new Promise(resolve => {
            const listener = vscode.tasks.onDidEndTaskProcess(e => {
                if (e.execution === execution) {
                    listener.dispose();
                    resolve(e.exitCode);
                }
            });
        });
        assert.strictEqual(code, 0, 'ps1transfer exit code');
        await new Promise(r => setTimeout(r, 2000));
        const info = cp.execFileSync(PS1TRANSFER, ['-n', DEVCART, '-info'], { encoding: 'utf8' });
        assert.ok(/state=running/.test(info), 'the program runs: ' + info.replace(/\s+/g, ' '));
    } finally {
        await config.update('devcartTty', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

async function debugTest(command, image) {
    const main = vscode.Uri.file(ws('main.c'));
    const line = fs.readFileSync(main.fsPath, 'utf8').split('\n').findIndex(l => l.includes(BREAK));
    assert.ok(line > 0, `"${BREAK}" line in main.c`);
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(main, new vscode.Position(line, 0)))]);
    try {
        const { session, top } = await debugStop(command);
        console.log(`        stopped in ${top.name} ${top.source && path.basename(top.source.path)}:${top.line}`);
        assert.ok(top.source && /main\.c$/i.test(top.source.path), 'stopped in main.c');
        assert.strictEqual(top.line, line + 1, 'at the breakpoint');
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
        await vscode.debug.stopDebugging(session);
    } finally {
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    }
    for (let i = 0; i < 40 && running(image); i++) {
        await new Promise(r => setTimeout(r, 500));
    }
    assert.ok(!running(image), `${image} closed after debugging`);
}

test('Debug on PS1 stops at a breakpoint (PS1 DEV CART)', async () => {
    if (!DEVCART || !PS1TRANSFER) {
        console.log('        (skipped: set NETYAROZE_TEST_DEVCART and NETYAROZE_TEST_PS1TRANSFER)');
        return;
    }
    await debugTest('netyaroze.debugOnPs1', 'ps1transfer.exe');
});

test('Debug in the emulator stops at a breakpoint (PCSX-Redux)', async () => {
    if (!EMULATOR) {
        console.log('        (skipped: set NETYAROZE_TEST_EMULATOR)');
        return;
    }
    await debugTest('netyaroze.debug', 'pcsx-redux.main');
});

test('Debug Linked on Two PS1s: both stop at a breakpoint', async () => {
    const second = process.env.NETYAROZE_TEST_DEVCART2;
    if (!DEVCART || !second || !PS1TRANSFER) {
        console.log('        (skipped: set NETYAROZE_TEST_DEVCART, NETYAROZE_TEST_DEVCART2 and NETYAROZE_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('netyaroze');
    await config.update('devcartSerial2', second, vscode.ConfigurationTarget.Workspace);
    await config.update('devcartLink', 'cable', vscode.ConfigurationTarget.Workspace);
    const main = vscode.Uri.file(ws('main.c'));
    const line = fs.readFileSync(main.fsPath, 'utf8').split('\n').findIndex(l => l.includes(BREAK));
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(main, new vscode.Position(line, 0)))]);
    const stops = new Map();
    const tracker = vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(s) {
            return {
                onDidSendMessage(m) {
                    if (m.type === 'event' && m.event === 'stopped' && !stops.has(s.id)) {
                        stops.set(s.id, { session: s, body: m.body });
                    }
                },
            };
        },
    });
    try {
        await vscode.commands.executeCommand('netyaroze.debugLinkedOnCarts');
        const end = Date.now() + 180000;
        while (stops.size < 2 && Date.now() < end) {
            await new Promise(r => setTimeout(r, 500));
        }
        assert.strictEqual(stops.size, 2, 'both sessions stopped');
        for (const { session, body } of stops.values()) {
            const top = (await session.customRequest('stackTrace', { threadId: body.threadId || 1, levels: 1 })).stackFrames[0];
            console.log(`        ${session.name}: ${top.name} ${top.source && path.basename(top.source.path)}:${top.line}`);
            assert.strictEqual(top.line, line + 1, 'at the breakpoint');
        }
    } finally {
        tracker.dispose();
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
        for (const { session } of stops.values()) {
            await vscode.debug.stopDebugging(session);
        }
        await config.update('devcartLink', undefined, vscode.ConfigurationTarget.Workspace);
    }
    for (let i = 0; i < 40 && running('ps1transfer.exe'); i++) {
        await new Promise(r => setTimeout(r, 500));
    }
    assert.ok(!running('ps1transfer.exe'), 'the cartridges freed after debugging');
});

async function run() {
    const config = vscode.workspace.getConfiguration('netyaroze');
    await config.update('ps1transferPath', PS1TRANSFER || undefined, vscode.ConfigurationTarget.Workspace);
    await config.update('emulatorPath', EMULATOR || undefined, vscode.ConfigurationTarget.Workspace);
    await config.update('psn00bsdkPath', process.env.NETYAROZE_TEST_PSN00BSDK || undefined, vscode.ConfigurationTarget.Workspace);
    if (DEVCART) {
        await config.update('devcartSerial', DEVCART, vscode.ConfigurationTarget.Workspace);
    }
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
