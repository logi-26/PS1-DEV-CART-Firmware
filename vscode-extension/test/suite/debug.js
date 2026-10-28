// Debugging test, run inside VS Code by runTest.js (with the C/C++ extension
// installed): debugs the template project in PCSX-Redux, or on a PlayStation
// with a PS1 DEV CART (PS1DEVCART_TEST_TARGET=devcart), stops at a breakpoint
// and checks where it stopped.
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

const ON_CART = process.env.PS1DEVCART_TEST_TARGET === 'devcart';

// The emulator still runs, or ps1transfer still holds the cartridge
function emulatorsRunning() {
    const image = ON_CART ? 'ps1transfer.exe' : 'pcsx-redux.main';
    const out = cp.execFileSync('tasklist', ['/FI', `IMAGENAME eq ${image}`, '/NH'], { encoding: 'utf8' });
    return out.includes(image);
}

async function run() {
    const folder = vscode.workspace.workspaceFolders[0];
    const main = vscode.Uri.file(path.join(folder.uri.fsPath, 'main.c'));
    // The line to break on (PS1DEVCART_TEST_BREAK: text on that line of main.c)
    const breakText = process.env.PS1DEVCART_TEST_BREAK || 'FntPrint(';
    const line = fs.readFileSync(main.fsPath, 'utf8').split('\n').findIndex(l => l.includes(breakText));
    assert.ok(line > 0, `"${breakText}" line in main.c`);

    const config = vscode.workspace.getConfiguration('ps1devcart');
    if (ON_CART) {
        await config.update('ps1transferPath', process.env.PS1DEVCART_TEST_PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
    } else {
        await config.update('emulatorPath', process.env.PS1DEVCART_TEST_EMULATOR, vscode.ConfigurationTarget.Workspace);
    }
    await vscode.extensions.getExtension('logi-26.ps1-dev-cart').activate();
    if (ON_CART) {
        await vscode.commands.executeCommand('ps1devcart.selectCart', process.env.PS1DEVCART_TEST_DEVCART);
    }

    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(main, new vscode.Position(line, 0)))]);

    // Resolves the next time the debugger stops
    let session;
    let onStop = () => {};
    const nextStop = () => new Promise(resolve => { onStop = resolve; });
    vscode.debug.registerDebugAdapterTrackerFactory('*', {
        createDebugAdapterTracker(s) {
            return {
                onDidSendMessage(m) {
                    if (m.type === 'event' && m.event === 'output' && process.env.PS1DEVCART_TEST_LOG) {
                        process.stdout.write('  | ' + m.body.output);
                    }
                    if (m.type === 'event' && m.event === 'stopped') {
                        session = s;
                        onStop(m.body);
                    }
                },
            };
        },
    });
    const stopped = nextStop();
    const timeout = new Promise((_, reject) => setTimeout(() => reject(new Error('no stop within 120 s')), 120000));

    // (a launch configuration names the project; the command may ask for it)
    const started = process.env.PS1DEVCART_TEST_LOG || process.env.PS1DEVCART_TEST_BREAK
        ? await vscode.debug.startDebugging(folder, Object.assign({ type: 'ps1devcart', request: 'launch', name: 'test', project: '',
                                                                    debugLog: !!process.env.PS1DEVCART_TEST_LOG },
                                                                  ON_CART ? { target: 'devcart' } : {}))
        : await vscode.commands.executeCommand(ON_CART ? 'ps1devcart.debugOnPs1' : 'ps1devcart.debug');
    assert.ok(started, 'debugging started');
    const stop = await Promise.race([stopped, timeout]);
    console.log('  stopped:', JSON.stringify(stop));

    const trace = await session.customRequest('stackTrace', { threadId: stop.threadId || 1, levels: 5 });
    const top = trace.stackFrames[0];
    console.log('  top frame:', top.name, top.source && top.source.path, top.line);
    assert.strictEqual(top.name.replace(/\(.*$/, '').trim(), 'main');
    assert.ok(top.source && /main\.c$/i.test(top.source.path), 'source: ' + (top.source && top.source.path));
    assert.strictEqual(top.line, line + 1);

    // A variable (no type information: read it as an int)
    const value = await session.customRequest('evaluate', { expression: '*(int *)&frame', frameId: top.id, context: 'watch' })
        .catch(e => ({ result: 'error: ' + e.message }));
    console.log('  frame =', value.result);

    // Step over: stops again at a later line of main.c
    const stepped = nextStop();
    await session.customRequest('next', { threadId: stop.threadId || 1 });
    const afterStep = await Promise.race([stepped, timeout]);
    const trace2 = await session.customRequest('stackTrace', { threadId: afterStep.threadId || 1, levels: 1 });
    const frame2 = trace2.stackFrames[0];
    console.log('  after step:', frame2.name, frame2.source && path.basename(frame2.source.path), frame2.line);
    assert.ok(frame2.source && /main\.c$/i.test(frame2.source.path), 'still in main.c');
    if (!process.env.PS1DEVCART_TEST_BREAK) {  // (in a loop, Step Over can go back up)
        assert.ok(frame2.line > top.line, `stepped to a later line (${frame2.line})`);
    }

    // Pause while running: usually stops in the interrupt handler or libps;
    // the extension then runs on to the program's next function
    vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    await session.customRequest('continue', { threadId: 1 });
    await new Promise(r => setTimeout(r, 3000));
    let inCode = nextStop();
    await session.customRequest('pause', { threadId: 1 });
    let where;
    for (let i = 0; i < 3; i++) {
        const s = await Promise.race([inCode, timeout]);
        const f = (await session.customRequest('stackTrace', { threadId: s.threadId || 1, levels: 1 })).stackFrames[0];
        console.log('  pause stop:', s.reason, JSON.stringify(s.description || ''), f.name, f.source ? `${path.basename(f.source.path)}:${f.line}` : '(no source)');
        if (f.source) {
            assert.notStrictEqual(s.reason, 'exception', 'reported as a normal stop, not an exception');
            where = f;
            break;
        }
        inCode = nextStop();
    }
    assert.ok(where, 'after Pause, stopped in code with source');
    const stepped2 = nextStop();
    await session.customRequest('next', { threadId: 1 });
    const afterStep2 = await Promise.race([stepped2, timeout]);
    const frame3 = (await session.customRequest('stackTrace', { threadId: afterStep2.threadId || 1, levels: 1 })).stackFrames[0];
    console.log('  step after pause:', frame3.name, frame3.source && `${path.basename(frame3.source.path)}:${frame3.line}`);
    assert.ok(frame3.source, 'Step Over after Pause stays in code with source');

    await vscode.debug.stopDebugging(session);
    const what = ON_CART ? 'ps1transfer' : 'emulator';
    for (let i = 0; i < 30 && emulatorsRunning(); i++) {   // (ps1transfer first removes the breakpoints on the PS1)
        await new Promise(r => setTimeout(r, 500));
    }
    assert.ok(!emulatorsRunning(), `${what} closed after debugging`);
    console.log(`  ok    debug session${ON_CART ? ' on the PS1 DEV CART' : ''}: breakpoint hit at main.c:${line + 1}, ` +
                `stepped to line ${frame2.line}, ${what} closed`);
}

module.exports = { run };
