// PS1 DEV CART extension: the tools the projects need, found where they are
// installed (the settings, PSX.Dev's, the PATH) or downloaded and installed by
// the extension in its own storage (globalStorage), like the PSX.Dev extension
// does: PCSX-Redux, the MIPS toolchain (GCC, make, gdb-multiarch), PSn00bSDK,
// CMake and Ninja (Windows). ps1transfer comes with the extension. The Net
// Yaroze build environment is installed in WSL: the toolchain that comes with
// the extension (yaroze/), the SDK from the Net Yaroze Software Development
// Disc (downloaded, read by yarozesdk.js), yarexe (built in WSL).
//
// Downloads use Node's https (no extra package); zips are unpacked with
// Windows' tar.exe.

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const https = require('https');
const path = require('path');

const onWindows = process.platform === 'win32';

// The same archives as the PSX.Dev extension
const MIPS_VERSION = '16.2.0';
const MIPS_URL = `https://static.grumpycoder.net/pixel/mips/g++-mipsel-none-elf-${MIPS_VERSION}.zip`;
const GDB_URL = 'https://static.grumpycoder.net/pixel/gdb-multiarch-windows/gdb-multiarch-17.2.zip';
const REDUX_MANIFESTS = 'https://distrib.app/storage/manifests/pcsx-redux/dev-win-cli-x64/';
const VC_REDIST_URL = 'https://aka.ms/vs/17/release/vc_redist.x64.exe';
const NET_YAROZE_SDK_URL = 'https://archive.org/download/ps1_sdks/' +
    'Net%20Yaroze%20Software%20Development%20Disc%20%28USA%2C%20Europe%29_redump.zip';

let context;
let host;       // { settings, output, runLinux, toLinuxPath } from extension.js

function init(extensionContext, helpers) {
    context = extensionContext;
    host = helpers;
}

const storage = (...p) => path.join(context.globalStorageUri.fsPath, ...p);
const exists = p => !!p && fs.existsSync(p);

// ---------------------------------------------------------------------------
// Where each tool is
// ---------------------------------------------------------------------------

// PSX.Dev's own storage, next to this extension's (its PCSX-Redux)
const psxDevStorage = (...p) => path.join(context.globalStorageUri.fsPath, '..', 'grumpycoders.psx-dev', ...p);

// The emulator for every kind of project: the setting, else PCSX-Redux installed by this extension or by PSX.Dev
function emulatorPath() {
    const set = host.settings().get('emulatorPath');
    if (set) {
        return set;
    }
    if (!onWindows) {
        return '';
    }
    return [storage('pcsx-redux', 'pcsx-redux.exe'), psxDevStorage('pcsx-redux', 'pcsx-redux.exe')].find(exists) || '';
}

// The MIPS toolchain's bin folder (mipsel-none-elf GCC, make, gdb-multiarch): the setting, PSX.Dev's (its setting,
// or %APPDATA%\mips of its earlier versions), or the one installed by this extension
function mipsToolchain() {
    const set = host.settings().get('mipsToolchainPath');
    if (set) {
        return set;
    }
    const psxDev = vscode.workspace.getConfiguration('psxDev').get('toolchainPath');
    const candidates = [storage('mips', MIPS_VERSION, 'bin'), psxDev && path.join(psxDev, 'bin'),
                        process.env.APPDATA && path.join(process.env.APPDATA, 'mips', 'mips', 'bin')];
    return candidates.find(dir => exists(dir) && exists(path.join(dir, onWindows ? 'make.exe' : 'make'))) || '';
}

// PSn00bSDK's folder (bin, lib/libpsn00b): the setting, the one installed by this extension, or PSN00BSDK_LIBS
function psn00bsdkPath() {
    const set = host.settings().get('psn00bsdkPath');
    if (set) {
        return set;
    }
    const installed = firstDir(storage('psn00bsdk'));
    if (installed && exists(path.join(installed, 'lib', 'libpsn00b', 'cmake', 'sdk.cmake'))) {
        return installed;
    }
    const libs = process.env.PSN00BSDK_LIBS;
    return libs ? path.resolve(libs, '..', '..') : '';
}

