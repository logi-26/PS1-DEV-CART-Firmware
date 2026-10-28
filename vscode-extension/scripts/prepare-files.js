// Copies the files the extension ships from the Net Yaroze WSL build
// environment repo, so that there is one copy of each in git: the example
// project (used as the New Project template), png2tim.py, the licence and the
// Net Yaroze toolchain's build files.
// The repo: NETYAROZE_REPO, else the folder above the extension (when it is in
// that repo), else Net-Yaroze-WSL-Build-Environment next to the folder of the
// repo holding the extension (e.g. the PS1 DEV CART firmware repo).
const fs = require('fs');
const path = require('path');

const ext = path.join(__dirname, '..');
const candidates = [process.env.NETYAROZE_REPO, path.join(ext, '..'),
                    path.join(ext, '..', '..', 'Net-Yaroze-WSL-Build-Environment'),
                    path.join(ext, '..', '..', '..', 'Net-Yaroze-WSL-Build-Environment')].filter(Boolean);
const repo = candidates.find(dir => fs.existsSync(path.join(dir, 'example', 'main.c')));
if (!repo) {
    console.error('Net Yaroze WSL build environment repo not found (its example/ and tools/): set NETYAROZE_REPO. ' +
                  'Looked in: ' + candidates.join(', '));
    process.exit(1);
}
console.log('files from ' + path.resolve(repo));

function copy(from, to) {
    fs.mkdirSync(path.dirname(to), { recursive: true });
    fs.copyFileSync(from, to);
}

const template = path.join(ext, 'template');
fs.rmSync(template, { recursive: true, force: true });
for (const name of ['main.c', 'Makefile', 'auto', 'iso.xml', 'system.cnf']) {
    copy(path.join(repo, 'example', name), path.join(template, name));
}
// Stored without the dot, as packaging can leave dotfiles out
copy(path.join(repo, 'example', '.gitignore'), path.join(template, 'gitignore'));
for (const name of ['launch.json', 'settings.json']) {
    copy(path.join(repo, 'example', '.vscode', name), path.join(template, 'vscode', name));
}
copy(path.join(repo, 'tools', 'png2tim.py'), path.join(ext, 'tools', 'png2tim.py'));
copy(path.join(repo, 'tools', 'yaroze_debuginfo.py'), path.join(ext, 'tools', 'yaroze_debuginfo.py'));
copy(path.join(repo, 'LICENSE'), path.join(ext, 'LICENSE'));
console.log('prepared template/, tools/ and LICENSE');

// The Net Yaroze toolchain (GCC 2.8.1, binutils: yaroze/yaroze-toolchain.tar.gz, installed in WSL by yaroze/install.sh)
// and how it is built (the GPL's corresponding source, with NOTICE.txt): toolchain/build.sh's output when the repo
// has one, else the one already in yaroze/
for (const name of ['Dockerfile', 'build.sh']) {
    copy(path.join(repo, 'toolchain', name), path.join(ext, 'yaroze', 'toolchain-build', name));
}
const built = path.join(repo, 'toolchain', 'yaroze-toolchain.tar.gz');
if (fs.existsSync(built)) {
    copy(built, path.join(ext, 'yaroze', 'yaroze-toolchain.tar.gz'));
}
if (!fs.existsSync(path.join(ext, 'yaroze', 'yaroze-toolchain.tar.gz'))) {
    console.error('yaroze/yaroze-toolchain.tar.gz missing: build it with toolchain/build.sh of the Net Yaroze WSL ' +
                  'build environment repo');
    process.exit(1);
}
console.log('prepared yaroze/ (Net Yaroze toolchain)');

// ps1transfer comes with the extension (no setting needed): from the PS1 DEV CART repo holding the extension, or
// PS1DEVCART_REPO. Its tools (.ps-exe, loaded by ps1transfer from its own folder) and the FTDI library come too.
const devcart = [process.env.PS1DEVCART_REPO, path.join(ext, '..')].filter(Boolean)
    .find(dir => fs.existsSync(path.join(dir, 'ps1transfer', 'ps1transfer.exe')));
const bundled = path.join(ext, 'ps1transfer');
fs.rmSync(bundled, { recursive: true, force: true });
if (devcart) {
    const from = path.join(devcart, 'ps1transfer');
    const names = fs.readdirSync(from).filter(n => /^(ps1transfer\.exe|ftd2xx\.dll|.*\.ps-exe)$/i.test(n));
    for (const name of names) {
        copy(path.join(from, name), path.join(bundled, name));
    }
    console.log(`prepared ps1transfer/ (${names.length} files from ${path.resolve(from)})`);
} else {
    console.warn('ps1transfer not found (PS1 DEV CART repo: set PS1DEVCART_REPO): the extension is packaged without it');
}
