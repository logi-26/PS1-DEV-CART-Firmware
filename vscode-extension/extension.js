// PS1 DEV CART extension for VS Code: builds, runs and debugs PlayStation
// projects in PCSX-Redux or on a PlayStation with a PS1 DEV CART cartridge
// (link play on two emulators or two consoles included), shows compiler errors
// in the Problems panel, and helps with IntelliSense, new projects and textures.
//
// Projects: Net Yaroze (an 'auto' script (siocons batch file) and a makefile,
// built in WSL), PsyQ / PsyQo (nugget makefiles, e.g. the PSX.Dev templates)
// and PSn00bSDK (CMake or makefiles), built on Windows. The settings, commands
// and debug/task types are named "ps1devcart" (before version 2.6: "netyaroze",
// from when the extension was "Net Yaroze"): settings of the earlier name are
// still read, and launch.json/tasks.json of the earlier name are offered an update.

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const tools = require('./toolchains');
const projectTemplates = require('./projecttemplates');

const TASK_TYPE = 'ps1devcart';
const MATCHER = '$ps1devcart-gcc';
const MATCHER_SDK = '$ps1-gcc';   // PsyQ/PSn00bSDK builds on Windows (paths can be absolute: C:/...)

// Make targets of each task; 'rebuild' relies on the usual 'all' target.
const TARGETS = {
    build:   { label: 'Build',        make: [],               group: vscode.TaskGroup.Build,   matcher: true },
    clean:   { label: 'Clean',        make: ['clean'],        group: vscode.TaskGroup.Clean,   matcher: false },
    rebuild: { label: 'Rebuild',      make: ['clean', 'all'], group: vscode.TaskGroup.Rebuild, matcher: true },
    run:     { label: 'Run',          make: ['run'],          group: undefined,                matcher: true },
    // Only for projects whose makefile has a 'link' target (two emulators linked through SIO1)
    link:    { label: 'Run Linked',   make: ['link'],         group: undefined,                matcher: true },
    models:  { label: 'Build Models', make: ['models'],       group: undefined,                matcher: false },
    // Only for projects whose makefile has 'iso' and 'run-iso' targets (CD image made with mkpsxiso)
    iso:     { label: 'Build CD Image', make: ['iso'],        group: undefined,                matcher: true },
    runIso:  { label: 'Run CD Image', make: ['run-iso'],      group: undefined,                matcher: true },
};

// Targets offered only when the makefile has them (task target: make target)
const OPTIONAL_TARGETS = { link: 'link', iso: 'iso', runIso: 'run-iso' };
// Targets that start the emulator
const RUN_TARGETS = ['run', 'link', 'runIso'];

const MAKEFILES = ['Makefile', 'makefile', 'GNUmakefile'];
const TEMPLATE_FILES = {
    'main.c': 'main.c', 'Makefile': 'Makefile', 'auto': 'auto', 'gitignore': '.gitignore',
    'iso.xml': 'iso.xml', 'system.cnf': 'system.cnf',
    'vscode/launch.json': '.vscode/launch.json', 'vscode/settings.json': '.vscode/settings.json',
};

let output;

// ---------------------------------------------------------------------------
// WSL helpers
// ---------------------------------------------------------------------------

// Settings "ps1devcart.*"; one not set is read from its earlier name "netyaroze.*" (before version 2.6), so the
// settings made with earlier versions keep working. Changes are saved with the new name.
const SETTINGS = 'ps1devcart';
const OLD_SETTINGS = 'netyaroze';

const isSet = info => !!info && [info.workspaceFolderValue, info.workspaceValue, info.globalValue].some(v => v !== undefined);

function settings() {
    const current = vscode.workspace.getConfiguration(SETTINGS);
    const old = vscode.workspace.getConfiguration(OLD_SETTINGS);
    const oldValue = key => {
        if (isSet(current.inspect(key))) {
            return undefined;
        }
        const info = old.inspect(key);     // (no longer declared: may come back empty, the value is still there)
        return isSet(info) ? info : (old.get(key) !== undefined ? { key, globalValue: old.get(key) } : undefined);
    };
    return {
        get: (key, fallback) => {
            const earlier = oldValue(key);
            return earlier ? old.get(key, fallback) : current.get(key, fallback);
        },
        inspect: key => oldValue(key) || current.inspect(key),
        update: (key, value, target) => current.update(key, value, target),
    };
}

const affectsSettings = e => e.affectsConfiguration(SETTINGS) || e.affectsConfiguration(OLD_SETTINGS);

// launch.json / tasks.json written by versions before 2.6 ("type": "netyaroze", "$netyaroze-gcc"): an update to the
// new names is offered (VS Code doesn't know the earlier types any more)
const OLD_TYPE = /("type"\s*:\s*")netyaroze(")/g;
const OLD_MATCHER = /\$netyaroze-gcc/g;

async function offerNewNames() {
    const files = [];
    for (const folder of vscode.workspace.workspaceFolders || []) {
        for (const name of ['launch.json', 'tasks.json']) {
            const file = path.join(folder.uri.fsPath, '.vscode', name);
            let text;
            try {
                text = fs.readFileSync(file, 'utf8');
            } catch {
                continue;
            }
            if (text.search(OLD_TYPE) >= 0 || text.search(OLD_MATCHER) >= 0) {
                files.push({ file, text });
            }
        }
    }
    if (!files.length) {
        return;
    }
    const names = files.map(f => path.relative(path.dirname(path.dirname(f.file)), f.file)).join(', ');
    const choice = await vscode.window.showInformationMessage(
        `${names}: written by an earlier version of the PS1 DEV CART extension ("netyaroze" names). Update them to the ` +
        'new names ("ps1devcart"), so that Run and Debug and the tasks work?', 'Update', 'Not now');
    if (choice !== 'Update') {
        return;
    }
    for (const { file, text } of files) {
        fs.writeFileSync(file, text.replace(OLD_TYPE, '$1ps1devcart$2').replace(OLD_MATCHER, '$ps1devcart-gcc'));
        output.appendLine(`Updated to the new names: ${file}`);
    }
}

// On Windows everything runs through 'wsl -e' (no shell, so arguments with
// spaces are safe). In a WSL remote window the extension runs in Linux.
const onWindows = process.platform === 'win32';

function linuxCommand(args) {
    if (!onWindows) {
        return { command: args[0], args: args.slice(1) };
    }
    const distro = settings().get('wslDistribution');
    return { command: 'wsl', args: [...(distro ? ['-d', distro] : []), '-e', ...args] };
}

function runLinux(args, options = {}) {
    const { command, args: argv } = linuxCommand(args);
    return new Promise((resolve, reject) => {
        cp.execFile(command, argv, { encoding: 'utf8', windowsHide: true, ...options }, (err, stdout, stderr) => {
            if (err) {
                err.stdout = stdout;
                err.stderr = stderr;
                reject(err);
            } else {
                resolve({ stdout, stderr });
            }
        });
    });
}

async function toLinuxPath(p) {
    if (!p || (!onWindows && p.startsWith('/'))) {
        return p;
    }
    return (await runLinux(['wslpath', '-u', p])).stdout.trim();
}

async function toHostPath(linuxPath) {
    if (!onWindows) {
        return linuxPath;
    }
    return (await runLinux(['wslpath', '-w', linuxPath])).stdout.trim();
}

// ---------------------------------------------------------------------------
// Projects
// ---------------------------------------------------------------------------

// A project: { folder (workspace folder), dir (absolute path), rel (path
// relative to the workspace folder, '' at its root), name, kind }. Projects can
// be at the root of a workspace folder or in a subfolder (e.g. example/).
// kind: 'yaroze' (Net Yaroze: an 'auto' script and a makefile, built in WSL),
// 'make' (PsyQ, PsyQo or PSn00bSDK with a Makefile, e.g. the PSX.Dev templates:
// nugget's common.mk; built on Windows with the PSX.Dev toolchain) or 'cmake'
// (PSn00bSDK with CMake: psn00bsdk_add_executable).
let projects = [];
let lastProject;

const KIND_NAMES = { yaroze: 'Net Yaroze', make: 'PsyQ / nugget', cmake: 'PSn00bSDK' };

// Not searched for projects (the extension's own template copy, SDKs and build folders included)
const EXCLUDE = '{**/node_modules/**,**/.git/**,**/.vscode-test/**,**/vscode-extension/template/**,' +
    '**/vscode-extension/templates/**,**/third_party/**,' +
    '**/build/**,**/buildtools/**}';

function readText(file) {
    try {
        return fs.readFileSync(file, 'utf8');
    } catch (e) {
        return '';
    }
}

function isProject(dir) {
    return fs.existsSync(path.join(dir, 'auto'))
        && MAKEFILES.some(m => fs.existsSync(path.join(dir, m)));
}

// The kind of project in a folder, undefined if none
function projectKind(dir) {
    if (isProject(dir)) {
        return 'yaroze';
    }
    if (/psn00bsdk_add_executable\s*\(/i.test(readText(path.join(dir, 'CMakeLists.txt')))) {
        return 'cmake';
    }
    const makefile = MAKEFILES.map(m => readText(path.join(dir, m))).find(Boolean) || '';
    // nugget (PSX.Dev templates: PsyQ, PsyQo, bare metal) or a PS-X EXE target (e.g. PSn00bSDK makefiles)
    if (/^\s*-?include\s+\S*common\.mk/m.test(makefile) || /^\s*TYPE\s*[:?]?=\s*ps-exe/m.test(makefile)
        || /psn00b/i.test(makefile)) {
        return 'make';
    }
    return undefined;
}

function makeProject(folder, dir) {
    const rel = path.relative(folder.uri.fsPath, dir).split(path.sep).join('/');
    return { folder, dir, rel, name: rel ? `${folder.name}/${rel}` : folder.name, kind: projectKind(dir) || 'yaroze' };
}

const isYaroze = project => project.kind === 'yaroze';

// Does the project's makefile have this make target?
function hasMakeTarget(project, name) {
    const rule = new RegExp('^' + name.replace(/[-.]/g, '\\$&') + '[ \\t]*:(?!=)', 'm');
    return MAKEFILES.some(m => {
        try {
            return rule.test(fs.readFileSync(path.join(project.dir, m), 'utf8'));
        } catch (e) {
            return false;
        }
    });
}

// Can the project run this task target? Optional ones need their make target:
// 'link' (link play in two emulators), 'iso' and 'run-iso' (CD image). PsyQ and
// PSn00bSDK projects have Build, Clean and Rebuild tasks (Run starts the
// emulator itself).
function hasTarget(project, target) {
    if (!isYaroze(project)) {
        return ['build', 'clean', 'rebuild'].includes(target);
    }
    return !OPTIONAL_TARGETS[target] || hasMakeTarget(project, OPTIONAL_TARGETS[target]);
}

const hasLinkTarget = project => isYaroze(project) && hasTarget(project, 'link');
const hasIsoTargets = project => isYaroze(project) && hasTarget(project, 'iso') && hasTarget(project, 'runIso');

async function findProjects() {
    const found = [];
    for (const folder of vscode.workspace.workspaceFolders || []) {
        const files = await vscode.workspace.findFiles(
            new vscode.RelativePattern(folder, '**/{auto,Makefile,makefile,GNUmakefile,CMakeLists.txt}'), EXCLUDE, 300);
        const dirs = [...new Set(files.map(u => path.dirname(u.fsPath)))]
            .filter(dir => projectKind(dir))
            .sort((a, b) => a.length - b.length || a.localeCompare(b));
        found.push(...dirs.map(dir => makeProject(folder, dir)));
    }
    projects = found;
    await vscode.commands.executeCommand('setContext', 'ps1devcart.hasProject', projects.length > 0);
    await vscode.commands.executeCommand('setContext', 'ps1devcart.hasLinkProject', projects.some(hasLinkTarget));
    await vscode.commands.executeCommand('setContext', 'ps1devcart.hasIsoProject', projects.some(hasIsoTargets));
    return projects;
}

// ---------------------------------------------------------------------------
// PsyQ and PSn00bSDK projects: built on Windows (the PSX.Dev toolchain's make,
// or CMake with PSn00bSDK), the program is a PS-X EXE with its ELF (debug
// information included)
// ---------------------------------------------------------------------------

// The MIPS toolchain, PSn00bSDK and CMake: the settings, the ones already installed (PSX.Dev's...) or installed by
// the extension (toolchains.js)
const mipsToolchain = () => tools.mipsToolchain();
const psn00bsdkPath = () => tools.psn00bsdkPath();
const cmakePath = () => tools.cmakePath();

// gdb-multiarch for PsyQ/PSn00bSDK programs: Windows' (PSX.Dev toolchain) when there is one, else WSL's
function nativeGdb() {
    const set = settings().get('windowsGdbPath');
    if (set) {
        return fs.existsSync(set) ? set : '';
    }
    const tc = mipsToolchain();
    const gdb = tc && path.join(tc, onWindows ? 'gdb-multiarch.exe' : 'gdb-multiarch');
    return onWindows && gdb && fs.existsSync(gdb) ? gdb : '';
}

// The environment of a PsyQ/PSn00bSDK build: the toolchain first in PATH (a PSn00bSDK project: PSn00bSDK's own, its
// GCC first, and Ninja)
function sdkEnv(kind) {
    const sdk = psn00bsdkPath() && path.join(psn00bsdkPath(), 'bin');
    const dirs = (kind === 'cmake' ? [sdk, tools.ninjaDir(), mipsToolchain()] : [mipsToolchain(), sdk]).filter(Boolean);
    const pathKey = Object.keys(process.env).find(k => k.toUpperCase() === 'PATH') || 'PATH';
    const env = { [pathKey]: [...dirs, process.env[pathKey] || ''].join(path.delimiter) };
    if (psn00bsdkPath()) {
        env.PSN00BSDK_LIBS = path.join(psn00bsdkPath(), 'lib', 'libpsn00b');
    }
    // GCC 15+ defaults to C23, where the PsyQ headers' K&R declarations (`int FntPrint();`) take no arguments. nugget's
    // common.mk appends to CFLAGS, so a -std of the Makefile still wins.
    if (kind === 'make' && process.env.CFLAGS === undefined) {
        env.CFLAGS = '-std=gnu17';
    }
    return env;
}

const BUILD_TYPE = () => (settings().get('buildConfiguration') === 'Release' ? 'Release' : 'Debug');

// The program (PS-X EXE) and its ELF: { exe, elf } (absolute paths), undefined if not found.
// Net Yaroze: psx.exe (made by yarexe, libps included), main.exe (ECOFF). Makefile: TARGET.TYPE and TARGET.elf
// (nugget). CMake: the newest PS-X EXE in build/.
function programOf(project) {
    if (isYaroze(project)) {
        return { exe: path.join(project.dir, 'psx.exe'), elf: path.join(project.dir, 'main.elf') };
    }
    const isPsxExe = file => {
        try {
            const fd = fs.openSync(file, 'r');
            const head = Buffer.alloc(8);
            fs.readSync(fd, head, 0, 8, 0);
            fs.closeSync(fd);
            return head.toString('latin1') === 'PS-X EXE';
        } catch (e) {
            return false;
        }
    };
    if (project.kind === 'make') {
        const makefile = MAKEFILES.map(m => readText(path.join(project.dir, m))).find(Boolean) || '';
        const value = name => (new RegExp(`^\\s*${name}\\s*[:?]?=\\s*(\\S+)`, 'm').exec(makefile) || [])[1];
        const target = value('TARGET');
        const candidates = target ? [`${target}.${value('TYPE') || 'ps-exe'}`, `${target}.ps-exe`, `${target}.exe`] : [];
        let exe = candidates.map(c => path.join(project.dir, c)).find(isPsxExe);
        if (!exe) {   // (another makefile: the newest PS-X EXE of the folder)
            exe = newestPsxExe(project.dir, isPsxExe);
        }
        return exe && { exe, elf: exe.replace(/\.[^.\\/]*$/, '.elf') };
    }
    const exe = newestPsxExe(path.join(project.dir, 'build'), isPsxExe);
    return exe && { exe, elf: exe.replace(/\.[^.\\/]*$/, '.elf') };
}

function newestPsxExe(dir, isPsxExe) {
    let names;
    try {
        names = fs.readdirSync(dir);
    } catch (e) {
        return undefined;
    }
    return names.filter(n => /\.(ps-exe|exe|psx)$/i.test(n)).map(n => path.join(dir, n)).filter(isPsxExe)
        .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];
}