function cmakePath() {
    const set = host.settings().get('cmakePath');
    if (set) {
        return set;
    }
    const installed = firstDir(storage('cmake'));
    const candidates = [installed && path.join(installed, 'bin', 'cmake.exe'),
                        path.join(process.env.ProgramFiles || 'C:\\Program Files', 'CMake', 'bin', 'cmake.exe')];
    return (onWindows && candidates.find(exists)) || 'cmake';
}

// Ninja's folder (CMake finds it in the PATH): the setting, the one installed by this extension, else the PATH
function ninjaDir() {
    const set = host.settings().get('ninjaPath');
    if (set) {
        return fs.existsSync(set) && fs.statSync(set).isFile() ? path.dirname(set) : set;
    }
    return exists(storage('ninja', 'ninja.exe')) ? storage('ninja') : '';
}

// ps1transfer: the setting, else the one that comes with the extension
function ps1transferBundled() {
    const exe = path.join(context.extensionPath, 'ps1transfer', onWindows ? 'ps1transfer.exe' : 'ps1transfer');
    return exists(exe) ? exe : '';
}

function firstDir(dir) {
    try {
        const sub = fs.readdirSync(dir).map(n => path.join(dir, n)).find(p => fs.statSync(p).isDirectory());
        return sub || '';
    } catch (e) {
        return '';
    }
}

// Is a program in the PATH (or the given path) and runs?
function runs(command, args) {
    return new Promise(resolve => {
        cp.execFile(command, args, { windowsHide: true, timeout: 15000 }, err => resolve(!err));
    });
}

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

function httpsGet(url, onResponse, redirects = 0) {
    return new Promise((resolve, reject) => {
        const req = https.get(url, { headers: { 'User-Agent': 'ps1-dev-cart-vscode' } }, res => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location && redirects < 8) {
                res.resume();
                resolve(httpsGet(new URL(res.headers.location, url).toString(), onResponse, redirects + 1));
                return;
            }
            if (res.statusCode !== 200) {
                res.resume();
                reject(new Error(`${url}: HTTP ${res.statusCode}`));
                return;
            }
            onResponse(res, resolve, reject);
        });
        req.on('error', reject);
    });
}

function getJson(url) {
    return httpsGet(url, (res, resolve, reject) => {
        let text = '';
        res.setEncoding('utf8');
        res.on('data', d => { text += d; });
        res.on('end', () => {
            try {
                resolve(JSON.parse(text));
            } catch (e) {
                reject(e);
            }
        });
    });
}

// Downloads a file, the progress shown in the notification (progress: its reporter)
function download(url, file, progress, label) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    host.output.appendLine(`Downloading ${url}`);
    return httpsGet(url, (res, resolve, reject) => {
        const total = Number(res.headers['content-length']) || 0;
        let done = 0;
        let shown = 0;
        const out = fs.createWriteStream(file);
        res.on('data', d => {
            done += d.length;
            const pct = total ? Math.floor(done * 100 / total) : 0;
            if (progress && pct >= shown + 2) {
                progress.report({ message: `${label}: ${pct}% of ${Math.round(total / 1048576)} MB` });
                shown = pct;
            }
        });
        res.pipe(out);
        out.on('finish', () => resolve(file));
        out.on('error', reject);
        res.on('error', reject);
    });
}

// Unpacks a zip into a folder (Windows' tar.exe reads zips)
function extract(zip, dir) {
    fs.mkdirSync(dir, { recursive: true });
    const tar = onWindows ? path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe') : 'unzip';
    const args = onWindows ? ['-xf', zip, '-C', dir] : ['-o', zip, '-d', dir];
    return new Promise((resolve, reject) => {
        cp.execFile(tar, args, { windowsHide: true, maxBuffer: 1 << 26 }, err => (err ? reject(err) : resolve(dir)));
    });
}

