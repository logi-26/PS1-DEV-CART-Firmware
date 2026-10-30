# -*- mode: python ; coding: utf-8 -*-
# A folder (dist/ps1devcart_gui: the .exe and its files in manager_files/), not a single file: a single file unpacks
# everything to %TEMP% at every start, and the antivirus scans it each time (slow on some computers). No UPX either
# (antivirus programs look harder at UPX packed programs).
from PyInstaller.utils.hooks import collect_all

import os

datas = [('icons', 'icons')]     # The app icon (the window's)
if os.path.isfile('data/cheats.sqlite'):
    datas.append(('data/cheats.sqlite', 'data'))   # The cheat database (cheatdb.py: copied to the settings at first)
binaries = []
hiddenimports = ['mido.backends.rtmidi']
for package in ('ttkbootstrap',):
    tmp_ret = collect_all(package)
    datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['ps1devcart_gui.py'],
    pathex=['.'],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='ps1devcart_gui',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    icon='icons/ps1devcart.ico',
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
    name='ps1devcart_gui',
)
