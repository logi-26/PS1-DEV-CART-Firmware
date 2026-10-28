// PS1 DEV CART extension: new projects from templates (the panel's Templates button), like the PSX.Dev extension:
// - Net Yaroze: a hello world (template/, the Net Yaroze WSL build environment's example),
// - PsyQ, PSYQo and bare metal (nugget makefiles): PSX.Dev's templates (templates/psxdev, MIT licence, from the PSX.Dev
//   extension), with their git submodules (nugget...) and, for the Psy-Q ones, the converted Psy-Q SDK PSX.Dev uses,
// - PSn00bSDK: the template of the installed PSn00bSDK (share/psn00bsdk/template).
// PSX.Dev's CMake bare metal and xmake templates are left out: the extension doesn't build those projects.

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');

const PSYQ_URL = 'https://github.com/pcsx-redux/psyq-converted/releases/download/v4.7-2/psyq-4_7-converted-light-v2.zip';
const NUGGET = { name: 'third_party/nugget', url: 'https://github.com/pcsx-redux/nugget.git' };
const PSYQ_IWYU = { name: 'third_party/psyq-iwyu', url: 'https://github.com/johnbaumann/psyq_include_what_you_use.git' };
const NET_YAROZE_PSYQ = { name: 'third_party/net-yaroze', url: 'https://github.com/gwald/psyq_to_netyaroze.git' };
const NUGGET_IGNORE = ['*.elf', '*.map', '*.cpe', '*.ps-exe', '*.dep', '*.o', '*.a'];
const EASTL = ['${workspaceFolder}/third_party/nugget/third_party/EASTL/include',
               '${workspaceFolder}/third_party/nugget/third_party/EABase/include/common'];

// kind: the kind of project made (the tools it needs); dir: the template's folder (in templates/psxdev); modules: git
// submodules; psyq: the Psy-Q SDK unpacked in third_party/psyq; include: IntelliSense include paths
const TEMPLATES = [
    {
        id: 'yaroze_hello', name: 'Hello World', category: 'Net Yaroze', kind: 'yaroze',
        description: 'A Net Yaroze program printing hello world and a frame counter (libps, built in WSL).',
    },
    {
        id: 'psn00bsdk_hello', name: 'Hello World (CMake)', category: 'PSn00bSDK', kind: 'cmake',
        description: 'PSn00bSDK\'s template: double buffered graphics, a bouncing square and the debug font.',
    },
    {
        id: 'nugget_empty', name: 'Empty (Nugget)', category: 'Bare metal', kind: 'make', dir: 'bare-metal/empty-nugget',
        description: 'An empty project, with just the barebone setup to get started.',
        modules: [NUGGET], ignore: NUGGET_IGNORE, include: ['${workspaceFolder}/third_party/nugget'],
    },
    {
        id: 'psyq_cube', name: 'Psy-Q Cube', category: 'Psy-Q SDK', kind: 'make', dir: 'psyq/cube',
        description: 'A project showing a spinning cube using the Psy-Q SDK.',
        modules: [NUGGET, PSYQ_IWYU], psyq: true, ignore: [...NUGGET_IGNORE, 'third_party/psyq'],
        include: ['${workspaceFolder}/third_party/nugget', '${workspaceFolder}/third_party/psyq-iwyu/include'],
    },
    {
        id: 'psyq_netyaroze', name: 'Net Yaroze Sprite (Psy-Q)', category: 'Psy-Q SDK', kind: 'make',
        dir: 'psyq/net-yaroze',
        description: 'A SCEE Net Yaroze tutorial (2D sprite movement, scaling and rotation) built with the Psy-Q SDK ' +
                     '(psyq_to_netyaroze).',
        modules: [NUGGET, PSYQ_IWYU, NET_YAROZE_PSYQ], psyq: true, ignore: [...NUGGET_IGNORE, 'third_party/psyq'],
        include: ['${workspaceFolder}/third_party/nugget', '${workspaceFolder}/third_party/psyq-iwyu/include',
                  '${workspaceFolder}/third_party/net-yaroze/include'],
    },
    {
        id: 'psyqo_hello', name: 'PSYQo Hello World', category: 'PSYQo SDK', kind: 'make', dir: 'psyqo/hello',
        description: 'A project simply displaying Hello World using the PSYQo SDK.',
        modules: [NUGGET], ignore: NUGGET_IGNORE, include: ['${workspaceFolder}/third_party/nugget', ...EASTL],
    },
    {
        id: 'psyqo_cube', name: 'PSYQo Cube', category: 'PSYQo SDK', kind: 'make', dir: 'psyqo/cube',
        description: 'A project featuring a rotating cube using the PSYQo SDK.',
        modules: [NUGGET], ignore: NUGGET_IGNORE, include: ['${workspaceFolder}/third_party/nugget', ...EASTL],
    },
];

let context;
let host;       // { output, tools, newYarozeProject } from extension.js

function init(extensionContext, helpers) {
    context = extensionContext;
    host = helpers;
}