// Downloads and unpacks a zip into a folder (emptied first)
async function install(url, dir, progress, label, keep) {
    const zip = storage('downloads', decodeURIComponent(url.split('/').pop().split('?')[0]));
    await download(url, zip, progress, label);
    progress.report({ message: `${label}: unpacking...` });
    if (!keep) {
        fs.rmSync(dir, { recursive: true, force: true });
    }
    await extract(zip, dir);
    fs.rmSync(zip, { force: true });
}

// The download of the latest GitHub release asset whose name matches
// (the GitHub API; when it refuses, e.g. its limit of 60 requests an hour without an account is reached: the latest
// release's tag, from the redirect of github.com/.../releases/latest, and nameOf(version) the asset's name)
async function latestAsset(owner, repo, pattern, nameOf) {
    let release;
    try {
        release = await getJson(`https://api.github.com/repos/${owner}/${repo}/releases/latest`);
    } catch (e) {
        if (!nameOf) {
            throw e;
        }
        const tag = await latestTag(owner, repo);
        host.output.appendLine(`GitHub API: ${e.message}; ${owner}/${repo} latest release: ${tag}`);
        return `https://github.com/${owner}/${repo}/releases/download/${tag}/${nameOf(tag.replace(/^v/, ''))}`;
    }
    const asset = (release.assets || []).find(a => pattern.test(a.name));
    if (!asset) {
        throw new Error(`no ${pattern} in the latest ${owner}/${repo} release`);
    }
    return asset.browser_download_url;
}

