# -*- mode: python ; coding: utf-8 -*-
# PicoStation Manager: dist/picostation_manager/ (picostation_manager.exe, its files in manager_files/), as the PS1 DEV
# CART Manager's (gui/ps1devcart_gui.spec): a folder, not a single file (no unpacking to %TEMP% at every start, that
# antivirus programs scan each time), no UPX. Its icon and cheat database are the PS1 DEV CART Manager's (gui/).
#     build_exe.bat        (python -m PyInstaller --noconfirm picostation_manager.spec)
from PyInstaller.utils.hooks import collect_all

datas = [
    ('../gui/icons', 'icons'),                  # The app icon (the windows')
    ('../gui/data/cheats.sqlite', 'data'),      # The cheat database ("From cheat database...")
    ('no_cover_template.png', '.'),             # The NO COVER placeholder (beside picostation_covers)
]
binaries = []
hiddenimports = ['picostation_cheats', 'cheatdb', 'codeengine', 'settings']    # (imported when the app opens)
for package in ('ttkbootstrap',):
    tmp_ret = collect_all(package)
    datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['picostation_manager.py'],
    pathex=['.', '../gui'],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['numpy', 'av', 'mido', 'rtmidi'],    # (the Manager's, not needed here)
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='picostation_manager',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    icon='../gui/icons/ps1devcart.ico',
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    contents_directory='manager_files',
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='picostation_manager',
)