// The Mustache subset of PSX.Dev's templates: {{name}}, {{#flag}}...{{/flag}}, {{^flag}}...{{/flag}} and the
// delimiter change {{=<% %>=}} (their C++ files use it so that braces stay as they are)
function render(text, data) {
    let open = '{{';
    let close = '}}';
    let out = '';
    const stack = [];       // sections: is the text shown?
    const shown = () => stack.every(Boolean);
    for (let i = 0; i < text.length;) {
        const start = text.indexOf(open, i);
        if (start < 0) {
            out += shown() ? text.slice(i) : '';
            break;
        }
        out += shown() ? text.slice(i, start) : '';
        const delim = text[start + open.length] === '=';
        const end = delim ? text.indexOf('=' + close, start) : text.indexOf(close, start + open.length);
        if (end < 0) {
            out += shown() ? text.slice(start) : '';
            break;
        }
        const tag = text.slice(start + open.length, end + (delim ? 1 : 0)).trim();
        i = end + close.length + (delim ? 1 : 0);
        if (delim) {
            [open, close] = tag.slice(1, -1).trim().split(/\s+/);
        } else if (tag[0] === '#' || tag[0] === '^') {
            const value = !!data[tag.slice(1).trim()];
            stack.push(tag[0] === '#' ? value : !value);
        } else if (tag[0] === '/') {
            stack.pop();
        } else if (tag[0] !== '!') {
            out += shown() ? String(data[tag] === undefined ? '' : data[tag]) : '';
        }
    }
    return out;
}

// Copies a template folder, its text files rendered
function copyRendered(from, to, data) {
    fs.mkdirSync(to, { recursive: true });
    for (const name of fs.readdirSync(from)) {
        const src = path.join(from, name);
        const dst = path.join(to, name);
        if (fs.statSync(src).isDirectory()) {
            copyRendered(src, dst, data);
        } else if (['.bin', '.dat', '.png', '.tim'].includes(path.extname(name).toLowerCase())) {
            fs.copyFileSync(src, dst);
        } else {
            fs.writeFileSync(dst, render(fs.readFileSync(src, 'utf8'), data));
        }
    }
}

// git with long paths on Windows (nugget's submodules go deep)
const git = (args, cwd) => run('git', ['-c', 'core.longpaths=true', ...args], cwd);

function run(command, args, cwd) {
    return new Promise((resolve, reject) => {
        cp.execFile(command, args, { cwd, windowsHide: true, maxBuffer: 1 << 26 }, (err, stdout, stderr) => {
            if (err) {
                err.message = `${command} ${args.join(' ')}: ${(stderr || err.message).trim()}`;
                reject(err);
            } else {
                resolve(stdout);
            }
        });
    });
}

const hasGit = () => run('git', ['--version']).then(() => true, () => false);

// The Psy-Q SDK (converted to ELF libraries, as PSX.Dev uses it): downloaded once, unpacked in the project
async function unpackPsyq(dest, progress) {
    const zip = host.tools.storage('psyq', PSYQ_URL.split('/').pop());
    if (!fs.existsSync(zip)) {
        await host.tools.download(PSYQ_URL, zip + '.part', progress, 'Psy-Q SDK');
        fs.renameSync(zip + '.part', zip);
    }
    progress.report({ message: 'Psy-Q SDK: unpacking...' });
    fs.mkdirSync(dest, { recursive: true });
    const tar = process.platform === 'win32'
        ? path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe') : 'bsdtar';
    await run(tar, ['-xf', zip, '-C', dest, '--strip-components=1']);
}

// IntelliSense: the MIPS toolchain's GCC, the template's include paths
function cppProperties(template) {
    const bin = host.tools.mipsToolchain();
    const gcc = bin && path.join(bin, process.platform === 'win32' ? 'mipsel-none-elf-gcc.exe' : 'mipsel-none-elf-gcc');
    return {
        configurations: [{
            name: 'PS1', ...(gcc ? { compilerPath: gcc } : {}), cStandard: 'c17', cppStandard: 'c++20',
            defines: ['__STDC_HOSTED__ = 0'], includePath: ['${workspaceFolder}/', ...template.include],
            intelliSenseMode: 'gcc-x86',
        }],
        version: 4,
    };
}

async function create(template, dir, name, progress) {
    const withGit = await hasGit();
    if (template.kind === 'yaroze') {
        await host.newYarozeProject(vscode.Uri.file(dir));
    } else if (template.kind === 'cmake') {
        const from = path.join(host.tools.psn00bsdkPath(), 'share', 'psn00bsdk', 'template');
        fs.cpSync(from, dir, { recursive: true });
        const cmake = path.join(dir, 'CMakeLists.txt');
        fs.writeFileSync(cmake, fs.readFileSync(cmake, 'utf8').replace(/\bPSn00bSDK-template\b/, name));
        fs.writeFileSync(path.join(dir, '.gitignore'), 'build/\n');
    } else {
        copyRendered(path.join(context.extensionPath, 'templates', 'psxdev', ...template.dir.split('/')), dir,
                     { projectName: name, isCMake: false });
        fs.writeFileSync(path.join(dir, '.gitignore'), template.ignore.join('\n') + '\n');
        fs.mkdirSync(path.join(dir, '.vscode'), { recursive: true });
        fs.writeFileSync(path.join(dir, '.vscode', 'c_cpp_properties.json'),
                         JSON.stringify(cppProperties(template), null, 2) + '\n');
    }
    if (withGit) {
        progress.report({ message: 'git repository...' });
        await git(['init', '-q'], dir);
        for (const m of template.modules || []) {
            progress.report({ message: `git submodule ${m.name}...` });
            await git(['submodule', 'add', '-q', m.url, m.name], dir);
        }
        if (template.modules) {
            progress.report({ message: 'git submodules of the submodules...' });
            await git(['submodule', 'update', '--init', '--recursive', '-q'], dir);
        }
        await git(['add', '-A'], dir);
    }
    if (template.psyq) {
        await unpackPsyq(path.join(dir, 'third_party', 'psyq'), progress);
    }
}

