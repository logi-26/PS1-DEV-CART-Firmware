// Extension tests, run inside VS Code by runTest.js. Each test runs in order;
// the workspace is a copy of the project template.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const vscode = require('vscode');
const cp = require('child_process');

const tests = [];
function test(name, fn) {
    tests.push({ name, fn });
}

// The project is at the root of the workspace folder, or in a subfolder
const PROJECT = process.env.PS1DEVCART_TEST_PROJECT || '';
const SUFFIX = PROJECT ? ` (${PROJECT})` : '';
const folder = () => vscode.workspace.workspaceFolders[0];
const ws = (...p) => path.join(folder().uri.fsPath, PROJECT, ...p);

async function waitFor(check, ms = 15000) {
    const end = Date.now() + ms;
    for (;;) {
        const value = check();
        if (value) {
            return value;
        }
        if (Date.now() > end) {
            return value;
        }
        await new Promise(r => setTimeout(r, 200));
    }
}

// A small indexed PNG (4x2, 3 colours), made here so the test needs no files.
function makePng(file) {
    const crcTable = Array.from({ length: 256 }, (_, n) => {
        let c = n;
        for (let k = 0; k < 8; k++) {
            c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        }
        return c >>> 0;
    });
    const crc = buf => {
        let c = 0xffffffff;
        for (const b of buf) {
            c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
        }
        return (c ^ 0xffffffff) >>> 0;
    };
    const chunk = (type, data) => {
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const body = Buffer.concat([Buffer.from(type), data]);
        const sum = Buffer.alloc(4);
        sum.writeUInt32BE(crc(body));
        return Buffer.concat([len, body, sum]);
    };
    const ihdr = Buffer.from([0, 0, 0, 4, 0, 0, 0, 2, 8, 3, 0, 0, 0]);
    const plte = Buffer.from([0, 0, 0, 255, 255, 255, 200, 40, 40]);
    const rows = Buffer.from([0, 0, 1, 2, 1, 0, 2, 2, 1, 0]);
    fs.writeFileSync(file, Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr), chunk('PLTE', plte), chunk('IDAT', zlib.deflateSync(rows)), chunk('IEND', Buffer.alloc(0)),
    ]));
}

test('activates and registers its commands', async () => {
    const ext = vscode.extensions.getExtension('logi-26.ps1-dev-cart');
    assert.ok(ext, 'extension found');
    await ext.activate();
    const commands = await vscode.commands.getCommands(true);
    for (const c of ['build', 'clean', 'rebuild', 'run', 'buildModels', 'buildIso', 'runIso', 'configureIntelliSense',
                     'newProject', 'convertPngToTim', 'runLinked', 'debugLinked', 'runOnPs1', 'debugOnPs1',
                     'runLinkedOnCarts', 'debugLinkedOnCarts', 'resetCart', 'selectCart', 'selectSecondCart',
                     'refreshCarts']) {
        assert.ok(commands.includes('ps1devcart.' + c), 'command ps1devcart.' + c);
    }
});

test('provides the tasks', async () => {
    const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
    assert.deepStrictEqual(tasks.map(t => t.name).sort(),
                           ['Build', 'Build CD Image', 'Build Models', 'Clean', 'Rebuild', 'Run', 'Run CD Image'].map(n => n + SUFFIX));
    const build = tasks.find(t => t.name === 'Build' + SUFFIX);
    assert.strictEqual(build.execution.options.cwd.toLowerCase(), ws().toLowerCase(), 'runs in the project folder');
    assert.strictEqual(build.source, 'Net Yaroze');
    assert.strictEqual(build.group && build.group.id, vscode.TaskGroup.Build.id);
    assert.deepStrictEqual(build.problemMatchers, ['$ps1devcart-gcc']);
});

test('build makes main.exe and psx.exe', async () => {
    const code = await vscode.commands.executeCommand('ps1devcart.build');
    assert.strictEqual(code, 0);
    assert.ok(fs.existsSync(ws('main.exe')), 'main.exe');
    assert.ok(fs.existsSync(ws('psx.exe')), 'psx.exe');
});