// The program for ps1transfer and the emulator: relative to the project folder (they run there)
function programArg(project) {
    const program = programOf(project);
    return program && path.relative(project.dir, program.exe);
}

// The project of the file being edited, or the only one, or the last one
// used; otherwise asks.
async function pickProject() {
    if (projects.length === 0) {
        await findProjects();
    }
    if (projects.length === 0) {
        showError('PS1 DEV CART: no project found. A project is a folder with an "auto" script and a makefile (Net ' +
                  'Yaroze), a Makefile using nugget (PsyQ, PsyQo, PSX.Dev templates) or making a PS-X EXE, or a ' +
                  'CMakeLists.txt with psn00bsdk_add_executable (PSn00bSDK).');
        return undefined;
    }
    if (projects.length === 1) {
        return projects[0];
    }
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.uri.scheme === 'file') {
        const file = editor.document.uri.fsPath.toLowerCase();
        const inside = projects.filter(p => file.startsWith(p.dir.toLowerCase() + path.sep));
        if (inside.length) {
            return inside.sort((a, b) => b.dir.length - a.dir.length)[0];
        }
    }
    const items = projects.map(p => ({ label: p.name, description: p.dir, project: p }));
    if (lastProject) {
        items.sort((a, b) => (b.project.dir === lastProject.dir) - (a.project.dir === lastProject.dir));
    }
    const pick = await vscode.window.showQuickPick(items, { placeHolder: 'Project (Net Yaroze, PsyQ or PSn00bSDK)' });
    if (pick) {
        lastProject = pick.project;
    }
    return pick && pick.project;
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

// The build of a PsyQ (Makefile) or PSn00bSDK (CMake) project, on Windows. make: the PSX.Dev templates' targets
// (BUILD=Debug or Release). CMake: configured once (the project's "default" preset, or PSn00bSDK's toolchain file),
// then built in build/.
function sdkExecution(project, target) {
    const options = { cwd: project.dir, env: sdkEnv(project.kind) };
    if (project.kind === 'make') {
        const goals = { build: [], clean: ['clean'], rebuild: ['clean', 'all'] }[target];
        return new vscode.ProcessExecution('make', [`BUILD=${BUILD_TYPE()}`, ...goals], options);
    }
    const cmake = `"${cmakePath()}"`;
    const presets = fs.existsSync(path.join(project.dir, 'CMakePresets.json'));
    const toolchain = path.join(psn00bsdkPath(), 'lib', 'libpsn00b', 'cmake', 'sdk.cmake');
    const configure = presets ? `${cmake} --preset default -DCMAKE_BUILD_TYPE=${BUILD_TYPE()}`
        : `${cmake} -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE()} "-DCMAKE_TOOLCHAIN_FILE=${toolchain}"`;
    const build = { build: '', clean: ' --target clean', rebuild: ' --clean-first' }[target];
    const line = `(if not exist build\\build.ninja ${configure}) && ${cmake} --build build${build}`;
    return new vscode.ShellExecution(line, Object.assign(options, { executable: 'cmd.exe', shellArgs: ['/d', '/s', '/c'] }));
}

async function createTask(project, target, definition) {
    const t = TARGETS[target];
    let execution;
    if (isYaroze(project)) {
        const make = ['make', ...t.make];
        if (RUN_TARGETS.includes(target)) {
            const emulator = tools.emulatorPath();     // (the same emulator for every kind of project)
            if (emulator) {
                make.push('PCSX_REDUX=' + await toLinuxPath(emulator));
            }
        }
        const { command, args } = linuxCommand(make);
        execution = new vscode.ProcessExecution(command, args, { cwd: project.dir });
    } else {
        execution = sdkExecution(project, target);
    }
    const def = definition || Object.assign({ type: TASK_TYPE, target }, project.rel ? { project: project.rel } : {});
    // Tasks of projects in subfolders show which project they are for
    const name = project.rel ? `${t.label} (${project.rel})` : t.label;
    const matchers = t.matcher ? [isYaroze(project) ? MATCHER : MATCHER_SDK] : [];
    const task = new vscode.Task(def, project.folder, name, isYaroze(project) ? 'Net Yaroze' : KIND_NAMES[project.kind],
                                 execution, matchers);
    if (t.group) {
        task.group = t.group;
    }
    task.presentationOptions = {
        reveal: vscode.TaskRevealKind.Always,
        panel: vscode.TaskPanelKind.Shared,
        clear: true,
    };
    return task;
}

const taskProvider = {
    async provideTasks() {
        const tasks = [];
        for (const project of await findProjects()) {
            for (const target of Object.keys(TARGETS)) {
                if (hasTarget(project, target)) {
                    tasks.push(await createTask(project, target));
                }
            }
        }
        return tasks;
    },
    // Tasks from tasks.json: { "type": "ps1devcart", "target": "run", "project": "example" }
    async resolveTask(task) {
        const target = task.definition.target;
        if (!TARGETS[target] || !task.scope || typeof task.scope === 'number') {
            return undefined;
        }
        const dir = path.join(task.scope.uri.fsPath, task.definition.project || '');
        return createTask(makeProject(task.scope, dir), target, task.definition);
    },
};

// ---------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------

// The emulator Run will start: the setting, or else what the makefile's run
// target would start (from a dry run, 'make -n run': its last command).
async function emulatorFor(project) {
    const setting = tools.emulatorPath();      // (the setting, else the PCSX-Redux installed)
    if (!isYaroze(project)) {   // (PsyQ/PSn00bSDK: no makefile run target, the setting, a Windows path)
        return { path: setting, fromSetting: true, host: true };
    }
    if (setting) {
        return { path: await toLinuxPath(setting), fromSetting: true };
    }
    const { stdout } = await runLinux(['make', '-n', 'run'], { cwd: project.dir });
    const last = stdout.trim().split('\n').pop() || '';
    const m = /^\s*(?:"([^"]+)"|'([^']+)'|(\S+))/.exec(last);
    return { path: m && (m[1] || m[2] || m[3]), fromSetting: false };
}

// Checks the emulator exists (in WSL); if not, offers to choose it.
async function checkEmulator(project) {
    let emulator;
    try {
        emulator = await emulatorFor(project);
    } catch (e) {
        return true;  // let make report the problem
    }
    if (emulator.host) {    // PsyQ/PSn00bSDK project: the setting, a Windows path
        if (emulator.path && fs.existsSync(emulator.path)) {
            return true;
        }
        return offerEmulator(emulator.path);
    }
    if (!emulator.path || !emulator.path.startsWith('/')) {
        return true;  // not a file path we can check
    }
    try {
        await runLinux(['test', '-f', emulator.path]);
        return true;
    } catch (e) {
        // not found
    }
    if (emulator.fromSetting) {
        return offerEmulator(emulator.path);
    }
    const answer = await vscode.window.showErrorMessage(
        `PS1 DEV CART: the emulator ${emulator.path} (from the project's makefile) was not found.`, 'Choose Emulator...');
    return answer === 'Choose Emulator...' && !!(await chooseEmulator());
}

// No emulator (or not found): install PCSX-Redux (like PSX.Dev), or choose one
async function offerEmulator(missing) {
    const install = 'Install PCSX-Redux';
    const answer = await vscode.window.showErrorMessage(
        missing ? `PS1 DEV CART: the emulator ${missing} (Emulator Path setting) was not found.`
            : 'PS1 DEV CART: no emulator yet: install PCSX-Redux, or choose the emulator to use for every project.',
        ...(onWindows ? [install] : []), 'Choose Emulator...');
    if (answer === install) {
        if (missing) {
            await settings().update('emulatorPath', undefined, vscode.ConfigurationTarget.Global);
        }
        const ok = await tools.installTool('redux');
        updatePanel();
        return ok;
    }
    return answer === 'Choose Emulator...' && !!(await chooseEmulator());
}

// Asks for the emulator program and saves it in the user settings.
async function chooseEmulator(file) {
    let chosen = file;
    if (!chosen) {
        const picked = await vscode.window.showOpenDialog({
            canSelectFiles: true, canSelectFolders: false, canSelectMany: false,
            openLabel: 'Use this emulator',
            filters: onWindows ? { Programs: ['exe'], 'All files': ['*'] } : undefined,
        });
        if (!picked) {
            return undefined;
        }
        chosen = picked[0].fsPath;
    }
    await settings().update('emulatorPath', chosen, vscode.ConfigurationTarget.Global);
    vscode.window.showInformationMessage(`PS1 DEV CART: Run will use ${chosen}.`);
    return chosen;
}

// ---------------------------------------------------------------------------
// PS1 DEV CART: running and debugging on a PlayStation with a PS1 DEV CART
// cartridge (PS1USB cartridge with the PS1 DEV CART firmware). ps1transfer
// (a Windows program, run through its /mnt path from WSL) loads psx.exe: run
// with printf and the PCDRV file server in a terminal, or stopped at its
// entry point with a GDB server ("-gdb"), like PCSX-Redux's.
// ---------------------------------------------------------------------------

// Debug configuration target of the cartridge ('ps1usb': its earlier name)
const CART_TARGETS = ['devcart', 'ps1usb'];
const isCartTarget = target => CART_TARGETS.includes(target);

// A setting, or its earlier name (before the PS1 DEV CART name)
function cartSetting(name, oldName) {
    const value = settings().inspect(name);
    const set = value && [value.workspaceFolderValue, value.workspaceValue, value.globalValue].some(v => v !== undefined);
    return set ? settings().get(name) : (settings().get(oldName) || settings().get(name));
}

const cartSerial = () => cartSetting('devcartSerial', 'ps1usbSerial') || '';
const cartTty = () => cartSetting('devcartTty', 'ps1usbTty') || 'usb';

// ps1transfer from the setting, or asks for it and saves it in the user settings.
async function ps1transferPath(quiet) {
    let exe = settings().get('ps1transferPath') || tools.ps1transferBundled();
    if (exe && !onWindows && /^[A-Za-z]:[\\/]/.test(exe)) {
        exe = (await runLinux(['wslpath', '-u', exe])).stdout.trim();
    }
    if (exe && fs.existsSync(exe)) {
        return exe;
    }
    if (quiet) {
        return undefined;
    }
    const answer = await vscode.window.showErrorMessage(
        exe ? `PS1 DEV CART: ps1transfer was not found: ${exe}`
            : 'PS1 DEV CART: running on a PS1 needs ps1transfer, the PS1 DEV CART tool (in the PS1 DEV CART zip).',
        'Choose ps1transfer...');
    if (answer !== 'Choose ps1transfer...') {
        return undefined;
    }
    const picked = await vscode.window.showOpenDialog({
        canSelectFiles: true, canSelectFolders: false, canSelectMany: false,
        openLabel: 'Use this ps1transfer',
        filters: onWindows ? { Programs: ['exe'], 'All files': ['*'] } : undefined,
    });
    if (!picked) {
        return undefined;
    }
    await settings().update('ps1transferPath', picked[0].fsPath, vscode.ConfigurationTarget.Global);
    return ps1transferPath();
}

// ps1transfer options choosing the cartridge (none: the first one found)
const cartArgs = () => (cartSerial() ? ['-n', cartSerial()] : []);

// ps1transfer options for the game's TTY (printf). run: a plain run, whose
// printf and PCDRV file server use the console mode ("-c")
function ttyArgs(run) {
    switch (cartTty()) {
        case 'sio': return ['-c', 'sio'];     // serial port TTY driver, e.g. for link play over the cable
        case 'link': return ['-c', 'link'];   // the same driver over the USB link (ps1transfer -relay)
        case 'bios': return ['-c', 'off'];
        default: return run ? ['-c'] : [];    // printf to this computer (USB)
    }
}

// The cartridges connected to this computer ("ps1transfer -l"): [{ serial, index, description }]
async function listCarts(exe) {
    const stdout = await new Promise(resolve => {
        cp.execFile(exe, ['-l'], { encoding: 'utf8', windowsHide: true, timeout: 15000 }, (err, out) => resolve(out || ''));
    });
    const carts = [];
    for (const line of stdout.split(/\r?\n/)) {
        const m = /^\s*-z\s+(\d+)\s+-n\s+(.*)$/.exec(line);
        if (!m) {
            continue;
        }
        // A cartridge another program has open is listed without its serial number: "-z 1 -n   (in use)"
        const inUse = /\(in use\)/.test(m[2]);
        const serial = inUse ? '' : m[2].trim().split(/\s+/)[0];
        carts.push({ index: Number(m[1]), serial, description: inUse ? '(in use)' : m[2].trim().slice(serial.length).trim(),
                     inUse });
    }
    return carts;
}

let cartItem;  // status bar item showing the cartridge in use

function updateCartItem() {
    if (!cartItem) {
        return;
    }
    const serial = cartSerial();
    cartItem.text = `$(plug) ${serial || 'PS1 DEV CART'}`;
    cartItem.tooltip = serial ? `PS1 DEV CART: Run and Debug on PS1 use the PS1 DEV CART ${serial} (click to change)`
        : 'PS1 DEV CART: Run and Debug on PS1 use the first PS1 DEV CART found (click to choose one)';
    updatePanel();
}

// Saves the main cartridge; the second cart is cleared when it is the same one (a cart is never both)
async function saveCart(serial) {
    await settings().update('devcartSerial', serial, vscode.ConfigurationTarget.Global);
    if (serial && serial === cartSerial2()) {
        await settings().update('devcartSerial2', '', vscode.ConfigurationTarget.Global);
    }
}

// Lists the connected cartridges and saves the one chosen (user settings).
// serial: chosen without asking ('' = the first one found), e.g. from a keybinding
async function selectCart(serial) {
    if (typeof serial === 'string') {
        await saveCart(serial);
        updateCartItem();
        return serial;
    }
    const exe = await ps1transferPath();
    if (!exe) {
        return undefined;
    }
    const carts = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: 'Looking for PS1 DEV CART cartridges' }, () => listCarts(exe));
    const current = cartSerial();
    const items = carts.filter(c => c.serial).map(c => ({
        label: c.serial, description: c.serial === current ? '(in use)' : '', detail: `${c.description} (-z ${c.index})`,
        serial: c.serial,
    }));
    items.push({ label: 'First one found', description: current ? '' : '(in use)',
                 detail: 'The first cartridge ps1transfer finds (one cartridge connected)', serial: '' });
    const pick = await vscode.window.showQuickPick(items, {
        placeHolder: carts.length ? 'PS1 DEV CART to run and debug on (USB serial number)'
            : 'No cartridge found: check the USB cable (and that no other program is using it)',
    });
    if (!pick) {
        return undefined;
    }
    await saveCart(pick.serial);
    updateCartItem();
    vscode.window.setStatusBarMessage(`PS1 DEV CART: PS1 DEV CART ${pick.serial || '(first one found)'}`, 4000);
    return pick.serial;
}