// The tag of the latest release of a GitHub repo (the page it redirects to)
function latestTag(owner, repo) {
    const url = `https://github.com/${owner}/${repo}/releases/latest`;
    return new Promise((resolve, reject) => {
        https.get(url, { headers: { 'User-Agent': 'ps1-dev-cart-vscode' } }, res => {
            res.resume();
            const m = /\/releases\/tag\/([^/?#]+)/.exec(res.headers.location || '');
            if (m) {
                resolve(decodeURIComponent(m[1]));
            } else {
                reject(new Error(`${url}: no latest release (HTTP ${res.statusCode})`));
            }
        }).on('error', reject);
    });
}

// ---------------------------------------------------------------------------
// The Net Yaroze build environment (WSL)
// ---------------------------------------------------------------------------

// The SDK files of the Net Yaroze Software Development Disc (PSX/INCLUDE, PSX/LIB), downloaded once
async function netYarozeSdk(progress) {
    const dir = storage('yaroze-sdk');
    if (exists(path.join(dir, 'PSX', 'LIB', 'LIBPS.A'))) {
        return dir;
    }
    const zip = storage('downloads', 'net-yaroze-sdk-disc.zip');
    const unpacked = storage('downloads', 'net-yaroze-sdk-disc');
    await download(NET_YAROZE_SDK_URL, zip, progress, 'Net Yaroze Software Development Disc');
    try {
        progress.report({ message: 'Net Yaroze SDK: unpacking the disc image...' });
        fs.rmSync(unpacked, { recursive: true, force: true });
        await extract(zip, unpacked);
        const image = fs.readdirSync(unpacked).find(n => /\.(bin|iso)$/i.test(n));
        if (!image) {
            throw new Error('no disc image (.bin) in the download');
        }
        progress.report({ message: 'Net Yaroze SDK: reading the disc...' });
        const count = require('./yarozesdk').extractSdk(path.join(unpacked, image), dir);
        host.output.appendLine(`Net Yaroze SDK: ${count} files from ${image}`);
    } finally {
        fs.rmSync(zip, { force: true });
        fs.rmSync(unpacked, { recursive: true, force: true });
    }
    return dir;
}

// Installs it in /opt/yaroze: yaroze/install.sh, run with sudo in a terminal (it asks for the WSL password); resolves
// when the terminal is closed (it closes by itself when the install worked)
async function installNetYaroze(progress) {
    try {
        await host.runLinux(['true']);
    } catch (e) {
        throw new Error('WSL is needed for Net Yaroze projects: install it first (wsl --install, in an administrator ' +
                        'command prompt), then install the Net Yaroze build environment again');
    }
    const sdk = await netYarozeSdk(progress);
    const own = path.join(context.extensionPath, 'yaroze');
    const [script, tarball, sdkLinux] = await Promise.all(
        [path.join(own, 'install.sh'), path.join(own, 'yaroze-toolchain.tar.gz'), sdk].map(p => host.toLinuxPath(p)));
    progress.report({ message: 'Net Yaroze: installing in WSL, in the terminal (it asks for your WSL password)...' });
    const distro = host.settings().get('wslDistribution');
    const q = s => "'" + s.replace(/'/g, "'\\''") + "'";
    const term = vscode.window.createTerminal({
        name: 'Install Net Yaroze (WSL)', shellPath: 'wsl.exe',
        shellArgs: [...(distro ? ['-d', distro] : []), '-e', 'sh', '-c',
                    `sudo sh ${q(script)} ${q(tarball)} ${q(sdkLinux)} || ` +
                    '{ echo; echo "The install failed: see above. Press Enter to close this terminal."; read x; exit 1; }'],
    });
    term.show();
    const code = await new Promise(resolve => {
        const sub = vscode.window.onDidCloseTerminal(t => {
            if (t === term) {
                sub.dispose();
                resolve(t.exitStatus ? t.exitStatus.code : undefined);
            }
        });
    });
    if (code) {
        throw new Error('the install in WSL failed (see its terminal)');
    }
}

// ---------------------------------------------------------------------------
// The tools
// ---------------------------------------------------------------------------

async function wslHas(test) {
    try {
        await host.runLinux(['sh', '-c', test]);
        return true;
    } catch (e) {
        return false;
    }
}

// kinds: the projects needing it (yaroze, make, cmake; all: every one), optional: not needed to build
const TOOLS = [
    {
        id: 'redux', name: 'PCSX-Redux', kinds: ['all'], optional: true,
        what: 'The emulator: Run, Run Linked and Debug in the emulator, for every kind of project.',
        where: emulatorPath,
        check: async () => exists(emulatorPath()),
        install: async progress => {
            const runtime = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'msvcp140_atomic_wait.dll');
            if (!exists(runtime)) {     // (PCSX-Redux needs the Visual C++ runtime; its installer asks to run as admin)
                const vc = storage('downloads', 'vc_redist.x64.exe');
                await download(VC_REDIST_URL, vc, progress, 'Visual C++ runtime');
                progress.report({ message: 'Visual C++ runtime: installing...' });
                await new Promise(resolve => cp.execFile(vc, ['/install', '/passive', '/norestart'], () => resolve()));
            }
            const catalog = await getJson(REDUX_MANIFESTS + 'manifest.json');
            const id = Math.max(...catalog.builds.map(b => b.id));
            const build = await getJson(REDUX_MANIFESTS + `manifest-${id}.json`);
            await install('https://distrib.app' + build.path, storage('pcsx-redux'), progress, 'PCSX-Redux');
        },
    },
    {
        id: 'mips', name: 'MIPS toolchain (GCC, make, gdb-multiarch)', kinds: ['make'],
        what: 'Builds PsyQ / PsyQo / nugget projects; its gdb-multiarch debugs PsyQ and PSn00bSDK programs.',
        where: mipsToolchain,
        check: async () => !!mipsToolchain(),
        install: async progress => {
            const dir = storage('mips', MIPS_VERSION);
            await install(MIPS_URL, dir, progress, 'MIPS toolchain');
            await install(GDB_URL, dir, progress, 'gdb-multiarch', true);
        },
    },
    {
        id: 'psn00bsdk', name: 'PSn00bSDK', kinds: ['cmake'],
        what: 'The SDK (libraries, tools and its GCC) of PSn00bSDK projects.',
        where: psn00bsdkPath,
        check: async () => exists(path.join(psn00bsdkPath(), 'lib', 'libpsn00b', 'cmake', 'sdk.cmake')),
        install: async progress => install(await latestAsset('Lameguy64', 'PSn00bSDK', /^PSn00bSDK-.*-win32\.zip$/,
                                                             v => `PSn00bSDK-${v}-win32.zip`),
                                           storage('psn00bsdk'), progress, 'PSn00bSDK'),
    },
    {
        id: 'cmake', name: 'CMake', kinds: ['cmake'],
        what: 'Builds PSn00bSDK projects (CMakeLists.txt).',
        where: cmakePath,
        check: async () => runs(cmakePath(), ['--version']),
        install: async progress => install(await latestAsset('Kitware', 'CMake', /^cmake-.*-windows-x86_64\.zip$/,
                                                             v => `cmake-${v}-windows-x86_64.zip`),
                                           storage('cmake'), progress, 'CMake'),
    },
    {
        id: 'ninja', name: 'Ninja', kinds: ['cmake'],
        what: 'The build tool CMake uses for PSn00bSDK projects.',
        where: () => (ninjaDir() ? path.join(ninjaDir(), 'ninja.exe') : 'ninja (PATH)'),
        check: async () => runs(ninjaDir() ? path.join(ninjaDir(), 'ninja.exe') : 'ninja', ['--version']),
        install: async progress => install(await latestAsset('ninja-build', 'ninja', /^ninja-win\.zip$/, () => 'ninja-win.zip'),
                                           storage('ninja'), progress, 'Ninja'),
    },
    {
        id: 'ps1transfer', name: 'ps1transfer', kinds: ['all'],
        what: 'Talks to the PS1 DEV CART cartridges (comes with the extension).',
        where: () => host.settings().get('ps1transferPath') || ps1transferBundled(),
        check: async () => exists(host.settings().get('ps1transferPath') || ps1transferBundled()),
    },
    {
        id: 'cpptools', name: 'C/C++ extension (Microsoft)', kinds: ['all'], optional: true,
        what: 'The debugger front end (and IntelliSense).',
        where: () => 'ms-vscode.cpptools',
        check: async () => !!vscode.extensions.getExtension('ms-vscode.cpptools'),
        install: async () => vscode.commands.executeCommand('workbench.extensions.installExtension', 'ms-vscode.cpptools'),
    },
    {
        id: 'yaroze', name: 'Net Yaroze build environment (WSL)', kinds: ['yaroze'],
        what: 'Builds Net Yaroze projects: GCC 2.8.1 (the PSX3 compiler), the SDK from the Net Yaroze Software ' +
              'Development Disc (downloaded) and yarexe, installed in /opt/yaroze in WSL (needs your WSL password).',
        where: () => 'WSL: ' + host.settings().get('toolchainPath'),
        check: async () => {
            const tc = host.settings().get('toolchainPath');
            return wslHas(`test -x ${tc}/bin/mipsel-unknown-ecoff-gcc && test -f ${tc}/mipsel-unknown-ecoff/include/libps.h ` +
                          `&& command -v yarexe >/dev/null || test -x ${tc}/bin/yarexe`);
        },
        install: installNetYaroze,
    },
    {
        id: 'wslgdb', name: 'gdb-multiarch in WSL', kinds: ['yaroze'], optional: true,
        what: 'Debugs Net Yaroze programs.',
        where: () => 'WSL: ' + host.settings().get('gdbPath'),
        check: async () => wslHas(`test -x ${host.settings().get('gdbPath')}`),
        install: async () => {
            const distro = host.settings().get('wslDistribution');
            const term = vscode.window.createTerminal({
                name: 'Install gdb-multiarch (WSL)', shellPath: 'wsl.exe',
                shellArgs: [...(distro ? ['-d', distro] : []), '-e', 'sh', '-c',
                            'sudo apt-get update && sudo apt-get install -y gdb-multiarch; echo; echo Done: close this terminal.; read x'],
            });
            term.show();
        },
    },
];

// The state of every tool: [{ id, name, what, kinds, ok, where, installable, manual, optional }]
async function status() {
    return Promise.all(TOOLS.map(async t => {
        let ok = false;
        try {
            ok = await t.check();
        } catch (e) {
            ok = false;
        }
        let where = '';
        try {
            where = ok ? t.where() : '';
        } catch (e) {
            where = '';
        }
        return { id: t.id, name: t.name, what: t.what, kinds: t.kinds, ok, where, optional: !!t.optional,
                 manual: !!t.manual, installable: !!t.install && (onWindows || ['cpptools', 'yaroze'].includes(t.id)) };
    }));
}

let installing = Promise.resolve();

// Installs a tool (one at a time); resolves true when it is then found
function installTool(id) {
    const tool = TOOLS.find(t => t.id === id);
    if (!tool || !tool.install) {
        return Promise.resolve(false);
    }
    const run = async () => {
        try {
            if (tool.manual || ['cpptools', 'wslgdb'].includes(id)) {
                await tool.install();
                return false;
            }
            await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification, cancellable: false,
                                               title: `PS1 DEV CART: installing ${tool.name}` },
                                             progress => tool.install(progress));
            const ok = await tool.check();
            host.output.appendLine(`${tool.name}: ${ok ? 'installed in ' + tool.where() : 'not found after the install'}`);
            if (ok) {
                vscode.window.setStatusBarMessage(`PS1 DEV CART: ${tool.name} installed`, 5000);
            }
            return ok;
        } catch (e) {
            host.output.appendLine(`${tool.name}: install failed: ${e.message || e}`);
            vscode.window.showErrorMessage(`PS1 DEV CART: ${tool.name} could not be installed: ${e.message || e}`);
            return false;
        }
    };
    installing = installing.then(run, run);
    return installing;
}

