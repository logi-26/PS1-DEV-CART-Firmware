# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_all

datas = [('licenses', 'licenses')]     # Third-party licences (FFmpeg notice), Settings page
binaries = []
hiddenimports = ['mido.backends.rtmidi', 'video_server', 'mdec_encoder', 'audio_server']
# ttkbootstrap; PyAV (with its FFmpeg libraries) and miniaudio for the video streamer page
for package in ('ttkbootstrap', 'av', 'miniaudio'):
    tmp_ret = collect_all(package)
    datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['ps1devcart_gui.py'],
    pathex=['.', '../ps1-tools/videostreamer', '../ps1-tools/audiostreamer'],     # (servers of the video and audio streamer pages)
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
    a.binaries,
    a.datas,
    [],
    name='ps1devcart_gui',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