// Builds, then runs psx.exe on the cartridge in a terminal (printf and the
// PCDRV file server of ps1transfer's console mode; Ctrl+C or the trash icon
// stops it, the game goes on). psx.exe, made by yarexe, includes libps.
async function runOnPs1() {
    const project = await pickProject();
    if (!project) {
        return undefined;
    }
    const exe = await ps1transferPath();
    if (!exe) {
        return undefined;
    }
    if (!(await buildForRun(project, 'run'))) {
        return undefined;
    }
    const args = [...cartArgs(), '-r', '-e', '-f', programArg(project), ...ttyArgs(true)];
    const execution = new vscode.ProcessExecution(exe, args, { cwd: project.dir });
    const def = Object.assign({ type: TASK_TYPE, target: 'runOnPs1' }, project.rel ? { project: project.rel } : {});
    const name = 'Run on PS1' + (cartSerial() ? ` (${cartSerial()})` : '') + (project.rel ? ` (${project.rel})` : '');
    const task = new vscode.Task(def, project.folder, name, 'PS1 DEV CART', execution, []);
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: true };
    output.appendLine(`> ${exe} ${args.join(' ')}`);
    return trackCartTask(await vscode.tasks.executeTask(task), [cartSerial()]);
}

// The terminals of ps1transfer that keep a cartridge open (Run on PS1's console, the USB link relay): execution ->
// the serials ('' = the first one found). Reset stops them first: the cartridge can only be opened by one program.
const cartTasks = new Map();

function trackCartTask(execution, serials) {
    cartTasks.set(execution, serials);
    return execution;
}

// Starts "ps1transfer -gdb" in the project folder. ready resolves true when
// its GDB server listens (the program is loaded and stopped at its entry
// point), seen in its output: this also works from WSL, where the Windows
// port can't be checked.
// serial / tty: another cartridge and TTY (link play on two cartridges), default: the selected one and its TTY.
function startPs1transfer(exe, project, port, serial, tty) {
    const cart = serial === undefined ? cartArgs() : (serial ? ['-n', serial] : []);
    const args = [...cart, '-gdb', String(port), '-f', programArg(project), ...(tty || ttyArgs(false))];
    output.appendLine(`> ${exe} ${args.join(' ')}`);
    output.show(true);
    const proc = cp.spawn(exe, args, { cwd: project.dir, windowsHide: true });
    proc.ps1devcartPs1transfer = true;
    proc.ps1devcartSerial = serial === undefined ? cartSerial() : serial;
    const ready = new Promise(resolve => {
        let recent = '';
        const timer = setTimeout(() => resolve(false), 60000);
        const done = ok => {
            clearTimeout(timer);
            resolve(ok);
        };
        proc.stdout.on('data', data => {
            // The upload progress lines end with \r
            const text = data.toString().replace(/[^\n]*bytes remaining\.\.\.\r/g, '').replace(/\r/g, '');
            output.append(text);
            recent = (recent + text).slice(-500);
            if (/GDB server on port/.test(recent)) {
                done(true);
            }
        });
        proc.stderr.on('data', data => output.append(data.toString()));
        proc.on('error', e => {
            output.appendLine('ps1transfer did not start: ' + e.message);
            done(false);
        });
        proc.on('exit', () => done(false));
    });
    return { proc, ready };
}

// Runs a target and resolves with the exit code when it finishes.
async function runTarget(target) {
    const project = await pickProject();
    if (!project) {
        return undefined;
    }
    if (!isYaroze(project) && target === 'run') {
        return runInEmulator(project);
    }
    if (!isYaroze(project) && !hasTarget(project, target)) {
        showError(`PS1 DEV CART: ${TARGETS[target].label} is for Net Yaroze projects (${project.name} is a ` +
                  `${KIND_NAMES[project.kind]} project).`);
        return undefined;
    }
    if (target === 'link' && !hasLinkTarget(project)) {
        showError(`PS1 DEV CART: the makefile of ${project.name} has no "link" target. ` +
                  'Run Linked runs "make link", which should start the game in two emulators linked through their serial ports.');
        return undefined;
    }
    if (!hasTarget(project, target)) {
        showError(`PS1 DEV CART: the makefile of ${project.name} has no "${OPTIONAL_TARGETS[target]}" target. ` +
                  'Build CD Image and Run CD Image run "make iso" and "make run-iso"; ' +
                  "see the build environment's example project for them.");
        return undefined;
    }
    if (RUN_TARGETS.includes(target) && !(await checkEmulator(project))) {
        return undefined;
    }
    return runTaskFor(project, target);
}

// PsyQ/PSn00bSDK project: builds, then runs the program in the emulator (in a terminal, like make run)
async function runInEmulator(project) {
    if (!(await checkEmulator(project)) || !(await buildForRun(project, 'run'))) {
        return undefined;
    }
    const emulator = tools.emulatorPath();
    const execution = new vscode.ProcessExecution(emulator, ['-run', '-loadexe', programArg(project)], { cwd: project.dir });
    const def = Object.assign({ type: TASK_TYPE, target: 'run' }, project.rel ? { project: project.rel } : {});
    const task = new vscode.Task(def, project.folder, 'Run' + (project.rel ? ` (${project.rel})` : ''), KIND_NAMES[project.kind],
                                 execution, []);
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Silent, panel: vscode.TaskPanelKind.Shared, clear: true };
    return vscode.tasks.executeTask(task);
}

// Runs a target of a project and resolves with the exit code. A build remembers when it was made (and its build type),
// Clean forgets it: Deploy and Debug don't build again a program that is up to date (see buildIfNeeded).
async function runTaskFor(project, target) {
    let task;
    try {
        task = await createTask(project, target);
    } catch (e) {
        showError('PS1 DEV CART: ' + describeError(e));
        return undefined;
    }
    const started = Date.now();
    const execution = await vscode.tasks.executeTask(task);
    const code = await new Promise(resolve => {
        const listener = vscode.tasks.onDidEndTaskProcess(e => {
            if (e.execution === execution) {
                listener.dispose();
                resolve(e.exitCode);
            }
        });
    });
    if (['build', 'rebuild', 'clean'].includes(target) && extensionContext) {
        await extensionContext.workspaceState.update(BUILT_KEY + project.dir, code === 0 && target !== 'clean'
            ? { time: started, type: BUILD_TYPE() } : undefined);
    }
    return code;
}

// ----- Up to date: no build before Deploy / Debug when nothing changed since the last build made here -----

const BUILT_KEY = 'ps1devcart.built:';
// Files a build uses (sources, makefiles, data embedded by a makefile); folders of build outputs and tools skipped
const SOURCE_EXTS = new Set(['.c', '.h', '.cc', '.cpp', '.cxx', '.hpp', '.hh', '.s', '.asm', '.inc', '.ld', '.lnk', '.mk',
                             '.cmake', '.tim', '.tmd', '.tod', '.vh', '.vb', '.vab', '.seq', '.sep', '.vag', '.xa', '.str']);
const SOURCE_NAMES = new Set(['makefile', 'gnumakefile', 'cmakelists.txt', 'cmakepresets.json']);
const SKIP_DIRS = new Set(['build', '.git', '.vscode', '.vs', 'node_modules', '.cache', 'out', 'dist', 'obj']);
const SCAN_MAX = 20000;

// The newest modification time (ms) of the project's source files, Infinity if there are too many to look at
function newestSource(dir) {
    let newest = 0;
    let seen = 0;
    const walk = (d, depth) => {
        let entries;
        try {
            entries = fs.readdirSync(d, { withFileTypes: true });
        } catch (e) {
            return;
        }
        for (const e of entries) {
            if (++seen > SCAN_MAX) {
                newest = Infinity;
                return;
            }
            const full = path.join(d, e.name);
            if (e.isDirectory()) {
                if (depth < 8 && !SKIP_DIRS.has(e.name.toLowerCase())) {
                    walk(full, depth + 1);
                }
            } else if (SOURCE_EXTS.has(path.extname(e.name).toLowerCase()) || SOURCE_NAMES.has(e.name.toLowerCase())) {
                try {
                    newest = Math.max(newest, fs.statSync(full).mtimeMs);
                } catch (err) {
                    // (removed meanwhile)
                }
            }
        }
    };
    walk(dir, 0);
    return newest;
}

// Builds the project unless it is up to date: built here with the same build type, its program there (and its debug
// information for a debug session), no source file changed since (saved first, as tasks do). Resolves the exit code.
async function buildIfNeeded(project, debug) {
    if (vscode.workspace.getConfiguration('task').get('saveBeforeRun') !== 'never') {
        await vscode.workspace.saveAll(false);
    }
    const last = extensionContext && extensionContext.workspaceState.get(BUILT_KEY + project.dir);
    const program = programOf(project);
    const outputs = program ? [program.exe, ...(debug ? [isYaroze(project) ? path.join(project.dir, 'main.exe')
        : program.elf] : [])] : [];
    if (last && last.type === BUILD_TYPE() && outputs.length && outputs.every(f => fs.existsSync(f)) &&
        newestSource(project.dir) < last.time) {
        output.appendLine(`${project.name}: up to date (no change since its last build), not built again ` +
                          '(Build always builds)');
        vscode.window.setStatusBarMessage(`PS1 DEV CART: ${project.name} up to date, not built again`, 4000);
        return 0;
    }
    return runTaskFor(project, 'build');
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

function createStatusBar(context) {
    const items = [
        ['$(tools) Build', 'ps1devcart.build', 'PS1 DEV CART: Build (make)'],
        ['$(play) Run', 'ps1devcart.run', 'PS1 DEV CART: Build and run in the emulator (make run)'],
        ['$(link) Run Linked', 'ps1devcart.runLinked', 'PS1 DEV CART: Build and run in two linked emulators (make link)'],
        ['$(debug-alt) Debug', 'ps1devcart.debug', 'PS1 DEV CART: Build and debug in PCSX-Redux'],
        ['$(trash) Clean', 'ps1devcart.clean', 'PS1 DEV CART: Clean (make clean)'],
        // PS1 DEV CART (shown once ps1transfer is set up)
        ['$(play-circle) Run on PS1', 'ps1devcart.runOnPs1', 'PS1 DEV CART: Build and run on a PlayStation (PS1 DEV CART)'],
        ['$(debug) Debug on PS1', 'ps1devcart.debugOnPs1', 'PS1 DEV CART: Build and debug on a PlayStation (PS1 DEV CART)'],
        ['$(plug) PS1 DEV CART', 'ps1devcart.selectCart', 'PS1 DEV CART: PS1 DEV CART used by Run and Debug on PS1'],
    ].map(([text, command, tooltip], i) => {
        // Each item needs its own id (and a name for the status bar's menu)
        const item = vscode.window.createStatusBarItem(command, vscode.StatusBarAlignment.Left, 50 - i);
        Object.assign(item, { text, command, tooltip, name: tooltip.split(' (')[0] });
        context.subscriptions.push(item);
        return item;
    });
    cartItem = items.find(item => item.command === 'ps1devcart.selectCart');
    const cartCommands = ['ps1devcart.runOnPs1', 'ps1devcart.debugOnPs1', 'ps1devcart.selectCart'];
    const update = async () => {
        await findProjects();
        updatePanel();
        const show = settings().get('showStatusBar') && projects.length > 0;
        // (Run Linked: the makefile's link target, or the extension's own launcher of two linked emulators)
        const showLink = show;
        const showCart = show && !!(settings().get('ps1transferPath') || tools.ps1transferBundled());
        updateCartItem();
        items.forEach(item => {
            const visible = item.command === 'ps1devcart.runLinked' ? showLink
                : (cartCommands.includes(item.command) ? showCart : show);
            if (visible) {
                item.show();
            } else {
                item.hide();
            }
        });
    };
    const watcher = vscode.workspace.createFileSystemWatcher('**/{auto,Makefile,makefile,GNUmakefile,CMakeLists.txt}');
    watcher.onDidCreate(update);
    watcher.onDidDelete(update);
    watcher.onDidChange(update);  // a 'link' or 'iso' target added or removed
    context.subscriptions.push(
        watcher,
        vscode.workspace.onDidChangeWorkspaceFolders(update),
        vscode.workspace.onDidChangeConfiguration(e => affectsSettings(e) && update()));
    update();
}

// ---------------------------------------------------------------------------
// IntelliSense (Microsoft C/C++ extension)
// ---------------------------------------------------------------------------

async function configureIntelliSense(folderArg) {
    // C/C++ reads its settings from the workspace folder's .vscode, also for
    // projects in subfolders (the include path covers the whole folder).
    const picked = folderArg || await pickProject();
    const folder = picked && (picked.folder || picked);
    if (!folder) {
        return undefined;
    }
    const toolchain = settings().get('toolchainPath').replace(/\/+$/, '');
    const includeLinux = toolchain + '/mipsel-unknown-ecoff/include';
    const gcc = toolchain + '/bin/mipsel-unknown-ecoff-gcc';

    let includePath;
    let defines;
    try {
        // Checked in WSL: VS Code blocks reading \\wsl.localhost paths from here
        // unless the host is allowed (security.allowedUNCHosts).
        await runLinux(['test', '-f', includeLinux + '/libps.h']).catch(() => {
            throw new Error(`libps.h not found in ${includeLinux}. Is the SDK installed (install-sdk.sh)?`);
        });
        includePath = await toHostPath(includeLinux);
        // The compiler's own predefined macros (__mips__, __GNUC__, ...)
        const { stdout } = await runLinux(['sh', '-c', `echo | "${gcc}" -dM -E -`]);
        defines = stdout.split('\n')
            .map(line => line.match(/^#define\s+(\S+)(?:\s+(.*?))?\s*$/))
            .filter(Boolean)
            .map(m => (m[2] ? `${m[1]}=${m[2]}` : m[1]))
            .sort();
    } catch (e) {
        showError('PS1 DEV CART: IntelliSense not configured: ' + describeError(e));
        return undefined;
    }

    const configuration = {
        name: 'Net Yaroze',
        includePath: ['${workspaceFolder}/**', includePath],
        defines,
        compilerPath: '',
        cStandard: 'c89',
        intelliSenseMode: 'gcc-x86',
    };

    const file = path.join(folder.uri.fsPath, '.vscode', 'c_cpp_properties.json');
    let json = { configurations: [], version: 4 };
    if (fs.existsSync(file)) {
        try {
            json = JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\s*\/\/.*$/gm, ''));
        } catch (e) {
            showError(
                `PS1 DEV CART: ${file} couldn't be read (${e.message}); it was not changed.`);
            return undefined;
        }
    }
    json.configurations = json.configurations || [];
    const index = json.configurations.findIndex(c => c.name === configuration.name);
    if (index >= 0) {
        json.configurations[index] = configuration;
    } else {
        json.configurations.unshift(configuration);
    }
    json.version = json.version || 4;
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, JSON.stringify(json, null, 4) + '\n');
    await hideCppRunButton(folder);

    const others = json.configurations.length > 1
        ? ' Select it with the configuration name in the status bar when a C file is open.' : '';
    vscode.window.showInformationMessage(`PS1 DEV CART: IntelliSense configured ("Net Yaroze" in c_cpp_properties.json).${others}`);
    return file;
}

// The C/C++ extension's "Run/Debug C/C++ File" button (▷ on C files) builds
// Windows programs, which is confusing next to Net Yaroze's Run and Debug:
// turn it off for this workspace folder.
async function hideCppRunButton(folder) {
    try {
        const cpp = vscode.workspace.getConfiguration('C_Cpp', folder.uri);
        if (cpp.inspect('debugShortcut') && cpp.inspect('debugShortcut').workspaceFolderValue === undefined) {
            await cpp.update('debugShortcut', false, vscode.ConfigurationTarget.WorkspaceFolder);
        }
    } catch (e) {
        // C/C++ extension not installed: no button to hide
    }
}