// The tools missing for these kinds of projects (that can be installed)
async function missingFor(kinds) {
    const all = await status();
    return all.filter(t => !t.ok && t.installable && !t.manual
                           && (t.kinds.includes('all') ? kinds.length > 0 : t.kinds.some(k => kinds.includes(k))));
}

// The toolchains the user picks from (checkboxes of the panel): each one the tools it needs, so that e.g. a PsyQ
// developer installs only the MIPS toolchain, not PSn00bSDK. kinds: the projects it builds (selected by default when
// the workspace has some). The MIPS toolchain is in PSn00bSDK too, for its gdb-multiarch (debugging).
const GROUPS = [
    { id: 'yaroze', name: 'Net Yaroze', kinds: ['yaroze'], tools: ['yaroze', 'wslgdb'] },
    { id: 'psyq', name: 'PsyQ / PsyQo', kinds: ['make'], tools: ['mips'] },
    { id: 'psn00bsdk', name: 'PSn00bSDK', kinds: ['cmake'], tools: ['psn00bsdk', 'cmake', 'ninja', 'mips'] },
    { id: 'emulator', name: 'Emulator (PCSX-Redux)', kinds: [], tools: ['redux'] },
    { id: 'debugger', name: 'Debugger (C/C++ extension)', kinds: [], tools: ['cpptools'] },
];

// The groups selected by default: the toolchains of the workspace's projects, the emulator and the debugger
function defaultGroups(kinds) {
    return GROUPS.filter(g => !g.kinds.length || g.kinds.some(k => kinds.includes(k))).map(g => g.id);
}

// The tools missing in these groups that can be installed (each once; not the manual ones)
async function missingIn(groupIds) {
    const ids = new Set(GROUPS.filter(g => groupIds.includes(g.id)).flatMap(g => g.tools));
    return (await status()).filter(t => ids.has(t.id) && !t.ok && t.installable && !t.manual);
}

module.exports = {
    init, status, installTool, missingFor, missingIn, GROUPS, defaultGroups, emulatorPath, mipsToolchain, psn00bsdkPath,
    cmakePath, ninjaDir, ps1transferBundled, storage, download,
};
