// Runs the extension's tests in a separate VS Code (downloaded into
// .vscode-test/), with a fresh copy of the project template as the workspace.
// Needs the build environment installed in WSL.
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const { runTests, downloadAndUnzipVSCode, resolveCliArgsFromVSCodeExecutablePath } = require('@vscode/test-electron');

// Debugging test: needs PCSX-Redux (PS1DEVCART_TEST_EMULATOR = its Windows path)
// and gdb-multiarch in WSL; the C/C++ extension is installed for it. On a
// PlayStation (target 'devcart'): a PS1 DEV CART (PS1DEVCART_TEST_DEVCART = its
// USB serial number) and ps1transfer (PS1DEVCART_TEST_PS1TRANSFER = its path).
async function debugTest(ext, target) {
    const emulator = process.env.PS1DEVCART_TEST_EMULATOR;
    const devcart = process.env.PS1DEVCART_TEST_DEVCART;
    const ps1transfer = process.env.PS1DEVCART_TEST_PS1TRANSFER;
    const devcart2 = process.env.PS1DEVCART_TEST_DEVCART2;
    const onCart = target === 'devcart' || target === 'linkedcarts';
    if (target === 'linkedcarts' && !devcart2) {
        console.log('\nLinked debugging on two PS1 DEV CARTs: skipped (set PS1DEVCART_TEST_DEVCART2 too)');
        return;
    }
    if (onCart && !(devcart && ps1transfer && fs.existsSync(ps1transfer))) {
        console.log('\nDebugging on a PS1 DEV CART: skipped (set PS1DEVCART_TEST_DEVCART and PS1DEVCART_TEST_PS1TRANSFER)');
        return;
    }
    if (!onCart && (!emulator || !fs.existsSync(emulator))) {
        console.log('\nDebugging: skipped (set PS1DEVCART_TEST_EMULATOR to the path of pcsx-redux.exe)');
        return;
    }
    console.log({ devcart: `\nDebugging on the PS1 DEV CART ${devcart}:`, linked: '\nLinked debugging in two PCSX-Redux:',
                  linkedcarts: `\nLinked debugging on the PS1 DEV CARTs ${devcart} and ${devcart2}:` }[target]
                || '\nDebugging in PCSX-Redux:');
    const vscodeExe = await downloadAndUnzipVSCode();
    const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExe);
    cp.spawnSync(cli, [...cliArgs, '--install-extension', 'ms-vscode.cpptools'], { stdio: 'ignore', shell: true });
    // PS1DEVCART_TEST_WORKSPACE: debug an existing project instead of the template
    const existing = process.env.PS1DEVCART_TEST_WORKSPACE;
    const workspace = existing || fs.mkdtempSync(path.join(os.tmpdir(), 'ps1devcart-debug-'));
    if (!existing) {
        for (const name of ['main.c', 'Makefile', 'auto']) {
            fs.copyFileSync(path.join(ext, 'template', name), path.join(workspace, name));
        }
    }
    try {
        await runTests({
            vscodeExecutablePath: vscodeExe,
            extensionDevelopmentPath: ext,
            extensionTestsPath: path.join(__dirname, 'suite', target === 'linked' || target === 'linkedcarts'
                ? 'debugLinked.js' : 'debug.js'),
            extensionTestsEnv: {
                PS1DEVCART_TEST_EMULATOR: emulator || '',
                PS1DEVCART_TEST_TARGET: target || '',
                PS1DEVCART_TEST_DEVCART: devcart || '',
                PS1DEVCART_TEST_DEVCART2: devcart2 || '',
                PS1DEVCART_TEST_PS1TRANSFER: ps1transfer || '',
                PS1DEVCART_TEST_LOG: process.env.PS1DEVCART_TEST_LOG || '',
                PS1DEVCART_TEST_BREAK: process.env.PS1DEVCART_TEST_BREAK || '',
            },
            launchArgs: [workspace],
        });
    } finally {
        // In case a failed test left the emulator running (ps1transfer ends by itself once GDB has gone)
        if (!onCart) {
            cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.main'], { stdio: 'ignore' });
            cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.exe'], { stdio: 'ignore' });
        }
        await new Promise(r => setTimeout(r, 1000));
        if (!existing) {
            fs.rmSync(workspace, { recursive: true, force: true });
        }
    }
}