test('clean deletes them', async () => {
    const code = await vscode.commands.executeCommand('ps1devcart.clean');
    assert.strictEqual(code, 0);
    assert.ok(!fs.existsSync(ws('main.exe')) && !fs.existsSync(ws('psx.exe')));
});

test('rebuild and models work', async () => {
    assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.rebuild'), 0);
    assert.ok(fs.existsSync(ws('psx.exe')), 'psx.exe after rebuild');
    assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.buildModels'), 0);
});

test('compiler errors and warnings go to the Problems panel', async () => {
    const original = fs.readFileSync(ws('main.c'), 'utf8');
    const broken = original + '\nint broken(void)\n{\n\tchar *p = 5;\n\treturn undefined_name;\n}\n';
    const warningLine = broken.split('\n').indexOf('\tchar *p = 5;');
    const errorLine = warningLine + 1;
    fs.writeFileSync(ws('main.c'), broken);
    try {
        const code = await vscode.commands.executeCommand('ps1devcart.build');
        assert.notStrictEqual(code, 0, 'build fails');
        const uri = vscode.Uri.file(ws('main.c'));
        const diags = await waitFor(() => {
            const d = vscode.languages.getDiagnostics(uri);
            return d.length >= 2 ? d : null;
        });
        assert.ok(diags, 'diagnostics for main.c');
        const error = diags.find(d => d.severity === vscode.DiagnosticSeverity.Error && /undefined_name/.test(d.message));
        const warning = diags.find(d => d.severity === vscode.DiagnosticSeverity.Warning);
        assert.ok(error && /undefined_name/.test(error.message), 'error: ' + (error && error.message));
        assert.strictEqual(error.range.start.line, errorLine);
        assert.ok(warning, 'warning');
        assert.strictEqual(warning.range.start.line, warningLine);
    } finally {
        fs.writeFileSync(ws('main.c'), original);
    }
});