async function offerIntelliSense(context) {
    if (!settings().get('offerIntelliSense') || !vscode.extensions.getExtension('ms-vscode.cpptools')) {
        return;
    }
    const folders = [...new Set((await findProjects()).map(p => p.folder))];
    for (const folder of folders) {
        const key = 'intelliSenseOffered:' + folder.uri.toString();
        if (context.workspaceState.get(key)
            || fs.existsSync(path.join(folder.uri.fsPath, '.vscode', 'c_cpp_properties.json'))) {
            continue;
        }
        const answer = await vscode.window.showInformationMessage(
            `PS1 DEV CART: set up IntelliSense for libps in "${folder.name}"?`, 'Configure', 'Not now', "Don't ask again");
        if (answer === 'Configure') {
            await configureIntelliSense(folder);
        } else if (answer === "Don't ask again") {
            await settings().update('offerIntelliSense', false, vscode.ConfigurationTarget.Global);
        }
        await context.workspaceState.update(key, true);
    }
}

// ---------------------------------------------------------------------------
// New project
// ---------------------------------------------------------------------------

async function newProject(context, targetUri) {
    let target = targetUri;
    if (!target) {
        const picked = await vscode.window.showOpenDialog({
            canSelectFolders: true, canSelectFiles: false, canSelectMany: false,
            openLabel: 'Create Net Yaroze project here',
        });
        if (!picked) {
            return undefined;
        }
        target = picked[0];
    }
    const dir = target.fsPath;
    const clashes = Object.values(TEMPLATE_FILES).filter(name => fs.existsSync(path.join(dir, name)));
    if (clashes.length) {
        showError(`PS1 DEV CART: ${dir} already has ${clashes.join(', ')}; choose an empty folder.`);
        return undefined;
    }
    fs.mkdirSync(dir, { recursive: true });
    for (const [from, to] of Object.entries(TEMPLATE_FILES)) {
        fs.mkdirSync(path.dirname(path.join(dir, to)), { recursive: true });
        fs.copyFileSync(path.join(context.extensionPath, 'template', from), path.join(dir, to));
    }
    if (!targetUri) {
        const answer = await vscode.window.showInformationMessage(
            `PS1 DEV CART: project created in ${dir}.`, 'Open Folder');
        if (answer === 'Open Folder') {
            await vscode.commands.executeCommand('vscode.openFolder', target);
        }
    }
    return dir;
}

// ---------------------------------------------------------------------------
// PNG to TIM
// ---------------------------------------------------------------------------

function parsePair(text) {
    const m = /^\s*(\d+)[\s,]+(\d+)\s*$/.exec(text || '');
    return m ? [m[1], m[2]] : undefined;
}

// options (for scripts/tests): { like: true, palette: true } or { org: [x, y], plt: [x, y] }
async function convertPngToTim(context, uri, options) {
    const png = (uri || (vscode.window.activeTextEditor && vscode.window.activeTextEditor.document.uri));
    if (!png || !/\.png$/i.test(png.fsPath)) {
        showError('PS1 DEV CART: choose a .png file to convert.');
        return undefined;
    }
    const base = png.fsPath.replace(/\.png$/i, '');
    const existing = ['.tim', '.TIM'].map(e => base + e).find(p => fs.existsSync(p));
    const tim = existing || base + '.tim';

    let opts = options;
    if (!opts) {
        const items = [];
        if (existing) {
            items.push({ label: `Same settings as ${path.basename(existing)}`, value: { like: true } });
            items.push({ label: `Same settings and palette as ${path.basename(existing)}`,
                         description: 'keeps the CLUT order', value: { like: true, palette: true } });
        }
        items.push({ label: 'Enter VRAM positions...', value: {} });
        const pick = items.length === 1 ? items[0] : await vscode.window.showQuickPick(items, { placeHolder: 'Convert to TIM' });
        if (!pick) {
            return undefined;
        }
        opts = pick.value;
        if (!opts.like) {
            const org = await vscode.window.showInputBox({
                prompt: 'Image position in VRAM (X Y)', placeHolder: 'e.g. 640 0',
                validateInput: v => (parsePair(v) ? null : 'Two numbers: X Y'),
            });
            if (org === undefined) {
                return undefined;
            }
            const plt = await vscode.window.showInputBox({
                prompt: 'CLUT position in VRAM (X Y); leave empty for a 16-bit image', placeHolder: 'e.g. 0 480',
                validateInput: v => (!v.trim() || parsePair(v) ? null : 'Two numbers: X Y, or empty'),
            });
            if (plt === undefined) {
                return undefined;
            }
            opts = { org: parsePair(org), plt: parsePair(plt) };
        }
    }

    try {
        const script = await toLinuxPath(path.join(context.extensionPath, 'tools', 'png2tim.py'));
        const linuxPng = await toLinuxPath(png.fsPath);
        const linuxTim = await toLinuxPath(tim);
        const args = ['python3', script, linuxPng, '-o', linuxTim];
        if (opts.like) {
            args.push('--like', linuxTim);
            if (opts.palette) {
                args.push('-pal', linuxTim);
            }
        } else {
            args.push('-org', ...opts.org);
            if (opts.plt) {
                args.push('-plt', ...opts.plt);
            }
        }
        const { stdout } = await runLinux(args);
        output.appendLine(stdout.trim());
        vscode.window.showInformationMessage(`PS1 DEV CART: ${path.basename(tim)} written.`);
        return tim;
    } catch (e) {
        const message = describeError(e);
        output.appendLine(message);
        showError('PS1 DEV CART: PNG to TIM failed: ' + message);
        return undefined;
    }
}

function describeError(e) {
    return ((e.stderr || '').trim() || e.message || String(e)).split('\n').pop();
}

// A line in the Net Yaroze output channel (and the console, e.g. in tests).
function log(message) {
    if (output) {
        output.appendLine(message);
    }
    console.log(message);
}

// Shows an error and also keeps it in the Net Yaroze output channel.
function showError(message) {
    if (output) {
        output.appendLine(message);
    }
    console.error(message);
    return vscode.window.showErrorMessage(message);
}

// ---------------------------------------------------------------------------
// Debugging
//
// PCSX-Redux runs the game with its GDB server on; gdb-multiarch (in WSL)
// is driven by Microsoft's C/C++ extension ('cppdbg'). Modern GDB can't use
// the program's ECOFF debug information, so yaroze_debuginfo.py makes a
// debug symbol file (ELF with DWARF line information) from the program and
// its object files. On Windows 10, WSL can't connect to Windows ports, so
// GDB reaches the emulator through debug/gdbbridge.ps1 (a pipe to the port).
// ---------------------------------------------------------------------------

const emulators = new Map();  // debug id -> emulator process
const startedDebugIds = new Set();
const debugSessions = new Map();    // ps1devcartDebugId -> its debug session
let debugId = 0;

function killEmulator(proc, now) {
    if (!proc || proc.exitCode !== null) {
        return;
    }
    // ps1transfer -gdb ends by itself once GDB has gone (breakpoints removed, the game goes on): stopping it while
    // it talks to the cartridge could leave the console waiting, so it is only forced if it hasn't ended
    if (proc.ps1devcartPs1transfer && !now) {
        setTimeout(() => killEmulator(proc, true), 8000);
        return;
    }
    if (onWindows) {
        // pcsx-redux.exe starts pcsx-redux.main: end the whole process tree
        cp.execFile('taskkill', ['/T', '/F', '/PID', String(proc.pid)], { windowsHide: true }, () => {});
    } else {
        proc.kill();
    }
}

// Waits until something listens on the port (Windows only; in WSL it can't be seen).
async function waitForPort(port, ms) {
    if (!onWindows) {
        await new Promise(r => setTimeout(r, 3000));
        return true;
    }
    const net = require('net');
    const end = Date.now() + ms;
    while (Date.now() < end) {
        const ok = await new Promise(resolve => {
            const socket = net.connect(port, '127.0.0.1', () => { socket.destroy(); resolve(true); });
            socket.on('error', () => resolve(false));
        });
        if (ok) {
            return true;
        }
        await new Promise(r => setTimeout(r, 300));
    }
    return false;
}

async function projectForDebug(folder, config) {
    if (folder && typeof config.project === 'string') {
        return makeProject(folder, path.join(folder.uri.fsPath, config.project));
    }
    await findProjects();
    const inFolder = folder ? projects.filter(p => p.folder.uri.toString() === folder.uri.toString()) : [];
    return inFolder.length === 1 ? inFolder[0] : pickProject();
}

const debugFail = message => {
    showError('PS1 DEV CART: ' + message);
    return undefined;
};

// What every debug session needs: the C/C++ extension, GDB in WSL, the emulator or ps1transfer, the build and
// the debug symbol file. Resolves { project, program, elf, linuxElf, linuxDir, ps1transfer }, or undefined after
// an error (shown).
async function prepareDebugBuild(context, project, onPs1) {
    const cpptools = vscode.extensions.getExtension('ms-vscode.cpptools');
    if (!cpptools) {
        return debugFail("debugging needs Microsoft's C/C++ extension (ms-vscode.cpptools).");
    }
    try {
        await cpptools.activate();  // so that its 'cppdbg' debugger is registered
    } catch (e) {
        return debugFail("Microsoft's C/C++ extension could not be started: " + describeError(e));
    }
    // GDB: for PsyQ/PSn00bSDK programs Windows' gdb-multiarch (PSX.Dev toolchain) when there is one, else WSL's
    const native = isYaroze(project) ? '' : nativeGdb();
    const gdb = settings().get('gdbPath');
    if (!native) {
        try {
            await runLinux(['test', '-x', gdb]);
        } catch (e) {
            return debugFail(isYaroze(project) ? `${gdb} not found in WSL: install it with "sudo apt install gdb-multiarch".`
                : 'no GDB found: install the PSX.Dev toolchain (its gdb-multiarch, PS1 DEV CART: Windows GDB Path setting), ' +
                  `or gdb-multiarch in WSL (${gdb}).`);
        }
    }
    let ps1transfer;
    if (onPs1 ? !(ps1transfer = await ps1transferPath()) : !(await checkEmulator(project))) {
        return undefined;
    }

    // Build (unless it is up to date)
    const code = await buildIfNeeded(project, true);
    if (code !== 0) {
        return debugFail('the build failed, so debugging was not started.');
    }
    if (isYaroze(project)) {
        return debugSymbols(context, project, path.join(project.dir, 'main.exe'), ps1transfer);
    }
    // PsyQ/PSn00bSDK: the build's ELF has the debug information
    const program = programOf(project);
    if (!program || !fs.existsSync(program.elf)) {
        return debugFail(`the program or its ELF (debug information) is missing after the build of ${project.name}` +
                         (program ? ` (${path.basename(program.elf)})` : '') + '.');
    }
    const info = { project, program: program.elf, elf: program.elf, exe: program.exe, ps1transfer, native };
    if (!native) {
        try {
            info.linuxElf = await toLinuxPath(program.elf);
            info.linuxDir = await toLinuxPath(project.dir);
        } catch (e) {
            return debugFail('WSL paths: ' + describeError(e));
        }
    }
    return info;
}

// The debug symbol file (ELF with DWARF lines) made from the linked program (ECOFF)
async function debugSymbols(context, project, program, ps1transfer) {
    const elf = program.replace(/\.[^.\\/]*$/, '') + '.elf';
    const psx = path.join(project.dir, 'psx.exe');
    if (!fs.existsSync(program) || !fs.existsSync(psx)) {
        return debugFail(`${path.basename(program)} or psx.exe is missing after the build.`);
    }
    let linuxElf;
    let linuxDir;
    try {
        linuxDir = await toLinuxPath(project.dir);
        linuxElf = await toLinuxPath(elf);
        const script = await toLinuxPath(path.join(context.extensionPath, 'tools', 'yaroze_debuginfo.py'));
        const { stdout } = await runLinux(['python3', script, await toLinuxPath(program), '-o', linuxElf,
                                           '--toolchain', settings().get('toolchainPath'),
                                           '--locations', linuxElf + '.json']);
        output.appendLine(stdout.trim());
    } catch (e) {
        return debugFail('making the debug symbols failed: ' + describeError(e));
    }
    return { project, program, elf, linuxElf, linuxDir, ps1transfer };
}

// The 'cppdbg' configuration of GDB (in WSL, driven by the C/C++ extension) connected to a GDB server port: the
// emulator's or ps1transfer's. id: the emulator / ps1transfer process to close with the session (emulators map).
async function cppdbgConfig(context, info, { name, port, onPs1, stopAtEntry, debugLog, id }) {
    if (info.native) {
        return nativeCppdbgConfig(info, { name, port, onPs1, stopAtEntry, debugLog, id });
    }
    const gdb = settings().get('gdbPath');
    let bridge = path.join(context.extensionPath, 'debug', 'gdbbridge.ps1');
    if (!onWindows) {
        bridge = (await runLinux(['wslpath', '-w', bridge])).stdout.trim();
    }
    bridge = bridge.replace(/\\/g, '/');
    const commands = [
        { text: `-file-exec-and-symbols "${info.linuxElf}"`, description: 'Load the debug symbols', ignoreFailures: false },
        { text: `target remote | powershell.exe -NoProfile -ExecutionPolicy Bypass -File '${bridge}' ${port}`,
          description: onPs1 ? 'Connect to the PS1' : 'Connect to PCSX-Redux', ignoreFailures: false },
    ];
    if (stopAtEntry) {
        commands.push({ text: 'tbreak main', description: 'Stop at main', ignoreFailures: true });
    }
    const cppdbg = {
        type: 'cppdbg',
        request: 'launch',
        name: name || 'PS1 DEV CART: Debug',
        program: info.linuxElf,
        cwd: info.linuxDir,
        MIMode: 'gdb',
        miDebuggerPath: gdb,
        targetArchitecture: 'mips',
        stopAtEntry: false,
        externalConsole: false,
        setupCommands: [{ text: 'set pagination off', ignoreFailures: true }],
        customLaunchSetupCommands: commands,
        launchCompleteCommand: 'exec-continue',
        ps1devcartDebugId: id,
        ps1devcartLocations: info.elf + '.json',
    };
    if (debugLog) {
        cppdbg.logging = { engineLogging: true, trace: true, traceResponse: true };
    }
    if (onWindows) {
        const distro = settings().get('wslDistribution');
        cppdbg.pipeTransport = {
            pipeCwd: '',
            pipeProgram: 'wsl',
            // GDB and its options go to WSL's shell unquoted, as one command line
            pipeArgs: distro ? ['-d', distro] : [],
            debuggerPath: gdb,
            quoteArgs: false,
        };
        // GDB reports WSL paths; open the Windows files
        cppdbg.sourceFileMap = { [await toLinuxPath(info.project.folder.uri.fsPath)]: info.project.folder.uri.fsPath };
    }
    // If the debug session never starts (the debugger failed), close the emulator (or ps1transfer)
    setTimeout(() => {
        if (!startedDebugIds.has(id) && emulators.has(id)) {
            killEmulator(emulators.get(id));
            emulators.delete(id);
        }
    }, 60000);
    return cppdbg;
}