// PsyQ / PSn00bSDK projects (test/suite/sdk.js): a PSX.Dev PsyQ project (PS1DEVCART_TEST_PSYQ = its folder, e.g. the
// PSX.Dev "cube" template) and the PSn00bSDK template (PS1DEVCART_TEST_PSN00BSDK = the PSn00bSDK folder), each copied
// to a short temporary folder (CMake builds need short paths); PS1DEVCART_TEST_DEVCART / _PS1TRANSFER and
// PS1DEVCART_TEST_EMULATOR for running and debugging.
async function sdkTests(ext) {
    const kinds = [
        { kind: 'make', from: process.env.PS1DEVCART_TEST_PSYQ, label: 'PsyQ project (PSX.Dev, nugget)' },
        { kind: 'cmake', label: 'PSn00bSDK project (CMake template)',
          from: process.env.PS1DEVCART_TEST_PSN00BSDK
              && path.join(process.env.PS1DEVCART_TEST_PSN00BSDK, 'share', 'psn00bsdk', 'template') },
    ];
    for (const { kind, from, label } of kinds) {
        if (!from || !fs.existsSync(from)) {
            console.log(`\n${label}: skipped (set ${kind === 'make' ? 'PS1DEVCART_TEST_PSYQ' : 'PS1DEVCART_TEST_PSN00BSDK'})`);
            continue;
        }
        console.log(`\n${label}:`);
        const vscodeExe = await downloadAndUnzipVSCode();
        const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExe);
        cp.spawnSync(cli, [...cliArgs, '--install-extension', 'ms-vscode.cpptools'], { stdio: 'ignore', shell: true });
        const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'nysdk-'));
        // (without what a previous build left)
        fs.cpSync(from, workspace, { recursive: true, filter: f => !/\.(o|dep|elf|ps-exe|map)$/i.test(f)
                                                                   && path.basename(f) !== 'build' });
        try {
            await runTests({
                vscodeExecutablePath: vscodeExe,
                extensionDevelopmentPath: ext,
                extensionTestsPath: path.join(__dirname, 'suite', 'sdk.js'),
                extensionTestsEnv: {
                    PS1DEVCART_TEST_KIND: kind,
                    PS1DEVCART_TEST_PSN00BSDK: process.env.PS1DEVCART_TEST_PSN00BSDK || '',
                    PS1DEVCART_TEST_DEVCART: process.env.PS1DEVCART_TEST_DEVCART || '',
                    PS1DEVCART_TEST_DEVCART2: process.env.PS1DEVCART_TEST_DEVCART2 || '',
                    PS1DEVCART_TEST_PS1TRANSFER: process.env.PS1DEVCART_TEST_PS1TRANSFER || '',
                    PS1DEVCART_TEST_EMULATOR: process.env.PS1DEVCART_TEST_EMULATOR || '',
                },
                launchArgs: [workspace],
            });
        } finally {
            cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.main'], { stdio: 'ignore' });
            cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.exe'], { stdio: 'ignore' });
            await new Promise(r => setTimeout(r, 1000));
            fs.rmSync(workspace, { recursive: true, force: true });
        }
    }
}