test('run passes the emulator setting to make as a WSL path', async () => {
    await vscode.workspace.getConfiguration('ps1devcart')
        .update('emulatorPath', 'C:\\Emulators\\PCSX Redux\\pcsx-redux.exe', vscode.ConfigurationTarget.Workspace);
    try {
        const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
        const run = tasks.find(t => t.name === 'Run' + SUFFIX);
        assert.deepStrictEqual(run.execution.args.slice(-3),
                               ['make', 'run', 'PCSX_REDUX=/mnt/c/Emulators/PCSX Redux/pcsx-redux.exe']);
    } finally {
        await vscode.workspace.getConfiguration('ps1devcart')
            .update('emulatorPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

test('Run Linked is offered when the makefile has a link target', async () => {
    const makefile = ws('Makefile');
    const original = fs.readFileSync(makefile, 'utf8');
    await vscode.workspace.getConfiguration('ps1devcart')
        .update('emulatorPath', 'C:\\Emulators\\pcsx-redux.exe', vscode.ConfigurationTarget.Workspace);
    fs.writeFileSync(makefile, original + '\nlink: psx.exe\n\techo linked "$(PCSX_REDUX)"\n');
    try {
        const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
        const link = tasks.find(t => t.name === 'Run Linked' + SUFFIX);
        assert.ok(link, 'Run Linked task');
        assert.deepStrictEqual(link.execution.args.slice(-3),
                               ['make', 'link', 'PCSX_REDUX=/mnt/c/Emulators/pcsx-redux.exe']);
    } finally {
        fs.writeFileSync(makefile, original);
        await vscode.workspace.getConfiguration('ps1devcart')
            .update('emulatorPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
    const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
    assert.ok(!tasks.some(t => t.name === 'Run Linked' + SUFFIX), 'not offered without the target');
});

test('Run CD Image passes the emulator setting to make run-iso', async () => {
    await vscode.workspace.getConfiguration('ps1devcart')
        .update('emulatorPath', 'C:\\Emulators\\pcsx-redux.exe', vscode.ConfigurationTarget.Workspace);
    try {
        const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
        const runIso = tasks.find(t => t.name === 'Run CD Image' + SUFFIX);
        assert.deepStrictEqual(runIso.execution.args.slice(-3),
                               ['make', 'run-iso', 'PCSX_REDUX=/mnt/c/Emulators/pcsx-redux.exe']);
        const iso = tasks.find(t => t.name === 'Build CD Image' + SUFFIX);
        assert.deepStrictEqual(iso.execution.args.slice(-2), ['make', 'iso']);
    } finally {
        await vscode.workspace.getConfiguration('ps1devcart')
            .update('emulatorPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

test('Build CD Image makes game.bin and game.cue', async () => {
    // Needs mkpsxiso (toolchain/install-mkpsxiso.sh)
    if (cp.spawnSync('wsl', ['-e', 'test', '-x', '/opt/yaroze/bin/mkpsxiso']).status !== 0) {
        console.log('        (skipped: mkpsxiso is not installed in /opt/yaroze/bin)');
        return;
    }
    assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.buildIso'), 0);
    assert.ok(fs.existsSync(ws('game.bin')) && fs.existsSync(ws('game.cue')), 'game.bin and game.cue');
    assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.clean'), 0);
    assert.ok(!fs.existsSync(ws('game.bin')), 'clean deletes the image');
});

test('CD image commands are not offered without the targets', async () => {
    const makefile = ws('Makefile');
    const original = fs.readFileSync(makefile, 'utf8');
    fs.writeFileSync(makefile, original.replace(/^(iso|run-iso):/gm, '$1-off:'));
    try {
        const tasks = await vscode.tasks.fetchTasks({ type: 'ps1devcart' });
        assert.ok(!tasks.some(t => /CD Image/.test(t.name)), 'no CD image tasks');
    } finally {
        fs.writeFileSync(makefile, original);
    }
});

test('Choose Emulator saves the emulator setting', async () => {
    const config = () => vscode.workspace.getConfiguration('ps1devcart');
    const chosen = await vscode.commands.executeCommand('ps1devcart.chooseEmulator', 'C:\\Emulators\\pcsx-redux.exe');
    try {
        assert.strictEqual(chosen, 'C:\\Emulators\\pcsx-redux.exe');
        assert.strictEqual(config().inspect('emulatorPath').globalValue, 'C:\\Emulators\\pcsx-redux.exe');
    } finally {
        await config().update('emulatorPath', undefined, vscode.ConfigurationTarget.Global);
    }
});

test('configures IntelliSense with the SDK headers', async () => {
    const file = await vscode.commands.executeCommand('ps1devcart.configureIntelliSense', folder());
    assert.ok(file && fs.existsSync(file), 'c_cpp_properties.json');
    const config = JSON.parse(fs.readFileSync(file, 'utf8')).configurations.find(c => c.name === 'Net Yaroze');
    assert.ok(config, 'Net Yaroze configuration');
    // The Windows path of /opt/yaroze/mipsel-unknown-ecoff/include in WSL
    assert.ok(/^\\\\wsl(\.localhost|\$)\\[^\\]+\\opt\\yaroze\\mipsel-unknown-ecoff\\include$/.test(config.includePath[1]),
              'include path: ' + config.includePath[1]);
    assert.ok(config.defines.includes('__mips__=1'), 'defines: ' + config.defines.join(' '));
    assert.ok(config.defines.includes('__GNUC__=2'));
});

test('creates a new project', async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ps1devcart-new-'));
    try {
        const created = await vscode.commands.executeCommand('ps1devcart.newProject', vscode.Uri.file(dir));
        // (VS Code writes Windows drive letters in lower case)
        assert.strictEqual(created && created.toLowerCase(), dir.toLowerCase());
        for (const name of ['main.c', 'Makefile', 'auto', 'iso.xml', 'system.cnf', '.gitignore',
                            '.vscode/launch.json', '.vscode/settings.json']) {
            assert.ok(fs.existsSync(path.join(dir, name)), name);
        }
        // Refuses to overwrite
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.newProject', vscode.Uri.file(dir)), undefined);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});

// PS1 DEV CART: Run on PS1 needs a cartridge (PS1DEVCART_TEST_DEVCART = its USB serial number, ps1transfer -l) and
// ps1transfer (PS1DEVCART_TEST_PS1TRANSFER = its path); otherwise skipped
const DEVCART = process.env.PS1DEVCART_TEST_DEVCART;
const PS1TRANSFER = process.env.PS1DEVCART_TEST_PS1TRANSFER;

test('Select PS1 DEV CART saves the cartridge', async () => {
    const before = vscode.workspace.getConfiguration('ps1devcart').inspect('devcartSerial').globalValue;
    try {
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.selectCart', 'A10TEST1'), 'A10TEST1');
        assert.strictEqual(vscode.workspace.getConfiguration('ps1devcart').get('devcartSerial'), 'A10TEST1');
    } finally {
        await vscode.workspace.getConfiguration('ps1devcart').update('devcartSerial', before, vscode.ConfigurationTarget.Global);
    }
});

test('Run on PS1 builds and runs psx.exe on the PS1 DEV CART', async () => {
    if (!DEVCART || !PS1TRANSFER) {
        console.log('        (skipped: set PS1DEVCART_TEST_DEVCART and PS1DEVCART_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('ps1devcart');
    await config.update('ps1transferPath', PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
    await config.update('devcartTty', 'bios', vscode.ConfigurationTarget.Workspace);  // (no console: ps1transfer ends)
    await vscode.commands.executeCommand('ps1devcart.selectCart', DEVCART);
    try {
        const execution = await vscode.commands.executeCommand('ps1devcart.runOnPs1');
        assert.ok(execution, 'task started');
        assert.ok(execution.task.name.includes(DEVCART), 'task name: ' + execution.task.name);
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
        assert.ok(/state=running/.test(info), 'the game runs on the console: ' + info.replace(/\s+/g, ' '));
    } finally {
        await config.update('devcartTty', undefined, vscode.ConfigurationTarget.Workspace);
        await config.update('ps1transferPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

test('Reset works while Run on PS1 keeps the cartridge open (its console terminal stopped first)', async () => {
    if (!DEVCART || !PS1TRANSFER) {
        console.log('        (skipped: set PS1DEVCART_TEST_DEVCART and PS1DEVCART_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('ps1devcart');
    await config.update('ps1transferPath', PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
    await vscode.commands.executeCommand('ps1devcart.selectCart', DEVCART);
    try {
        const execution = await vscode.commands.executeCommand('ps1devcart.runOnPs1');
        assert.ok(execution, 'task started');
        let ended = false;
        const listener = vscode.tasks.onDidEndTaskProcess(e => { ended = ended || e.execution === execution; });
        await new Promise(r => setTimeout(r, 8000));    // (built, uploaded: the console runs)
        assert.ok(!ended, 'the console terminal is still open (it holds the cartridge)');
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.resetCart'), 0, 'reset exit code');
        await new Promise(r => setTimeout(r, 1000));
        listener.dispose();
        assert.ok(ended, 'the console terminal was stopped');
    } finally {
        await config.update('ps1transferPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

test('Select Second PS1 DEV CART saves the cartridge', async () => {
    const before = vscode.workspace.getConfiguration('ps1devcart').inspect('devcartSerial2').globalValue;
    try {
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.selectSecondCart', 'A10TEST2'), 'A10TEST2');
        assert.strictEqual(vscode.workspace.getConfiguration('ps1devcart').get('devcartSerial2'), 'A10TEST2');
    } finally {
        await vscode.workspace.getConfiguration('ps1devcart').update('devcartSerial2', before, vscode.ConfigurationTarget.Global);
    }
});

// Link play on two cartridges: PS1DEVCART_TEST_DEVCART2 = the second one's USB serial number
const DEVCART2 = process.env.PS1DEVCART_TEST_DEVCART2;

test('the cartridges connected are listed (PS1 DEV CART panel)', async () => {
    if (!DEVCART || !PS1TRANSFER) {
        console.log('        (skipped: set PS1DEVCART_TEST_DEVCART and PS1DEVCART_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('ps1devcart');
    await config.update('ps1transferPath', PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
    try {
        const carts = await vscode.commands.executeCommand('ps1devcart.refreshCarts');
        assert.ok(carts.some(c => c.serial === DEVCART), 'listed: ' + JSON.stringify(carts));
        if (DEVCART2) {
            assert.ok(carts.some(c => c.serial === DEVCART2), 'second cartridge listed: ' + JSON.stringify(carts));
        }
    } finally {
        await config.update('ps1transferPath', undefined, vscode.ConfigurationTarget.Workspace);
    }
});

test('Run Linked on Two PS1s runs psx.exe on both, Reset resets them', async () => {
    if (!DEVCART || !DEVCART2 || !PS1TRANSFER) {
        console.log('        (skipped: set PS1DEVCART_TEST_DEVCART, PS1DEVCART_TEST_DEVCART2 and PS1DEVCART_TEST_PS1TRANSFER)');
        return;
    }
    const config = vscode.workspace.getConfiguration('ps1devcart');
    const before = [config.inspect('devcartSerial').globalValue, config.inspect('devcartSerial2').globalValue];
    await config.update('ps1transferPath', PS1TRANSFER, vscode.ConfigurationTarget.Workspace);
    await config.update('devcartLink', 'cable', vscode.ConfigurationTarget.Workspace);
    await vscode.commands.executeCommand('ps1devcart.selectCart', DEVCART);
    await vscode.commands.executeCommand('ps1devcart.selectSecondCart', DEVCART2);
    const state = serial => /state=(\w+)/.exec(cp.execFileSync(PS1TRANSFER, ['-n', serial, '-info'], { encoding: 'utf8' }))[1];
    try {
        const codes = await vscode.commands.executeCommand('ps1devcart.runLinkedOnCarts');
        assert.deepStrictEqual(codes, [0, 0], 'ps1transfer exit codes');
        await new Promise(r => setTimeout(r, 2000));
        assert.strictEqual(state(DEVCART), 'running', DEVCART);
        assert.strictEqual(state(DEVCART2), 'running', DEVCART2);
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.resetCart'), 0, 'reset');
        assert.strictEqual(await vscode.commands.executeCommand('ps1devcart.resetCart', DEVCART2), 0, 'reset second');
        await new Promise(r => setTimeout(r, 3000));
        assert.strictEqual(state(DEVCART), 'menu', DEVCART + ' after the reset');
        assert.strictEqual(state(DEVCART2), 'menu', DEVCART2 + ' after the reset');
    } finally {
        await config.update('devcartLink', undefined, vscode.ConfigurationTarget.Workspace);
        await config.update('ps1transferPath', undefined, vscode.ConfigurationTarget.Workspace);
        await config.update('devcartSerial', before[0], vscode.ConfigurationTarget.Global);
        await config.update('devcartSerial2', before[1], vscode.ConfigurationTarget.Global);
    }
});

test('converts a PNG to TIM', async () => {
    const png = ws('test.png');
    makePng(png);
    const tim = await vscode.commands.executeCommand('ps1devcart.convertPngToTim', vscode.Uri.file(png),
                                                     { org: ['640', '0'], plt: ['0', '480'] });
    assert.strictEqual(tim, ws('test.tim'));
    const d = fs.readFileSync(tim);
    assert.strictEqual(d.readUInt32LE(0), 0x10, 'TIM magic');
    assert.strictEqual(d.readUInt32LE(4), 0x08, '4bpp with CLUT');
    assert.deepStrictEqual([d.readUInt16LE(12), d.readUInt16LE(14)], [0, 480], 'CLUT position');
    const img = 8 + d.readUInt32LE(8);
    assert.deepStrictEqual([d.readUInt16LE(img + 4), d.readUInt16LE(img + 6)], [640, 0], 'image position');
    // Same settings as the existing TIM gives the same file
    const first = Buffer.from(d);
    await vscode.commands.executeCommand('ps1devcart.convertPngToTim', vscode.Uri.file(png), { like: true });
    assert.ok(first.equals(fs.readFileSync(tim)), '--like gives the same TIM');
});

async function run() {
    const failures = [];
    for (const t of tests) {
        try {
            await t.fn();
            console.log('  ok    ' + t.name);
        } catch (e) {
            console.log('  FAIL  ' + t.name + '\n        ' + (e && e.message));
            failures.push(t.name);
        }
    }
    console.log(`${tests.length - failures.length}/${tests.length} tests passed`);
    if (failures.length) {
        throw new Error(failures.length + ' test(s) failed');
    }
}

module.exports = { run };
