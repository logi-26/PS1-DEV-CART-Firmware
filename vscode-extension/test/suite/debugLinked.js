// Linked debugging test, run inside VS Code by runTest.js (with the C/C++
// extension installed): the template project in two linked PCSX-Redux
// (PS1DEVCART_TEST_TARGET=linked), or on two PlayStations with PS1 DEV CART
// cartridges (=linkedcarts): two debug sessions, both stop at a breakpoint.
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

const ON_CARTS = process.env.PS1DEVCART_TEST_TARGET === 'linkedcarts';

// How many emulators run, or ps1transfer processes hold a cartridge
function running() {
    const image = ON_CARTS ? 'ps1transfer.exe' : 'pcsx-redux.main';
    const out = cp.execFileSync('tasklist', ['/FI', `IMAGENAME eq ${image}`, '/NH'], { encoding: 'utf8' });
    return out.split('\n').filter(l => l.includes(image)).length;
}

async function run() {
    const folder = vscode.workspace.workspaceFolders[0];
    const main = vscode.Uri.file(path.join(folder.uri.fsPath, 'main.c'));
    const line = fs.readFileSync(main.fsPath, 'utf8').split('\n').findIndex(l => l.includes('FntPrint('));
    assert.ok(line > 0, '"FntPrint(" line in main.c');

    const config = vscode.workspace.getConfiguration('ps1devcart');
    await vscode.extensions.getExtension('logi-26.ps1-dev-cart').activate();
    if (ON_CARTS) {
        await config.update('ps1transferPath', process.env.PS1DEVCART_TEST_PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
        await config.update('devcartLink', 'cable', vscode.ConfigurationTarget.Workspace);
        await vscode.commands.executeCommand('ps1devcart.selectCart', process.env.PS1DEVCART_TEST_DEVCART);
        await vscode.commands.executeCommand('ps1devcart.selectSecondCart', process.env.PS1DEVCART_TEST_DEVCART2);
    } else {
        await config.update('emulatorPath', process.env.PS1DEVCART_TEST_EMULATOR, vscode.ConfigurationTarget.Workspace);
    }
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(main, new vscode.Position(line, 0)))]);

    // The first stop of each session
    const stops = new Map();
    vscode.debug.registerDebugAdapterTrackerFactory('*', {
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
    await vscode.commands.executeCommand(ON_CARTS ? 'ps1devcart.debugLinkedOnCarts' : 'ps1devcart.debugLinked');
    const end = Date.now() + 180000;
    while (stops.size < 2 && Date.now() < end) {
        await new Promise(r => setTimeout(r, 500));
    }
    assert.strictEqual(stops.size, 2, 'both sessions stopped');
    for (const { session, body } of stops.values()) {
        const top = (await session.customRequest('stackTrace', { threadId: body.threadId || 1, levels: 1 })).stackFrames[0];
        console.log(`  ${session.name}: stopped in ${top.name} ${top.source && path.basename(top.source.path)}:${top.line}`);
        assert.ok(top.source && /main\.c$/i.test(top.source.path), 'stopped in main.c');
        assert.strictEqual(top.line, line + 1, 'at the breakpoint');
    }

    vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    for (const { session } of stops.values()) {
        await vscode.debug.stopDebugging(session);
    }
    for (let i = 0; i < 40 && running(); i++) {    // (ps1transfer first removes the breakpoints on the PS1)
        await new Promise(r => setTimeout(r, 500));
    }
    assert.strictEqual(running(), 0, (ON_CARTS ? 'ps1transfer' : 'emulators') + ' closed after debugging');
    console.log(`  ok    linked debug sessions${ON_CARTS ? ' on two PS1 DEV CARTs' : ' in two emulators'}: both stopped ` +
                `at main.c:${line + 1}, closed after`);
    if (ON_CARTS) {
        await config.update('devcartLink', undefined, vscode.ConfigurationTarget.Workspace);
    }
}

module.exports = { run };