// The same with Windows' gdb-multiarch (PsyQ/PSn00bSDK program, PSX.Dev toolchain): it reaches the GDB server port
// directly
function nativeCppdbgConfig(info, { name, port, onPs1, stopAtEntry, debugLog, id }) {
    const elf = info.elf.replace(/\\/g, '/');
    const commands = [
        { text: `-file-exec-and-symbols "${elf}"`, description: 'Load the program and its debug information', ignoreFailures: false },
        { text: `target remote localhost:${port}`, description: onPs1 ? 'Connect to the PS1' : 'Connect to PCSX-Redux',
          ignoreFailures: false },
    ];
    if (stopAtEntry) {
        commands.push({ text: 'tbreak main', description: 'Stop at main', ignoreFailures: true });
    }
    const cppdbg = {
        type: 'cppdbg',
        request: 'launch',
        name: name || 'PS1 DEV CART: Debug',
        program: info.elf,
        cwd: info.project.dir,
        MIMode: 'gdb',
        miDebuggerPath: info.native,
        targetArchitecture: 'mips',
        stopAtEntry: false,
        externalConsole: false,
        setupCommands: [{ text: 'set pagination off', ignoreFailures: true }],
        customLaunchSetupCommands: commands,
        launchCompleteCommand: 'exec-continue',
        ps1devcartDebugId: id,
        ps1devcartLocations: info.elf + '.json',
    };
    if (debugLog) {
        cppdbg.logging = { engineLogging: true, trace: true, traceResponse: true };
    }
    setTimeout(() => {
        if (!startedDebugIds.has(id) && emulators.has(id)) {
            killEmulator(emulators.get(id));
            emulators.delete(id);
        }
    }, 60000);
    return cppdbg;
}

// The emulator program (a Windows path on Windows) of a project
async function emulatorProgram(project) {
    let emulatorPath = (await emulatorFor(project)).path;
    if (onWindows && emulatorPath && emulatorPath.startsWith('/')) {
        emulatorPath = await toHostPath(emulatorPath);
    }
    return emulatorPath;
}

// Turns a 'ps1devcart' launch configuration into a 'cppdbg' one: builds,
// makes the debug symbols, starts the emulator (or ps1transfer for a PS1
// with a PS1 DEV CART cartridge, "target": "devcart"), and sets up GDB.
async function prepareDebug(context, folder, config) {
    const onPs1 = isCartTarget(config.target);
    const project = await projectForDebug(folder, config);
    if (!project) {
        return undefined;
    }
    let info = await prepareDebugBuild(context, project, onPs1);
    if (info && isYaroze(project) && config.program && config.program !== 'main.exe') {
        info = await debugSymbols(context, project, path.join(project.dir, config.program), info.ps1transfer);
    }
    if (!info) {
        return undefined;
    }
    const port = settings().get('gdbPort');
    const id = `ps1devcart-${++debugId}`;
    let targetName;
    if (onPs1) {
        // PS1 with a PS1 DEV CART cartridge: ps1transfer loads psx.exe stopped at its entry point
        const { proc, ready } = startPs1transfer(info.ps1transfer, project, port);
        emulators.set(id, proc);
        if (!(await ready)) {
            killEmulator(proc, true);
            emulators.delete(id);
            return debugFail('ps1transfer could not load the program on the PS1 (see the PS1 DEV CART output).');
        }
        targetName = 'the PS1 (PS1 DEV CART' + (cartSerial() ? ' ' + cartSerial() : '') + ')';
    } else {
        // Emulator, paused at the start of the program, with its GDB server on
        let emulatorPath;
        try {
            emulatorPath = await emulatorProgram(project);
        } catch (e) {
            return debugFail('the emulator could not be found: ' + describeError(e));
        }
        const proc = cp.spawn(emulatorPath, ['-gdb', '-gdb-port', String(port), '-debugger', '-loadexe', programArg(project)],
                              { cwd: project.dir, stdio: 'ignore' });
        proc.on('error', e => showError('PS1 DEV CART: the emulator did not start: ' + e.message));
        emulators.set(id, proc);
        if (!(await waitForPort(port, 20000))) {
            killEmulator(proc);
            emulators.delete(id);
            return debugFail(`the emulator's GDB server didn't start on port ${port}.`);
        }
        targetName = emulatorPath;
    }
    output.appendLine(`Debugging ${path.basename(info.program)} on ${targetName} (GDB server on port ${port})`);
    return cppdbgConfig(context, info, { name: config.name, port, onPs1, stopAtEntry: config.stopAtEntry,
                                         debugLog: config.debugLog, id });
}

function debugConfigurationProvider(context) {
    return {
        provideDebugConfigurations() {
            return [{ type: 'ps1devcart', request: 'launch', name: 'PS1 DEV CART: Debug in PCSX-Redux' },
                    { type: 'ps1devcart', request: 'launch', name: 'PS1 DEV CART: Debug on PS1', target: 'devcart' }];
        },
        resolveDebugConfiguration(folder, config) {
            if (!config.type && !config.request) {  // F5 without a launch.json
                Object.assign(config, { type: 'ps1devcart', request: 'launch', name: 'PS1 DEV CART: Debug in PCSX-Redux' });
            }
            return config;
        },
        resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
            return prepareDebug(context, folder, config);
        },
    };
}

// Pausing a running game usually stops in the interrupt handler or in
// libps/BIOS code, which has no source (and Step Over fails there). Then run
// on to the program's next function (or a line of main(), whose loop is
// already running), so the pause shows your code: with temporary function
// and instruction breakpoints set through the debugger (so it reports a
// normal breakpoint stop), and your own breakpoints put back at the stop.
function userFunctionBreakpoints() {
    return vscode.debug.breakpoints
        .filter(b => b instanceof vscode.FunctionBreakpoint && b.enabled)
        .map(b => Object.assign({ name: b.functionName },
                                b.condition ? { condition: b.condition } : {},
                                b.hitCondition ? { hitCondition: b.hitCondition } : {}));
}

async function resumeToProgramCode(session, threadId) {
    const locations = JSON.parse(fs.readFileSync(session.configuration.ps1devcartLocations, 'utf8'));
    const user = userFunctionBreakpoints();
    const names = new Set(user.map(b => b.name));
    await session.customRequest('setFunctionBreakpoints', {
        breakpoints: user.concat(locations.functions.filter(n => !names.has(n)).map(name => ({ name }))),
    });
    if (locations.mainLines.length) {
        await session.customRequest('setInstructionBreakpoints', {
            breakpoints: locations.mainLines.map(a => ({ instructionReference: '0x' + a.toString(16) })),
        }).catch(() => {});
    }
    await session.customRequest('continue', { threadId });
}

async function restoreBreakpoints(session) {
    await session.customRequest('setFunctionBreakpoints', { breakpoints: userFunctionBreakpoints() });
    await session.customRequest('setInstructionBreakpoints', { breakpoints: [] }).catch(() => {});
}

function pauseTracker() {
    return {
        createDebugAdapterTracker(session) {
            if (!session.configuration.ps1devcartDebugId) {
                return undefined;
            }
            let resuming = false;
            return {
                onDidSendMessage(m) {
                    if (m.type !== 'event' || m.event !== 'stopped') {
                        return;
                    }
                    if (resuming) {  // stopped in the program's code: put the breakpoints back
                        resuming = false;
                        restoreBreakpoints(session).catch(e => log('Restoring breakpoints failed: ' + (e.message || e)));
                        return;
                    }
                    if (m.body.reason !== 'pause' || !settings().get('pauseInProgramCode')
                        || !fs.existsSync(session.configuration.ps1devcartLocations || '')) {
                        return;
                    }
                    const threadId = m.body.threadId || 1;
                    session.customRequest('stackTrace', { threadId, levels: 1 }).then(trace => {
                        const top = trace.stackFrames[0];
                        if (!top || top.source) {
                            return undefined;  // already in code with source
                        }
                        log('Paused outside the program\'s code: running on to its next function');
                        resuming = true;
                        return resumeToProgramCode(session, threadId);
                    }).catch(e => {
                        resuming = false;
                        log('Running on to the program\'s code failed: ' + (e.message || e));
                    });
                },
            };
        },
    };
}

// target: undefined (emulator) or 'devcart' (PS1 with a PS1 DEV CART cartridge)
async function startDebugging(target) {
    const project = await pickProject();
    if (!project) {
        return false;
    }
    return vscode.debug.startDebugging(project.folder, Object.assign({
        type: 'ps1devcart', request: 'launch', name: target ? 'PS1 DEV CART: Debug on PS1' : 'PS1 DEV CART: Debug', project: project.rel,
    }, target ? { target } : {}));
}

// ---------------------------------------------------------------------------
// Link play on two emulators or two PS1 DEV CART cartridges, reset, and the
// PS1 DEV CART panel (activity bar): the cartridges connected, a button for
// each action.
// ---------------------------------------------------------------------------

let extensionPath;

// Second cartridge of link play, and how the two consoles are linked: 'cable' (link cable, the serial port TTY
// driver like the Net Yaroze boot disc) or 'usb' (no cable: the same driver over the USB link, ps1transfer -relay)
const cartSerial2 = () => settings().get('devcartSerial2') || '';
const cartLink = () => (settings().get('devcartLink') === 'usb' ? 'usb' : 'cable');
const linkTtyArgs = () => (cartLink() === 'usb' ? ['-c', 'link'] : ['-c', 'sio']);

// A Windows path for powershell.exe (from WSL: wslpath -w)
async function windowsPath(p) {
    return onWindows ? p : (await runLinux(['wslpath', '-w', p])).stdout.trim();
}

// Runs a program, its output in the Net Yaroze output channel; resolves with the exit code (-1: didn't start).
function runProcess(exe, args, cwd, label) {
    output.appendLine(`> ${label ? label + ': ' : ''}${exe} ${args.join(' ')}`);
    return new Promise(resolve => {
        const proc = cp.spawn(exe, args, { cwd, windowsHide: true });
        const show = data => output.append(data.toString().replace(/[^\n]*bytes remaining\.\.\.\r/g, '').replace(/\r/g, ''));
        proc.stdout.on('data', show);
        proc.stderr.on('data', show);
        proc.on('error', e => {
            output.appendLine(`${exe} did not start: ${e.message}`);
            resolve(-1);
        });
        proc.on('exit', code => resolve(code === null ? -1 : code));
    });
}

// An emulator started by the launcher script (not a child process): ended with taskkill
function processById(pid) {
    const kill = () => cp.execFile(onWindows ? 'taskkill' : 'taskkill.exe', ['/T', '/F', '/PID', String(pid)],
                                   { windowsHide: true }, () => {});
    return { pid, exitCode: null, kill };
}

// Builds the project, checks psx.exe is there; resolves false after an error (shown).
async function buildForRun(project, what) {
    if ((await buildIfNeeded(project, false)) !== 0) {
        showError(`PS1 DEV CART: the build failed, so the game was not ${what}.`);
        return false;
    }
    const program = programOf(project);
    if (!program || !fs.existsSync(program.exe)) {
        showError(isYaroze(project) ? 'PS1 DEV CART: psx.exe is missing after the build (made by yarexe from the auto script).'
            : `PS1 DEV CART: no PS-X EXE found after the build of ${project.name}` +
              (project.kind === 'cmake' ? ' (in its build folder).' : ' (TARGET.ps-exe of its Makefile).'));
        return false;
    }
    return true;
}

// The two cartridges of link play (Cart and Second cart in the panel): [serial1, serial2], undefined after an error
function linkCarts() {
    const a = cartSerial();
    const b = cartSerial2();
    if (!a || !b || a === b) {
        showError('PS1 DEV CART: link play on two PS1 DEV CART cartridges needs two different cartridges: choose ' +
                  'them in the PS1 DEV CART panel (Cart and Second cart), or with PS1 DEV CART: Select Cart... ' +
                  'and Select Second Cart...');
        return undefined;
    }
    return [a, b];
}

// The launcher script of two linked emulators (debug/run-linked.ps1), as powershell.exe arguments. exe: the program,
// relative to the project folder (default: the project's)
async function linkedEmulatorArgs(project, extra, exe) {
    const emulator = await emulatorProgram(project);
    const script = await windowsPath(path.join(extensionPath, 'debug', 'run-linked.ps1'));
    return ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script,
            '-Emulator', await windowsPath(emulator), '-Exe', exe || programArg(project), ...extra];
}

// ----- Link play of an unmodified Net Yaroze game in the emulator -----
//
// On a Net Yaroze the boot disc's monitor installs its serial port tty ("tty:", the link cable); on a PS1 DEV CART
// the firmware's (-c sio). A psx.exe run in the emulator has neither. For Run Linked and Debug Linked of a Net Yaroze
// project, the extension makes a copy of psx.exe with the link driver added (linkdriver/: yario_emu.c from
// yaroze-racer, and a loader): its entry point is the loader, which copies the driver to kernel RAM (a part no BIOS
// uses), installs it as the BIOS's "tty" device, then starts the game at its own entry point. The game's code is not
// changed (its addresses, so its debug symbols, stay the same), and the same build runs on a console: no build flag.
// (The driver calls the Net Yaroze library that yarexe puts in psx.exe: other projects run their own program.)

const LINK_EXE = path.join('.pcsx-link', 'psx-link.exe');

// The program of link play in the emulator, relative to the project folder: for a Net Yaroze project, psx.exe with
// the link driver (written to LINK_EXE), else the project's own. Undefined after an error (shown).
function makeLinkExe(project) {
    if (!isYaroze(project)) {
        return programArg(project);
    }
    try {
        const dir = path.join(extensionPath, 'linkdriver');
        const [addr, size] = fs.readFileSync(path.join(dir, 'linkdriver.size'), 'utf8').trim().split(/\s+/);
        const loader = fs.readFileSync(path.join(dir, 'loader.bin'));
        const driver = fs.readFileSync(path.join(dir, 'linkdriver.bin'));
        const exe = fs.readFileSync(programOf(project).exe);
        if (exe.toString('latin1', 0, 8) !== 'PS-X EXE') {
            throw new Error('psx.exe is not a PS-X EXE');
        }
        const pc = exe.readUInt32LE(0x10);
        const tAddr = exe.readUInt32LE(0x18);
        const tSize = exe.readUInt32LE(0x1c);
        // Table after the loader: the driver's address, its size, the room cleared for it, the game's entry point
        const table = Buffer.alloc(16);
        table.writeUInt32LE(parseInt(addr, 16), 0);
        table.writeUInt32LE((driver.length + 3) & ~3, 4);
        table.writeUInt32LE(Math.max(Number(size), driver.length) + 3 & ~3, 8);
        table.writeUInt32LE(pc, 12);
        let added = Buffer.concat([loader, table, driver]);
        added = Buffer.concat([added, Buffer.alloc((0x800 - added.length % 0x800) % 0x800)]);
        const out = Buffer.concat([exe.subarray(0, 0x800 + tSize), added]);
        out.writeUInt32LE(tAddr + tSize, 0x10);         // entry point: the loader
        out.writeUInt32LE(tSize + added.length, 0x1c);
        fs.mkdirSync(path.join(project.dir, '.pcsx-link'), { recursive: true });
        fs.writeFileSync(path.join(project.dir, LINK_EXE), out);
        output.appendLine(`Link play: ${LINK_EXE} (psx.exe with the link driver at 0x${addr}, its loader at ` +
                          `0x${(tAddr + tSize).toString(16)}, the game from 0x${pc.toString(16)})`);
        return LINK_EXE;
    } catch (e) {
        showError('PS1 DEV CART: the copy of psx.exe for link play could not be made: ' + describeError(e));
        return undefined;
    }
}

// Run Linked: the makefile's "link" target, or (without one) the extension's own launcher of two linked emulators.
async function runLinkedEmulators() {
    const project = await pickProject();
    if (!project || !(await checkEmulator(project))) {
        return undefined;
    }
    if (hasLinkTarget(project)) {
        return runTaskFor(project, 'link');
    }
    // (Net Yaroze: psx.exe with the link driver, the serial port tty of the boot disc)
    const exe = (await buildForRun(project, 'run')) && makeLinkExe(project);
    if (!exe) {
        return undefined;
    }
    let args;
    try {
        args = await linkedEmulatorArgs(project, [], exe);
    } catch (e) {
        showError('PS1 DEV CART: the emulator could not be found: ' + describeError(e));
        return undefined;
    }
    const execution = new vscode.ProcessExecution('powershell.exe', args, { cwd: project.dir });
    const def = Object.assign({ type: TASK_TYPE, target: 'runLinkedEmulators' }, project.rel ? { project: project.rel } : {});
    const task = new vscode.Task(def, project.folder, 'Run Linked' + (project.rel ? ` (${project.rel})` : ''),
                                 'PS1 DEV CART', execution, []);
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Shared, clear: true };
    return vscode.tasks.executeTask(task);
}