// The panel's Templates button: choose a template, the folder and the name; the project is made, then opened
async function newFromTemplate() {
    const items = [];
    for (const t of TEMPLATES) {
        if (!items.length || items[items.length - 1].category !== t.category) {
            items.push({ label: t.category, kind: vscode.QuickPickItemKind.Separator, category: t.category });
        }
        items.push({ label: t.name, detail: t.description, template: t, category: t.category });
    }
    const pick = await vscode.window.showQuickPick(items, { placeHolder: 'New project from a template',
                                                            matchOnDetail: true });
    if (!pick) {
        return undefined;
    }
    const template = pick.template;
    if (template.modules && !(await hasGit())) {
        const answer = await vscode.window.showErrorMessage(
            `PS1 DEV CART: ${template.name} needs git (its libraries are git submodules).`, 'Get Git');
        if (answer) {
            vscode.env.openExternal(vscode.Uri.parse('https://git-scm.com/downloads'));
        }
        return undefined;
    }
    if (template.kind === 'cmake'
        && !fs.existsSync(path.join(host.tools.psn00bsdkPath(), 'share', 'psn00bsdk', 'template'))) {
        const answer = await vscode.window.showErrorMessage(
            'PS1 DEV CART: the PSn00bSDK template comes with PSn00bSDK, which is not installed.', 'Install PSn00bSDK');
        if (answer && await host.tools.installTool('psn00bsdk')) {
            return newFromTemplate();
        }
        return undefined;
    }
    const folders = vscode.workspace.workspaceFolders;
    const parent = await vscode.window.showOpenDialog({
        canSelectFolders: true, canSelectFiles: false, canSelectMany: false,
        openLabel: 'Create the project in this folder', title: `${template.name}: where to create the project`,
        defaultUri: folders && folders.length ? vscode.Uri.file(path.dirname(folders[0].uri.fsPath)) : undefined,
    });
    if (!parent) {
        return undefined;
    }
    const name = await vscode.window.showInputBox({
        title: `${template.name}: project name`, prompt: `A folder of this name is made in ${parent[0].fsPath}`,
        validateInput: v => {
            if (!/^[A-Za-z0-9_-]+$/.test(v)) {
                return 'Letters, digits, - and _ only';
            }
            if (fs.existsSync(path.join(parent[0].fsPath, v))) {
                return 'That folder already exists';
            }
            return undefined;
        },
    });
    if (!name) {
        return undefined;
    }
    const dir = path.join(parent[0].fsPath, name);
    if (/\s/.test(dir)) {
        vscode.window.showErrorMessage(`PS1 DEV CART: ${dir} has a space: make can't build projects there. ` +
                                       'Choose another folder.');
        return undefined;
    }
    try {
        await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification,
                                           title: `PS1 DEV CART: creating ${name} (${template.name})` },
                                         async progress => {
                                             fs.mkdirSync(dir, { recursive: true });
                                             await create(template, dir, name, progress);
                                         });
    } catch (e) {
        host.output.appendLine(`New project ${dir}: ${e.message}`);
        vscode.window.showErrorMessage(`PS1 DEV CART: the project could not be created: ${e.message}`);
        return undefined;
    }
    host.output.appendLine(`New project ${dir} (${template.category}: ${template.name})`);
    const missing = await host.tools.missingFor([template.kind]);
    const actions = ['Open Folder', 'Add to Workspace', ...(missing.length ? ['Install Tools'] : [])];
    const answer = await vscode.window.showInformationMessage(
        `PS1 DEV CART: ${name} created in ${parent[0].fsPath}.` +
        (missing.length ? ` It needs: ${missing.map(t => t.name).join(', ')}.` : ''), ...actions);
    if (answer === 'Install Tools') {
        for (const t of missing) {
            await host.tools.installTool(t.id);
        }
    }
    if (answer === 'Add to Workspace') {
        vscode.workspace.updateWorkspaceFolders(folders ? folders.length : 0, 0, { uri: vscode.Uri.file(dir) });
    } else if (answer === 'Open Folder') {
        await vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(dir));
    }
    return dir;
}

module.exports = { init, newFromTemplate, render, TEMPLATES };