// Tools (test/suite/toolchains.js, PS1DEVCART_TEST_ONLY=tools): in a new profile (its own user data and storage), the
// extension downloads and installs PCSX-Redux, the MIPS toolchain, PSn00bSDK, CMake and Ninja (a few hundred MB), then
// builds a PsyQ project (PS1DEVCART_TEST_PSYQ) and the PSn00bSDK template with them, and debugs the latter in the
// PCSX-Redux installed. The PSn00bSDK template comes from PS1DEVCART_TEST_PSN00BSDK (only copied, not used to build).
async function toolsTests(ext) {
    const psyq = process.env.PS1DEVCART_TEST_PSYQ;
    const sdk = process.env.PS1DEVCART_TEST_PSN00BSDK;
    if (!psyq || !sdk) {
        console.log('\nTools: skipped (set PS1DEVCART_TEST_PSYQ and PS1DEVCART_TEST_PSN00BSDK)');
        return;
    }
    console.log('\nTools (installed by the extension in a new profile):');
    const vscodeExe = await downloadAndUnzipVSCode();
    const [cli, ...cliArgs] = resolveCliArgsFromVSCodeExecutablePath(vscodeExe);
    cp.spawnSync(cli, [...cliArgs, '--install-extension', 'ms-vscode.cpptools'], { stdio: 'ignore', shell: true });
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'nytools-'));
    const userData = fs.mkdtempSync(path.join(os.tmpdir(), 'nyprofile-'));
    const skipBuilt = f => !/\.(o|dep|elf|ps-exe|map)$/i.test(f) && path.basename(f) !== 'build';
    fs.cpSync(psyq, path.join(workspace, 'psyq'), { recursive: true, filter: skipBuilt });
    fs.cpSync(path.join(sdk, 'share', 'psn00bsdk', 'template'), path.join(workspace, 'psn00b'), { recursive: true });
    try {
        await runTests({
            vscodeExecutablePath: vscodeExe,
            extensionDevelopmentPath: ext,
            extensionTestsPath: path.join(__dirname, 'suite', 'toolchains.js'),
            launchArgs: [workspace, '--user-data-dir', userData],
        });
    } finally {
        cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.main'], { stdio: 'ignore' });
        cp.spawnSync('taskkill', ['/F', '/IM', 'pcsx-redux.exe'], { stdio: 'ignore' });
        await new Promise(r => setTimeout(r, 1000));
        fs.rmSync(workspace, { recursive: true, force: true });
        fs.rmSync(userData, { recursive: true, force: true });
    }
}

async function main() {
    // When started from a terminal inside VS Code, these would make the test
    // instance run as plain Node or talk to the running VS Code.
    for (const name of Object.keys(process.env)) {
        if (name === 'ELECTRON_RUN_AS_NODE' || name.startsWith('VSCODE_')) {
            delete process.env[name];
        }
    }
    const ext = path.join(__dirname, '..');
    // The project at the root of the opened folder, and in a subfolder
    // PS1DEVCART_TEST_ONLY=build or =debug runs only those tests
    if (process.env.PS1DEVCART_TEST_ONLY === 'tools') {
        await toolsTests(ext);
        return;
    }
    // PS1DEVCART_TEST_ONLY=sdk: only the PsyQ / PSn00bSDK tests
    if (process.env.PS1DEVCART_TEST_ONLY === 'sdk') {
        await sdkTests(ext);
        return;
    }
    const layouts = process.env.PS1DEVCART_TEST_ONLY === 'debug' ? [] : ['', 'demo'];
    for (const project of layouts) {
        console.log(`\nProject ${project ? 'in a subfolder (' + project + ')' : 'at the root of the folder'}:`);
        const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'ps1devcart-test-'));
        fs.mkdirSync(path.join(workspace, project), { recursive: true });
        for (const name of ['main.c', 'Makefile', 'auto', 'iso.xml', 'system.cnf']) {
            fs.copyFileSync(path.join(ext, 'template', name), path.join(workspace, project, name));
        }
        try {
            await runTests({
                extensionDevelopmentPath: ext,
                extensionTestsPath: path.join(__dirname, 'suite', 'index.js'),
                extensionTestsEnv: {
                    PS1DEVCART_TEST_PROJECT: project,
                    PS1DEVCART_TEST_DEVCART: process.env.PS1DEVCART_TEST_DEVCART || '',
                    PS1DEVCART_TEST_PS1TRANSFER: process.env.PS1DEVCART_TEST_PS1TRANSFER || '',
                    PS1DEVCART_TEST_DEVCART2: process.env.PS1DEVCART_TEST_DEVCART2 || '',
                },
                launchArgs: [workspace, '--disable-extensions'],
            });
        } finally {
            fs.rmSync(workspace, { recursive: true, force: true });
        }
    }
    if (!process.env.PS1DEVCART_TEST_ONLY) {
        await sdkTests(ext);
    }
    if (process.env.PS1DEVCART_TEST_ONLY !== 'build') {
        // PS1DEVCART_TEST_DEBUG=emulator, devcart, linked or linkedcarts runs only that one
        const only = process.env.PS1DEVCART_TEST_DEBUG;
        for (const target of [undefined, 'devcart', 'linked', 'linkedcarts']) {
            if (!only || only === (target || 'emulator')) {
                await debugTest(ext, target);
            }
        }
    }
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