// Starts two debug sessions (GDB in WSL, one per GDB server port), named after what they debug
async function startTwoSessions(project, info, targets, onPs1) {
    for (const t of targets) {
        const config = await cppdbgConfig(extensionContext, info, { name: t.name, port: t.port, onPs1, id: t.id,
                                                                    stopAtEntry: false, debugLog: false });
        output.appendLine(`Debugging ${path.basename(info.program)} on ${t.where} (GDB server on port ${t.port})`);
        await vscode.debug.startDebugging(project.folder, config);
    }
}

// Debug Linked: the game in two linked emulators (SIO1 server and client), each with its GDB server (gdbPort and
// gdbPort + 1), paused until both debuggers run it.
async function debugLinkedEmulators() {
    const project = await pickProject();
    if (!project) {
        return undefined;
    }
    // (Net Yaroze: psx.exe with the link driver; the game's code is the same, so are its debug symbols)
    const info = await prepareDebugBuild(extensionContext, project, false);
    const exe = info && makeLinkExe(project);
    if (!exe) {
        return undefined;
    }
    const port = settings().get('gdbPort');
    let args;
    try {
        args = await linkedEmulatorArgs(project, ['-ServerGdbPort', String(port), '-ClientGdbPort', String(port + 1),
                                                  '-NoRun'], exe);
    } catch (e) {
        return debugFail('the emulator could not be found: ' + describeError(e));
    }
    let text = '';
    const code = await new Promise(resolve => {
        output.appendLine(`> powershell.exe ${args.join(' ')}`);
        const proc = cp.spawn('powershell.exe', args, { cwd: project.dir, windowsHide: true });
        proc.stdout.on('data', d => { text += d; output.append(d.toString()); });
        proc.stderr.on('data', d => output.append(d.toString()));
        proc.on('error', e => { output.appendLine('powershell did not start: ' + e.message); resolve(-1); });
        proc.on('exit', c => resolve(c));
    });
    const pids = [...text.matchAll(/\(pid (\d+)\)/g)].map(m => Number(m[1]));
    const ids = pids.map(pid => {
        const id = `ps1devcart-${++debugId}`;
        emulators.set(id, processById(pid));
        return id;
    });
    const stopAll = () => ids.forEach(id => { killEmulator(emulators.get(id), true); emulators.delete(id); });
    if (code !== 0 || pids.length !== 2) {
        stopAll();
        return debugFail('the two linked emulators did not start (see the PS1 DEV CART output).');
    }
    for (const p of [port, port + 1]) {
        if (!(await waitForPort(p, 20000))) {
            stopAll();
            return debugFail(`an emulator's GDB server didn't start on port ${p}.`);
        }
    }
    return startTwoSessions(project, info, [
        { name: 'PS1 DEV CART: Debug Linked (emulator 1, SIO1 server)', port, id: ids[0], where: 'emulator 1 (SIO1 server)' },
        { name: 'PS1 DEV CART: Debug Linked (emulator 2, SIO1 client)', port: port + 1, id: ids[1],
          where: 'emulator 2 (SIO1 client)' },
    ], false);
}

// Run Linked on two cartridges: psx.exe started on both at once, with the serial port TTY driver (link cable), or
// its USB link version and the relay between the two cartridges (no cable, the relay runs in a terminal).
async function runLinkedOnCarts() {
    const project = await pickProject();
    const carts = project && linkCarts();
    const exe = carts && await ps1transferPath();
    if (!exe || !(await buildForRun(project, 'run'))) {
        return undefined;
    }
    for (const serial of carts) {       // (e.g. the relay of the last run: it has both carts open)
        await releaseCart(serial);
    }
    output.show(true);
    // Both at once (each cartridge has its own USB link); a start that fails is tried once more (a reset can leave
    // the cartridge busy for a moment)
    const start = async serial => {
        const args = ['-n', serial, '-r', '-e', '-f', programArg(project), ...linkTtyArgs()];
        const code = await runProcess(exe, args, project.dir, serial);
        if (code === 0) {
            return code;
        }
        await new Promise(r => setTimeout(r, 1500));
        return runProcess(exe, args, project.dir, serial + ' (again)');
    };
    const codes = await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Notification, title: `PS1 DEV CART: starting the game on ${carts.join(' and ')}` },
        () => Promise.all(carts.map(start)));
    const failed = carts.filter((s, i) => codes[i] !== 0);
    if (failed.length) {
        showError(`PS1 DEV CART: the game could not be started on ${failed.join(' and ')} (see the PS1 DEV CART output).`);
        return undefined;
    }
    if (cartLink() !== 'usb') {
        vscode.window.setStatusBarMessage(`PS1 DEV CART: link game running on ${carts.join(' and ')} (link cable)`, 6000);
        return codes;
    }
    // USB link: the relay joins the two consoles' links through this computer, until it is stopped
    startRelay(exe, carts, project.dir);
    return codes;
}

// The USB link relay of link play (ps1transfer -relay), run as the PS1 Dev Cart Manager does: its standard input a
// pipe kept open, a "quit" line stops it cleanly (never in the middle of a USB transfer; a terminal's input could be
// taken for it at once), its output in the PS1 DEV CART output; a status bar item while it runs (a click stops it).
let relayProc;
let relayItem;

function startRelay(exe, carts, cwd) {
    if (relayProc) {
        stopRelay();
    }
    const args = ['-n', carts[0], '-relay', carts[1]];
    output.appendLine(`> ${exe} ${args.join(' ')}`);
    const proc = cp.spawn(exe, args, { cwd, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
    proc.ps1devcartSerials = carts;
    // (its throughput line each second left out)
    const show = data => output.append(data.toString().replace(/\r/g, '').split('\n')
        .filter(l => !/ B\/s /.test(l)).join('\n'));
    proc.stdout.on('data', show);
    proc.stderr.on('data', show);
    proc.on('error', e => output.appendLine('The USB link relay did not start: ' + e.message));
    proc.on('exit', code => {
        output.appendLine(`USB link relay ended (exit code ${code}).`);
        if (relayProc === proc) {
            relayProc = undefined;
            relayItem.hide();
        }
    });
    relayProc = proc;
    relayItem.text = `$(link) Relay ${carts.join(' \u21c4 ')}`;
    relayItem.tooltip = 'PS1 DEV CART: the USB link relay between the two carts runs (click to stop it)';
    relayItem.show();
    vscode.window.setStatusBarMessage(`PS1 DEV CART: link game running on ${carts.join(' and ')} (USB link relay)`, 6000);
}

// Stops the relay ("quit", then ended after 2 s); resolves when it has ended
function stopRelay() {
    const proc = relayProc;
    if (!proc) {
        return Promise.resolve(false);
    }
    relayProc = undefined;
    relayItem.hide();
    return new Promise(resolve => {
        const timer = setTimeout(() => { try { proc.kill(); } catch (e) { /* (ended) */ } resolve(true); }, 2000);
        proc.once('exit', () => { clearTimeout(timer); resolve(true); });
        try {
            proc.stdin.write('quit\n');
            proc.stdin.end();
        } catch (e) {
            proc.kill();
        }
    });
}

// Debug Linked on two cartridges: the game run on the second cart, then debugged on the main cart (ps1transfer -gdb,
// stopped at its entry point, one debug session). Over the USB link, that GDB server also runs the relay between the
// two carts (-relay: the main cart's USB is the debugger's; firmware with the debug monitor version 12).
async function debugLinkedOnCarts() {
    const project = await pickProject();
    const carts = project && linkCarts();
    if (!carts) {
        return undefined;
    }
    const info = await prepareDebugBuild(extensionContext, project, true);
    if (!info) {
        return undefined;
    }
    for (const serial of carts) {
        await releaseCart(serial);
    }
    output.show(true);
    // The second cart: the game runs, waiting for the link
    const args = ['-n', carts[1], '-r', '-e', '-f', programArg(project), ...linkTtyArgs()];
    let code = await runProcess(info.ps1transfer, args, project.dir, carts[1]);
    if (code !== 0) {
        await new Promise(r => setTimeout(r, 1500));
        code = await runProcess(info.ps1transfer, args, project.dir, carts[1] + ' (again)');
    }
    if (code !== 0) {
        return debugFail(`the game could not be started on ${carts[1]} (see the PS1 DEV CART output).`);
    }
    // The main cart: debugged (the relay in its session over the USB link)
    const port = settings().get('gdbPort');
    const id = `ps1devcart-${++debugId}`;
    const relay = cartLink() === 'usb' ? ['-relay', carts[1]] : [];
    const { proc, ready } = startPs1transfer(info.ps1transfer, project, port, carts[0], [...linkTtyArgs(), ...relay]);
    proc.ps1devcartSerials = relay.length ? carts : [carts[0]];
    emulators.set(id, proc);
    if (!(await ready)) {
        killEmulator(proc, true);
        emulators.delete(id);
        return debugFail(`ps1transfer could not load the program on ${carts[0]} (see the PS1 DEV CART output).`);
    }
    return startTwoSessions(project, info, [{
        name: `PS1 DEV CART: Debug Linked (${carts[0]}, linked with ${carts[1]})`, port, id,
        where: `the PS1 of ${carts[0]} (linked with ${carts[1]}${relay.length ? ' through the USB link relay' : ''})`,
    }], true);
}

// Stops what the extension runs on a cartridge ('' = any), as the PS1 Dev Cart Manager stops its console before a
// reset: Run on PS1's console terminal, the USB link relay, a debug session on it (its ps1transfer -gdb).
// Resolves the names of what was stopped.
async function releaseCart(serial) {
    const uses = s => !serial || !s || s === serial;
    const stopped = [];
    const ending = [];
    for (const [execution, serials] of cartTasks) {
        if (serials.some(uses)) {
            stopped.push(execution.task.name);
            ending.push(new Promise(resolve => {
                const done = vscode.tasks.onDidEndTask(e => {
                    if (e.execution === execution) {
                        done.dispose();
                        resolve();
                    }
                });
                setTimeout(() => { done.dispose(); resolve(); }, 3000);
            }));
            execution.terminate();
            cartTasks.delete(execution);
        }
    }
    if (relayProc && relayProc.ps1devcartSerials.some(uses)) {
        stopped.push('the USB link relay');
        ending.push(stopRelay());
    }
    for (const [id, proc] of emulators) {
        if (proc && proc.ps1devcartPs1transfer && proc.exitCode === null &&
            (proc.ps1devcartSerials || [proc.ps1devcartSerial]).some(uses)) {
            stopped.push('the debug session');
            const session = debugSessions.get(id);
            if (session) {
                vscode.debug.stopDebugging(session);
            }
            ending.push(new Promise(resolve => {
                proc.once('exit', resolve);
                setTimeout(resolve, 3000);
            }));
            killEmulator(proc, true);
            emulators.delete(id);
        }
    }
    await Promise.all(ending);
    if (stopped.length) {
        await new Promise(r => setTimeout(r, 500));     // (the USB device closed by the driver)
    }
    return stopped;
}

// Resets the console of the cartridge (serial: another one than the selected cartridge)
async function resetCart(serial) {
    const exe = await ps1transferPath();
    if (!exe) {
        return undefined;
    }
    const target = typeof serial === 'string' ? serial : cartSerial();
    const stopped = await releaseCart(target);
    if (stopped.length) {
        output.appendLine(`Reset: ${[...new Set(stopped)].join(', ')} stopped first (it had the cartridge open)`);
    }
    const args = [...(target ? ['-n', target] : []), '-r'];
    let code = await runProcess(exe, args, undefined, 'Reset');
    if (code !== 0 && stopped.length) {     // (the cartridge just released can need a moment)
        await new Promise(r => setTimeout(r, 1500));
        code = await runProcess(exe, args, undefined, 'Reset (again)');
    }
    if (code === 0) {
        vscode.window.setStatusBarMessage(`PS1 DEV CART: ${target || 'PS1 DEV CART'} reset`, 4000);
    } else {
        showError(`PS1 DEV CART: the reset of ${target || 'the PS1 DEV CART'} failed: is the cartridge connected, ` +
                  'and not used by another program (e.g. the PS1 Dev Cart Manager)?');
    }
    return code;
}

// Chooses the second cartridge of link play (quick pick, like Select PS1 DEV CART)
async function selectSecondCart(serial) {
    if (typeof serial === 'string') {     // (not the main cart: None then)
        await settings().update('devcartSerial2', serial === cartSerial() ? '' : serial,
                                vscode.ConfigurationTarget.Global);
        updatePanel();
        return serial;
    }
    const exe = await ps1transferPath();
    if (!exe) {
        return undefined;
    }
    const carts = (await listCarts(exe)).filter(c => c.serial && c.serial !== cartSerial());
    const pick = await vscode.window.showQuickPick(carts.map(c => ({ label: c.serial, detail: c.description })),
                                                   { placeHolder: carts.length ? 'Second PS1 DEV CART (link play)'
                                                       : 'No other cartridge found' });
    if (!pick) {
        return undefined;
    }
    await settings().update('devcartSerial2', pick.label, vscode.ConfigurationTarget.Global);
    updatePanel();
    return pick.label;
}

// ----- The panel -----

let extensionContext;
let panelView;
let carts = [];             // Last list of the cartridges: [{ serial, description, inUse }]
let cartsBusy = false;
let panelTimer;

// Lists the cartridges again (the panel's list); keeps the selected ones when they are in use by another program
// (listed without their serial number then)
async function refreshCarts() {
    const exe = await ps1transferPath(true);
    if (!exe || cartsBusy) {
        updatePanel();
        return carts;
    }
    cartsBusy = true;
    updatePanel();
    try {
        const found = await listCarts(exe);
        const inUse = found.filter(c => c.inUse).length;
        const list = found.filter(c => c.serial);
        // In use cartridges: the selected ones (and the ones seen before) are kept, shown as in use
        for (const serial of [cartSerial(), cartSerial2(), ...carts.map(c => c.serial)]) {
            if (serial && inUse && !list.some(c => c.serial === serial) && list.length < found.length) {
                list.push({ serial, description: '(in use)', inUse: true });
            }
        }
        carts = list;
    } finally {
        cartsBusy = false;
        updatePanel();
    }
    return carts;
}

function updatePanel() {
    if (!panelView) {
        return;
    }
    const project = projects.length === 1 ? projects[0] : undefined;
    panelView.webview.postMessage({
        type: 'state',
        ps1transfer: !!(settings().get('ps1transferPath') || tools.ps1transferBundled()),
        carts: carts.map(c => ({ serial: c.serial, label: c.serial + (c.inUse ? ' (in use)' : '') })),
        serial: cartSerial(),
        serial2: cartSerial2(),
        link: cartLink(),
        busy: cartsBusy,
        project: projects.length === 0 ? 'No project (Net Yaroze, PsyQ or PSn00bSDK) in this workspace'
            : (project ? `Project: ${project.name} (${KIND_NAMES[project.kind]})`
                : `${projects.length} projects (the one of the file being edited)`),
        hasProject: projects.length > 0,
        emulator: tools.emulatorPath(),
        emulatorFrom: settings().get('emulatorPath') ? 'setting' : (tools.emulatorPath() ? 'installed' : ''),
        tools: toolStatus,
        groups: tools.GROUPS,
        selected: selectedGroups(),
        toolsOpen: !!(extensionContext && extensionContext.globalState.get('toolsOpen')),
    });
}

// ----- Tools (toolchains.js): the state shown in the panel, installs -----

let toolStatus = [];
let toolsBusy = false;

async function refreshTools() {
    if (toolsBusy) {
        return toolStatus;
    }
    toolsBusy = true;
    try {
        toolStatus = await tools.status();
    } finally {
        toolsBusy = false;
    }
    updatePanel();
    return toolStatus;
}

async function installTool(id) {
    const ok = await tools.installTool(id);
    await refreshTools();
    return ok;
}

// The toolchains ticked in the panel (saved; by default the ones of the workspace's projects)
function selectedGroups() {
    const saved = extensionContext && extensionContext.globalState.get('toolGroups');
    return Array.isArray(saved) ? saved : tools.defaultGroups([...new Set(projects.map(p => p.kind))]);
}

async function selectGroups(ids) {
    await extensionContext.globalState.update('toolGroups', tools.GROUPS.map(g => g.id).filter(id => ids.includes(id)));
    updatePanel();
}

// Installs the tools missing in these toolchains (ids of tools.GROUPS); resolves the ids of the ones then found
async function installGroups(groupIds) {
    const missing = await tools.missingIn(groupIds);
    for (const t of missing) {
        await tools.installTool(t.id);
    }
    const status = await refreshTools();
    if (!missing.length) {
        vscode.window.setStatusBarMessage('PS1 DEV CART: no tool missing', 4000);
    }
    return missing.map(t => t.id).filter(id => (status.find(t => t.id === id) || {}).ok);
}

// Lets the user tick the toolchains to install (the ticked ones are saved, as in the panel)
async function chooseGroups() {
    await findProjects();
    const status = await refreshTools();
    const selected = selectedGroups();
    const items = tools.GROUPS.map(g => {
        const groupTools = g.tools.map(id => status.find(t => t.id === id)).filter(Boolean);
        const missing = groupTools.filter(t => !t.ok);
        return {
            label: g.name, id: g.id, picked: selected.includes(g.id),
            description: missing.length ? 'missing: ' + missing.map(t => t.name).join(', ') : 'installed',
            detail: groupTools.map(t => t.name).join(', '),
        };
    });
    const picked = await vscode.window.showQuickPick(items, { canPickMany: true, title: 'PS1 DEV CART: Install Tools',
                                                              placeHolder: 'Tick the toolchains to install' });
    if (!picked) {
        return undefined;
    }
    await selectGroups(picked.map(i => i.id));
    return installGroups(picked.map(i => i.id));
}

// The command: the toolchains given (ids, or true: all of them), else chosen by the user
function installMissingTools(groups) {
    if (groups === true) {
        return installGroups(tools.GROUPS.map(g => g.id));
    }
    return Array.isArray(groups) ? installGroups(groups) : chooseGroups();
}

// When a workspace with projects opens: offers to install the tools they need (like PSX.Dev), once per workspace
async function offerTools(context) {
    if (!settings().get('offerTools')) {
        return;
    }
    const kinds = [...new Set((await findProjects()).map(p => p.kind))];
    const missing = (await tools.missingFor(kinds)).filter(t => !t.optional);
    const key = 'toolsOffered:' + missing.map(t => t.id).join(',');
    if (!missing.length || context.workspaceState.get(key)) {
        return;
    }
    const answer = await vscode.window.showInformationMessage(
        `PS1 DEV CART: to build ${kinds.map(k => KIND_NAMES[k]).join(', ')} projects, install ` +
        `${missing.map(t => t.name).join(', ')}?`, 'Install', 'Choose...', 'Not now', "Don't ask again");
    if (answer === 'Install') {
        // Only the toolchains of these projects (not the others)
        await installGroups(tools.GROUPS.filter(g => g.kinds.some(k => kinds.includes(k))).map(g => g.id));
    } else if (answer === 'Choose...') {
        await chooseGroups();
    } else if (answer === "Don't ask again") {
        await settings().update('offerTools', false, vscode.ConfigurationTarget.Global);
    }
    await context.workspaceState.update(key, true);
}

// Actions of the panel's buttons (commands)
const PANEL_ACTIONS = {
    templates: 'ps1devcart.newFromTemplate',
    build: 'ps1devcart.build',
    clean: 'ps1devcart.clean',
    runEmulator: 'ps1devcart.run',
    runLinkedEmulators: 'ps1devcart.runLinked',
    debugEmulator: 'ps1devcart.debug',
    debugLinkedEmulators: 'ps1devcart.debugLinked',
    runCart: 'ps1devcart.runOnPs1',
    debugCart: 'ps1devcart.debugOnPs1',
    runLinkedCarts: 'ps1devcart.runLinkedOnCarts',
    debugLinkedCarts: 'ps1devcart.debugLinkedOnCarts',
};

// The panel's buttons with an icon (media/buttons/NAME.png, made by scripts/make-icons.py: white on transparent, used
// as a mask, so that the panel colours it) and a hint shown when the mouse is over them
function iconButton(webview, action, icon, text, hint, extra = '') {
    const url = webview.asWebviewUri(vscode.Uri.file(path.join(__dirname, 'media', 'buttons', icon + '.png')));
    return `<button class="icon" data-action="${action}" data-hint="${hint}"${extra}>` +
           `<span class="ico" style="--icon: url('${url}')"></span><span>${text}</span></button>`;
}

function panelHtml(webview) {
    const nonce = [...Array(24)].map(() => Math.floor(Math.random() * 36).toString(36)).join('');
    const button = (...args) => iconButton(webview, ...args);
    return `<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource}; style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}';">
<style>
body { padding: 4px 8px 12px; color: var(--vscode-foreground); font-family: var(--vscode-font-family); font-size: var(--vscode-font-size); }
h3 { margin: 14px 0 6px; font-size: 11px; text-transform: uppercase; letter-spacing: .04em; color: #4fc1ff; }
body.vscode-light h3 { color: #0070c1; }
.row { display: flex; gap: 6px; align-items: center; margin: 4px 0; }
.row label { min-width: 74px; color: #e5c07b; }
body.vscode-light .row label { color: #9a6700; }
button.browse { background: #4fc1ff; color: #0b1e2d; }
button.browse:hover { background: #7dd3ff; }
button.clean { background: #89d185; color: #0b1e0b; }
button.templates { background: #e8912d; color: #1e1206; margin-bottom: 12px; }
button.templates:hover { background: #f0a850; }
button.clean:hover { background: #a6e3a1; }
/* Buttons with an icon: transparent, the icon light blue (darker blue in light themes) */
button.icon { display: flex; align-items: center; gap: 8px; background: transparent; color: var(--vscode-foreground);
              padding: 3px 6px; }
button.icon:hover { background: var(--vscode-list-hoverBackground, rgba(128, 128, 128, .15)); }
button.icon:disabled:hover { background: transparent; }
.ico { flex: none; width: 36px; height: 24px; background: #4fc1ff; -webkit-mask: var(--icon) center / contain no-repeat;
       mask: var(--icon) center / contain no-repeat; }
body.vscode-light .ico { background: #0070c1; }
/* Hints: shown below the button when the mouse stays over it */
[data-hint] { position: relative; }
[data-hint]:hover::after { content: attr(data-hint); position: absolute; left: 0; right: 0; top: calc(100% + 2px); z-index: 10;
    padding: 5px 8px; font-size: 11px; line-height: 1.4; white-space: normal; text-align: left; pointer-events: none;
    background: var(--vscode-editorHoverWidget-background, #252526); color: var(--vscode-editorHoverWidget-foreground, #ccc);
    border: 1px solid var(--vscode-editorHoverWidget-border, #454545); box-shadow: 0 2px 8px rgba(0, 0, 0, .35);
    animation: hint .15s .4s both; }
h3 [data-hint]:hover::after, .row [data-hint]:hover::after { left: auto; right: 0; width: 220px; text-transform: none;
    letter-spacing: 0; font-weight: normal; }
@keyframes hint { from { opacity: 0; } to { opacity: 1; } }
select { flex: 1; min-width: 0; background: var(--vscode-dropdown-background); color: var(--vscode-dropdown-foreground);
         border: 1px solid var(--vscode-dropdown-border); padding: 3px; }
button { width: 100%; margin: 3px 0; padding: 5px 8px; text-align: left; border: none; cursor: pointer;
         background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground); }
button:hover { background: var(--vscode-button-secondaryHoverBackground); }
button.primary { background: var(--vscode-button-background); color: var(--vscode-button-foreground); }
button.primary:hover { background: var(--vscode-button-hoverBackground); }
button.small { width: auto; margin: 0; padding: 3px 8px; }
button:disabled { opacity: .45; cursor: default; }
.note { color: var(--vscode-descriptionForeground); font-size: 11px; margin: 2px 0 4px; }
.pair { display: flex; gap: 6px; } .pair button { flex: 1; }
.path { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; direction: rtl; text-align: left; }
h3 button.small { float: right; margin-top: -3px; padding: 0 6px; }
#toolsPanel > summary { display: flex; align-items: center; gap: 6px; margin: 4px 0; padding: 4px 8px; cursor: pointer;
    list-style: none; user-select: none; background: var(--vscode-button-secondaryBackground);
    color: var(--vscode-button-secondaryForeground); }
#toolsPanel > summary::-webkit-details-marker { display: none; }
#toolsPanel > summary::before { content: '\\25B8'; color: #4fc1ff; }
#toolsPanel[open] > summary::before { content: '\\25BE'; }
#toolsPanel > summary:hover { background: var(--vscode-button-secondaryHoverBackground); }
#toolsPanel > summary .summary { margin-left: auto; font-size: 11px; }
.group { margin-top: 8px; }
.group > label { display: flex; align-items: center; gap: 6px; font-weight: 600; cursor: pointer; }
.group > label .summary { font-weight: normal; font-size: 11px; margin-left: auto; }
.group.off .tools { opacity: .5; }
.tools { margin-left: 22px; }
.tool { display: flex; align-items: center; gap: 6px; margin-top: 4px; }
.tool .toolname { flex: 1; }
.toolpath { font-size: 11px; color: var(--vscode-descriptionForeground); overflow: hidden; text-overflow: ellipsis;
            white-space: nowrap; margin-left: 16px; }
.ok { color: var(--vscode-testing-iconPassed, #3fb950); }
.missing { color: var(--vscode-errorForeground); }
</style></head><body>
<div class="note" id="project"></div>
<button class="templates" data-action="templates" data-hint="Creates a new project from a template: Net Yaroze, Psy-Q, PSYQo, bare metal (nugget) or PSn00bSDK.">Templates</button>
<button class="primary" data-action="build" data-hint="Builds the project (the one of the file being edited).">Build</button>
<button class="clean" data-action="clean" data-hint="Deletes what the build made, so that the next build makes everything again.">Clean</button>

<h3>PS1 DEV CART</h3>
<div id="nocart" class="note" hidden>Set the path of ps1transfer to use the cartridges.
  <button class="small" id="choosePs1transfer">Choose ps1transfer...</button></div>
<div class="note" id="status"></div>
<div class="row"><label for="cart">Cart</label><select id="cart"></select>
  <button class="small" id="refresh" data-hint="Look for the connected cartridges again.">&#x21bb;</button></div>
<div class="row"><label for="cart2">Second cart</label><select id="cart2"></select></div>
<div class="row"><label for="link">Link</label><select id="link">
  <option value="cable">Cable (serial port)</option><option value="usb">USB link (relay, no cable)</option></select></div>
<div id="resets"></div>

<h3>Deploy</h3>
${button('runEmulator', 'deploy-to-emulator', 'Deploy to emulator', 'Builds the project and runs it in the emulator (PCSX-Redux).')}
${button('runCart', 'deploy-to-cart', 'Deploy to selected cart', 'Builds the project and runs it on the PS1 of the selected cart; its printf output and PCDRV files in a terminal.', ' data-cart="1"')}
${button('runLinkedEmulators', 'deploy-link-game-to-emulators', 'Deploy link game to emulators (two)', 'Builds the project and runs it in two emulators joined by an emulated link cable (two-player link games).')}
${button('runLinkedCarts', 'deploy-link-game-to-carts', 'Deploy link game to carts (two)', 'Builds the project and runs it on the PS1s of the two carts, joined by the link cable or the USB link (Link above).', ' data-cart="2"')}

<h3>Debug</h3>
${button('debugEmulator', 'debug-in-emulator', 'Debug in emulator', 'Builds the project with debug information and debugs it in the emulator: breakpoints, stepping, variables.')}
${button('debugCart', 'debug-on-cart', 'Debug on selected cart', 'Builds the project with debug information and debugs it on the PS1 of the selected cart (GDB server of ps1transfer).', ' data-cart="1"')}
${button('debugLinkedEmulators', 'debug-link-game-in-emulators', 'Debug link game in emulators (two)', 'Debugs the link game in two linked emulators: one debug session each.')}
${button('debugLinkedCarts', 'debug-link-game-on-carts', 'Debug link game on carts (two)', 'Runs the link game on the second cart and debugs it on the main cart (over the USB link: the relay in the debug session).', ' data-cart="2" id="debugLinkedCarts"')}

<h3>Emulator</h3>
<div class="row"><span id="emulator" class="path"></span>
  <button class="small browse" id="browseEmulator" data-hint="Choose the emulator (PCSX-Redux) used for every kind of project: Net Yaroze, PsyQ, PsyQo, PSn00bSDK.">Browse...</button></div>
<div class="note" id="emulatorNote"></div>

<h3>Tools <button class="small" id="refreshTools" data-hint="Check the tools again.">&#x21bb;</button></h3>
<div class="note">Tick the toolchains you develop with: only their tools are installed.</div>
<details id="toolsPanel">
  <summary><span id="toolsToggle">Show tools</span><span id="toolsStatus" class="summary"></span></summary>
  <div id="tools" class="note">Checking...</div>
  <button id="installSelected">Install selected</button>
</details>

<script nonce="${nonce}">
const vscode = acquireVsCodeApi();
const resetIcon = '${webview.asWebviewUri(vscode.Uri.file(path.join(__dirname, 'media', 'buttons', 'reset-cart.png')))}';
const $ = id => document.getElementById(id);
document.querySelectorAll('button[data-action]').forEach(b =>
    b.addEventListener('click', () => vscode.postMessage({ type: 'action', action: b.dataset.action })));
$('refresh').addEventListener('click', () => vscode.postMessage({ type: 'refresh' }));
$('choosePs1transfer').addEventListener('click', () => vscode.postMessage({ type: 'choosePs1transfer' }));
$('cart').addEventListener('change', e => vscode.postMessage({ type: 'cart', serial: e.target.value }));
$('cart2').addEventListener('change', e => vscode.postMessage({ type: 'cart2', serial: e.target.value }));
$('link').addEventListener('change', e => e.target.value && vscode.postMessage({ type: 'link', value: e.target.value }));

function fill(select, carts, value, none) {
    select.innerHTML = '';
    const add = (v, text) => { const o = document.createElement('option'); o.value = v; o.textContent = text; select.appendChild(o); };
    add('', none);
    carts.forEach(c => add(c.serial, c.label));
    if (value && carts.length && !carts.some(c => c.serial === value)) add(value, value + ' (not found)');
    select.value = carts.length ? value : '';
}

// Link: Cable or USB link with two carts connected, else None (the setting is kept)
function showLink(select, link, two) {
    select.innerHTML = '';
    const options = two ? [['cable', 'Cable (serial port)'], ['usb', 'USB link (relay, no cable)']] : [['', 'None']];
    options.forEach(([v, text]) => { const o = document.createElement('option'); o.value = v; o.textContent = text; select.appendChild(o); });
    select.value = two ? link : '';
}

window.addEventListener('message', ({ data }) => {
    if (data.type !== 'state') return;
    $('project').textContent = data.project;
    $('nocart').hidden = data.ps1transfer;
    // The cart: only the ones detected (the saved one, else the first one, which is also the cart used when the
    // saved one is not connected: the setting follows what is shown)
    // (none connected: None, like the Second cart)
    const cart = $('cart');
    cart.innerHTML = '';
    data.carts.forEach(c => { const o = document.createElement('option'); o.value = c.serial; o.textContent = c.label; cart.appendChild(o); });
    if (!data.carts.length) {
        const o = document.createElement('option'); o.value = ''; o.textContent = 'None'; cart.appendChild(o);
        cart.value = '';
    } else {
        const shown = data.carts.some(c => c.serial === data.serial) ? data.serial : data.carts[0].serial;
        cart.value = shown;
        if (data.serial && shown !== data.serial && !data.busy) vscode.postMessage({ type: 'cart', serial: shown });
    }
    // The second cart: not the one shown as Cart
    const main = cart.value;
    fill($('cart2'), data.carts.filter(c => c.serial !== main), data.serial2 === main ? '' : data.serial2, 'None');
    // Two carts connected (the Cart and a Second cart): the link play buttons and the Link choice
    const connected = s => !!s && data.carts.some(c => c.serial === s);
    const two = connected(main) && connected($('cart2').value) && main !== $('cart2').value;
    showLink($('link'), data.link, data.carts.length >= 2);
    $('refresh').disabled = data.busy || !data.ps1transfer;
    $('status').textContent = data.busy ? 'Looking for the cartridges...'
        : (data.ps1transfer ? data.carts.length + ' cartridge(s) connected' : '');
    document.querySelectorAll('button[data-action]').forEach(b => {
        b.disabled = (!data.hasProject && b.dataset.action !== 'templates')
            || (b.dataset.cart && (!data.ps1transfer || !connected(main))) || (b.dataset.cart === '2' && !two);
    });
    // Reset: a button for each connected cart, usable when it is the Cart or the Second cart
    const resets = $('resets');
    resets.innerHTML = '';
    data.carts.forEach(c => {
        const b = document.createElement('button');
        const used = c.serial === main || c.serial === $('cart2').value;
        b.className = 'icon';
        b.disabled = !data.ps1transfer || !used;
        b.dataset.hint = used ? 'Resets the console of this cart (first stopping what runs on it from here, e.g. its console terminal).'
            : 'Choose this cart as the Cart or the Second cart to reset it here.';
        const ico = document.createElement('span');
        ico.className = 'ico';
        ico.style.setProperty('--icon', "url('" + resetIcon + "')");
        const text = document.createElement('span');
        text.textContent = 'Reset cart (' + c.serial + ')';
        b.append(ico, text);
        b.addEventListener('click', () => vscode.postMessage({ type: 'reset', serial: c.serial }));
        resets.appendChild(b);
    });
    $('debugLinkedCarts').dataset.hint = 'Runs the link game on the second cart and debugs it on the main cart' +
        (data.link === 'usb' ? ', the USB link relay running in the debug session.' : ' (link cable).');

    // The emulator, the same for every kind of project
    $('emulator').textContent = data.emulator || 'No emulator chosen';
    $('emulator').title = data.emulator || '';
    $('emulatorNote').textContent = data.emulatorFrom === 'installed' ? 'PCSX-Redux installed (Tools below); Browse to use another one.'
        : (data.emulatorFrom === 'setting' ? '' : 'Browse to choose it, or install PCSX-Redux (Tools below).');
    $('emulatorNote').hidden = !$('emulatorNote').textContent;

    // The tools, by toolchain: a checkbox each (Install selected installs the missing tools of the ticked ones only),
    // found or missing, Install for one tool
    const list = $('tools');
    if (data.tools.length) {
        list.innerHTML = '';
        const byId = Object.fromEntries(data.tools.map(t => [t.id, t]));
        const toInstall = new Set();
        const toolRow = (t, parent) => {
            const row = document.createElement('div');
            row.className = 'tool';
            const name = document.createElement('div');
            name.className = 'toolname';
            const mark = document.createElement('span');
            mark.className = t.ok ? 'ok' : 'missing';
            mark.textContent = t.ok ? '\\u2713 ' : '\\u2717 ';
            name.appendChild(mark);
            name.appendChild(document.createTextNode(t.name));
            name.title = t.what;
            row.appendChild(name);
            if (!t.ok && t.installable) {
                const b = document.createElement('button');
                b.className = 'small';
                b.textContent = t.manual ? 'Guide' : 'Install';
                b.addEventListener('click', () => vscode.postMessage({ type: 'installTool', id: t.id }));
                row.appendChild(b);
            }
            parent.appendChild(row);
            const info = document.createElement('div');
            info.className = 'toolpath';
            info.textContent = t.ok ? t.where : t.what;
            info.title = t.ok ? t.where : '';
            parent.appendChild(info);
        };
        for (const g of data.groups) {
            const on = data.selected.includes(g.id);
            const groupTools = g.tools.map(id => byId[id]).filter(Boolean);
            const missing = groupTools.filter(t => !t.ok);
            const box = document.createElement('div');
            box.className = 'group' + (on ? '' : ' off');
            const label = document.createElement('label');
            const check = document.createElement('input');
            check.type = 'checkbox';
            check.checked = on;
            check.addEventListener('change', () => {
                const ids = data.groups.map(x => x.id).filter(id => id === g.id ? check.checked : data.selected.includes(id));
                vscode.postMessage({ type: 'toolGroups', ids });
            });
            label.appendChild(check);
            label.appendChild(document.createTextNode(g.name));
            const summary = document.createElement('span');
            summary.className = 'summary ' + (missing.length ? 'missing' : 'ok');
            summary.textContent = missing.length ? missing.length + ' missing' : 'ready';
            label.appendChild(summary);
            box.appendChild(label);
            const inner = document.createElement('div');
            inner.className = 'tools';
            groupTools.forEach(t => toolRow(t, inner));
            box.appendChild(inner);
            list.appendChild(box);
            if (on) {
                missing.filter(t => t.installable && !t.manual).forEach(t => toInstall.add(t.id));
            }
        }
        if (byId.ps1transfer) {
            const box = document.createElement('div');
            box.className = 'group';
            box.innerHTML = '<label>PS1 DEV CART</label>';
            const inner = document.createElement('div');
            inner.className = 'tools';
            toolRow(byId.ps1transfer, inner);
            box.appendChild(inner);
            list.appendChild(box);
        }
        $('installSelected').disabled = !toInstall.size;
        $('installSelected').textContent = toInstall.size ? 'Install selected (' + toInstall.size + ' missing)'
            : (data.selected.length ? 'The selected toolchains are installed' : 'Tick a toolchain to install it');
        // (seen with the section closed)
        $('toolsStatus').className = 'summary ' + (toInstall.size ? 'missing' : 'ok');
        $('toolsStatus').textContent = toInstall.size ? toInstall.size + ' to install' : (data.selected.length ? 'ready' : '');
    }
});
// The tools section: closed by default (the tools are installed once), open or closed as the user left it
const toolsPanel = $('toolsPanel');
let toolsOpenSet = false;
window.addEventListener('message', ({ data }) => {
    if (data.type === 'state' && !toolsOpenSet) {
        toolsOpenSet = true;
        toolsPanel.open = data.toolsOpen;
    }
});
toolsPanel.addEventListener('toggle', () => {
    $('toolsToggle').textContent = toolsPanel.open ? 'Hide tools' : 'Show tools';
    if (toolsOpenSet) vscode.postMessage({ type: 'toolsOpen', open: toolsPanel.open });
});
$('browseEmulator').addEventListener('click', () => vscode.postMessage({ type: 'browseEmulator' }));
$('refreshTools').addEventListener('click', () => vscode.postMessage({ type: 'refreshTools' }));
$('installSelected').addEventListener('click', () => vscode.postMessage({ type: 'installSelected' }));
vscode.postMessage({ type: 'ready' });
</script></body></html>`;
}

const panelProvider = {
    resolveWebviewView(view) {
        panelView = view;
        view.webview.options = { enableScripts: true, localResourceRoots: [vscode.Uri.file(path.join(__dirname, 'media'))] };
        view.webview.html = panelHtml(view.webview);
        view.webview.onDidReceiveMessage(async m => {
            switch (m.type) {
                case 'ready':
                    await findProjects();
                    updatePanel();
                    refreshCarts();
                    refreshTools();
                    break;
                case 'refresh':
                    refreshCarts();
                    break;
                case 'choosePs1transfer':
                    if (await ps1transferPath()) {
                        refreshCarts();
                    }
                    break;
                case 'cart':
                    await selectCart(m.serial);
                    break;
                case 'cart2':
                    await selectSecondCart(m.serial);
                    break;
                case 'reset':
                    if (m.serial && (m.serial === cartSerial() || m.serial === cartSerial2())) {
                        await resetCart(m.serial);
                    }
                    break;
                case 'link':
                    await settings().update('devcartLink', m.value, vscode.ConfigurationTarget.Global);
                    updatePanel();
                    break;
                case 'browseEmulator':
                    if (await chooseEmulator()) {
                        refreshTools();
                    }
                    break;
                case 'installTool':
                    await installTool(m.id);
                    break;
                case 'toolGroups':
                    await selectGroups(m.ids);
                    break;
                case 'toolsOpen':
                    await extensionContext.globalState.update('toolsOpen', !!m.open);
                    break;
                case 'installSelected':
                    await installGroups(selectedGroups());
                    break;
                case 'refreshTools':
                    await refreshTools();
                    break;
                case 'action':
                    if (PANEL_ACTIONS[m.action]) {
                        vscode.commands.executeCommand(PANEL_ACTIONS[m.action]);
                    }
                    break;
            }
        });
        // The cartridges connected: looked for again every 15 seconds while the panel is shown
        const tick = () => view.visible && refreshCarts();
        clearInterval(panelTimer);
        panelTimer = setInterval(tick, 15000);
        view.onDidChangeVisibility(() => view.visible && refreshCarts());
        view.onDidDispose(() => {
            clearInterval(panelTimer);
            panelView = undefined;
        });
    },
};

// ---------------------------------------------------------------------------

function activate(context) {
    output = vscode.window.createOutputChannel('PS1 DEV CART');
    tools.init(context, { settings, output, runLinux, toLinuxPath });
    projectTemplates.init(context, { output, tools, newYarozeProject: uri => newProject(context, uri) });
    extensionContext = context;
    extensionPath = context.extensionPath;
    context.subscriptions.push(
        vscode.window.registerWebviewViewProvider('ps1devcart.panel', panelProvider),
        vscode.commands.registerCommand('ps1devcart.debugLinked', () => debugLinkedEmulators()),
        vscode.commands.registerCommand('ps1devcart.runLinkedOnCarts', () => runLinkedOnCarts()),
        vscode.commands.registerCommand('ps1devcart.debugLinkedOnCarts', () => debugLinkedOnCarts()),
        vscode.commands.registerCommand('ps1devcart.stopRelay', () => stopRelay()),
        vscode.commands.registerCommand('ps1devcart.resetCart', serial => resetCart(serial)),
        vscode.commands.registerCommand('ps1devcart.selectSecondCart', serial => selectSecondCart(serial)),
        vscode.commands.registerCommand('ps1devcart.refreshCarts', () => refreshCarts()),
        vscode.commands.registerCommand('ps1devcart.installTools', groups => installMissingTools(groups)),
        vscode.commands.registerCommand('ps1devcart.installTool', id => installTool(id)),
        vscode.commands.registerCommand('ps1devcart.toolStatus', () => refreshTools()),
        vscode.workspace.onDidChangeConfiguration(e => affectsSettings(e) && updatePanel()));
    offerNewNames();
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('ps1devcart', debugConfigurationProvider(context)),
        vscode.debug.registerDebugAdapterTrackerFactory('cppdbg', pauseTracker()),
        // Listed in Run and Debug's configuration list even without a launch.json
        vscode.debug.registerDebugConfigurationProvider('ps1devcart', {
            async provideDebugConfigurations(folder) {
                const found = (await findProjects()).filter(p => !folder || p.folder.uri.toString() === folder.uri.toString());
                return found.flatMap(p => [
                    Object.assign({ type: 'ps1devcart', request: 'launch', name: 'PS1 DEV CART: Debug in PCSX-Redux' + (p.rel ? ` (${p.rel})` : '') },
                                  p.rel ? { project: p.rel } : {}),
                    Object.assign({ type: 'ps1devcart', request: 'launch', name: 'PS1 DEV CART: Debug on PS1' + (p.rel ? ` (${p.rel})` : ''),
                                    target: 'devcart' },
                                  p.rel ? { project: p.rel } : {}),
                ]);
            },
        }, vscode.DebugConfigurationProviderTriggerKind.Dynamic),
        vscode.debug.onDidStartDebugSession(session => {
            if (session.configuration.ps1devcartDebugId) {
                startedDebugIds.add(session.configuration.ps1devcartDebugId);
                debugSessions.set(session.configuration.ps1devcartDebugId, session);
            }
        }),
        vscode.debug.onDidTerminateDebugSession(session => {
            const id = session.configuration.ps1devcartDebugId;
            debugSessions.delete(id);
            if (id && emulators.has(id)) {
                killEmulator(emulators.get(id));
                emulators.delete(id);
            }
        }),
        vscode.commands.registerCommand('ps1devcart.debug', () => startDebugging()),
        vscode.commands.registerCommand('ps1devcart.debugOnPs1', () => startDebugging('devcart')),
        vscode.commands.registerCommand('ps1devcart.runOnPs1', () => runOnPs1()),
        vscode.tasks.onDidEndTask(e => cartTasks.delete(e.execution)),
        vscode.commands.registerCommand('ps1devcart.selectCart', serial => selectCart(serial)));
    context.subscriptions.push(
        output,
        vscode.tasks.registerTaskProvider(TASK_TYPE, taskProvider),
        vscode.commands.registerCommand('ps1devcart.build', () => runTarget('build')),
        vscode.commands.registerCommand('ps1devcart.clean', () => runTarget('clean')),
        vscode.commands.registerCommand('ps1devcart.rebuild', () => runTarget('rebuild')),
        vscode.commands.registerCommand('ps1devcart.run', () => runTarget('run')),
        vscode.commands.registerCommand('ps1devcart.runLinked', () => runLinkedEmulators()),
        vscode.commands.registerCommand('ps1devcart.buildModels', () => runTarget('models')),
        vscode.commands.registerCommand('ps1devcart.buildIso', () => runTarget('iso')),
        vscode.commands.registerCommand('ps1devcart.runIso', () => runTarget('runIso')),
        vscode.commands.registerCommand('ps1devcart.chooseEmulator', file => chooseEmulator(file)),
        vscode.commands.registerCommand('ps1devcart.configureIntelliSense', folder => configureIntelliSense(folder)),
        vscode.commands.registerCommand('ps1devcart.newProject', uri => newProject(context, uri)),
        vscode.commands.registerCommand('ps1devcart.newFromTemplate', () => projectTemplates.newFromTemplate()),
        vscode.commands.registerCommand('ps1devcart.convertPngToTim', (uri, options) => convertPngToTim(context, uri, options)));
    relayItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 90);
    relayItem.command = 'ps1devcart.stopRelay';
    context.subscriptions.push(relayItem);
    createStatusBar(context);
    offerTools(context).then(() => offerIntelliSense(context));
}

function deactivate() {
    stopRelay();
    for (const proc of emulators.values()) {
        killEmulator(proc, true);
    }
}

module.exports = { activate, deactivate };
