# PS1 DEV CART GUI

A window for `ps1transfer`, so you don't have to remember its command line parameters.

Each page builds the `ps1transfer` command of what you do, shows it at the bottom (you can copy it, e.g. for a
script) and runs it. So everything works the same way as from the command line, with the same firmware features.

## Running it

Needs Python 3.10 or newer:

    pip install -r requirements.txt
    python ps1devcart_gui.py

MIDI input on the Synth page also needs `pip install mido python-rtmidi` (optional).

It uses the `ps1transfer` program of this repository (`../ps1transfer/ps1transfer.exe` on Windows), or another one
set in **Settings**. The tools `ps1transfer` runs on the PS1 (model viewer, TIM viewer, ...) are found next to it.

## The window

- **Top bar:**
  - the cartridge to use (by USB serial number, the same as the "Device ID" on the cartridge menu);
  - its state: Menu, Running, Stopped (debugger) or Crashed;
  - the firmware.
  - **Stop task** stops what runs (console, GDB server), **Reset the PS1** goes back to the menu.
- **Bottom:**
  - the command of the current page, with **Copy**;
  - the transfer progress bar;
  - the output of `ps1transfer`.
- **Pages:**
  - **Run**: upload and execute a program, in two sections: a PlayStation program (PS-X EXE, also a Net Yaroze
    game converted with yarexe), or a Net Yaroze game as the Net Yaroze tools make it (its `auto` script or ECOFF
    executable, with `libps.exe` from the Net Yaroze boot disc). Its printf goes to this computer (USB), to the
    BIOS device or to the serial port (Net Yaroze link play), or to the USB link (link play without cable, see Link
    test). Optionally the console with the PCDRV file server.
  - **Console**: printf of the running program. **Attach** connects to a program already running. The **Files**
    list shows the file server activity: each file the program opens or creates, with its mode, the bytes read and
    written, and the requests refused (file not found, or outside the PCDRV folder). The program can only open files
    in the PCDRV folder, unless "Let the PS1 open any file of this computer" is on. While the file server runs, the
    top bar shows the folder it serves.
  - **Memory**: peek, poke, upload and download, while a program runs. Addresses in hexadecimal or as a symbol
    (`@player`, `@table+0x10`), with the symbol file of your program.
  - **Screen**: screenshots of the screen or any VRAM area, live while a program runs, with automatic refresh.
  - **Debug**: crash report (cause, registers, call stack with function names, the last frame the program
    displayed), and the GDB server for GDB or
    VS Code (`target remote localhost:3333`).
  - **Hot reload**: a table of files and where they go (TIM: its VRAM position or `x,y`; sounds: SPU RAM
    address; data: RAM address). **Send now**, or **Watch** to send a file again each time it is saved (the
    printf of the program is shown meanwhile).
  - **Viewers**: the TMD model viewer with up to 8 textures, the TIM and VRAM viewer with up to 16 TIMs (with
    the palette editor save/reload, which keeps the file server running).
  - **Sound**: up to 8 sound files (VH, VB, VAB, SEQ, VAG) played by the sound tester.
  - **Synth**: the PS1 as a 16 slot sample synth (`ps1transfer -synth`), slot n on MIDI channel n.
    - Slots: click to select, double click to load a WAV or VAG (WAV files are converted to VAG, at the chosen
      sample rate, optionally looped), right click for a menu.
    - The settings of the selected slot change the sound at once: root note, volume, pan, attack, decay,
      sustain, release, reverb, fine tune.
    - Keyboard: click it, or click it and play with the computer keys (Z S X D C ... and Q 2 W 3 E ...).
    - MIDI in: a MIDI interface or a virtual MIDI port (needs `pip install mido python-rtmidi`). Either each
      channel plays its slot, or everything plays the selected slot. SysEx samples (format in `sysex.py`) are
      loaded into their slot.
    - Kits: the 16 slots (files and settings) saved to a `.ps1kit` file and opened again. The last kit is sent
      again when the synth starts with empty slots.
    - `.syx`: a slot saved as SysEx messages (for a sequencer or SysEx librarian to send), or a `.syx` file opened.
    - `python vag.py in.wav out.vag [--rate 22050] [--loop]` converts a WAV file from the command line.
  - **Memory card**: backup and restore of the whole card in slot 1 or 2, and a save browser with the icons and
    titles: export (.mcs), import, copy (to an image or a card slot), delete, new blank image. It works on a card
    image file (without the cartridge) or on the card in slot 1 or 2 of the PS1: then only the sectors needed are
    read or written, and writes are confirmed on the PS1 with the pad.
  - **Link test**: the link tester started on two consoles (two cartridges connected to this computer): serial
    cable used like a PsyQ game or like a Net Yaroze game, or the USB link through this computer (no cable: the
    relay runs until **Stop task**). **Start the relay** links two consoles already running your programs with the
    USB link tty driver (Run page, printf / TTY "USB link, no cable"): e.g. Net Yaroze games using YarIO, unmodified.
  - **Video streamer**: plays video files or links on the PlayStation (`videostreamer`, see the main README). The
    playlist is filled with the Blender examples (Sintel trailer and film); **Add file...** / **Add link...** add your
    own (anything FFmpeg opens: files, http(s) streams), **Rename...** the name shown on the PS1, **Examples** puts
    the examples back. Options: picture 320x240 (1:1) or 160x120 (2x), the most frames per second, the video KB/s and
    the sound rate. **Start** runs the video server in the GUI (any free port) and `ps1transfer -r -e -f
    videostreamer.ps-exe -net 127.0.0.1:port`; the server's lines show below, the link speed on the right; **Stop**
    (or Stop task) ends both. Needs `pip install av numpy miniaudio` (included in the executable).
  - **Audio streamer**: plays internet radio stations and audio files on the PlayStation (`audiostreamer`, see the
    main README). The playlist is filled with six UK stations (Smooth, Capital London, Heart, Classic FM, Heart 80s,
    Radio X Classic Rock); **Add file...** adds audio files (MP3, FLAC, WAV, OGG, M4A, ...), **Add link...** station
    links (Icecast/SHOUTcast MP3 streams show the song playing) or any audio link FFmpeg opens, **Rename...** the name
    shown on the PS1, **Examples** puts the stations back. Option: the sound rate. **Start** runs the audio server in
    the GUI and `ps1transfer -r -e -f audiostreamer.ps-exe -net 127.0.0.1:port`; at the end of a file the next one
    plays; Left/Right on the controller goes to the previous/next. **Stop** (or Stop task) ends both.
  - **Netplay**: link games with a console elsewhere (Net Yaroze tty games, psxcomb homebrew; see the main README).
    **Host** runs the lobby in this program (port 7010): the router opens the ports itself when it can (UPnP,
    renewed every 30 minutes, removed on Disconnect and when the program closes), the status shows the address to
    give the other players (**Copy address**) or what to forward (TCP+UDP 7010, UDP 7011-7014) and warnings
    (carrier-grade NAT, two routers); allow the program in the Windows firewall. **Join** connects to a host's address
    (optional password); a lost connection reconnects every 3 s (a game goes on). Your
    game: a PS-X EXE, or a Net Yaroze game's auto script or ECOFF executable (both players need the same game:
    checked with a checksum, of the script, its program and its data files for an auto script), libps.exe for Net
    Yaroze games (not for a game converted with yarexe, which includes it),
    the link type. The player list (game, status, ping), a chat to everyone or one player (Message), **Invite to
    play**. An accepted invite resets both consoles and starts the game with `ps1transfer -netlink` (the host waits,
    the guest connects; two guests go through the host's relay); the status line shows the round trip time and the
    units per second. **Stop the game** ends it for both. The lobby is `gui/netplay.py` (server + client).
    `netplay_bot.py`: a pretend player for tests from another network (plays the link tester's other side).
  - **Profiler**: profiles a program (`ps1transfer -prof -json`, see the main README). **Run and profile** resets
    the console and runs the executable (its free RAM filled first if "Measure RAM use" is on), **Profile the running
    program** attaches to what runs. Every second: CPU, GPU, frames, RAM used, stack, SPU voices, dropped samples;
    the functions (last second and since the start), a chart of the last minute, DMA activity and the 24 SPU voices.
    A double click on a function marks it as waiting (not CPU load; saved, passed with `-idle`). Options: samples a
    second, root counter. Needs the firmware with debug monitor version 3.
  - **Firmware**: the cartridge information (firmware, version, features, debug monitor), and flashing a ROM
    (the repository's `cartrom/ps1devcart.rom` by default). The page checks the ROM file first, the button only works
    once you have typed the serial number of the cartridge, and the flash is read back and compared at the end.
  - **Settings**: `ps1transfer` program, theme.

A cartridge can only be used by one program at a time: while the console or the GDB server runs, starting
something else asks you to stop it first.

Settings and recent files are saved in `%APPDATA%\ps1devcart-gui\settings.json` (Windows) or
`~/.config/ps1devcart-gui/settings.json` (Linux).

## A single program (no Python needed)

    pip install pyinstaller
    build_exe.bat          (Linux: ./build_exe.sh)

makes `dist/ps1devcart_gui.exe`. It finds `ps1transfer` next to itself, or in `../../ps1transfer` (when it stays in
`gui/dist`), or set it in **Settings**.

## ps1transfer options used by the GUI

- `-info`: the cartridge state as `key=value` lines (device, state, firmware, features, monitor).
- `-progress`: transfer progress as `progress=done/total` lines.
- `-pcdrvlog`: file server activity as `pcdrv=...` lines (with the console, `-c`):
  `pcdrv=init`, `pcdrv=open|create fd=N mode=read|write|readwrite name="..."`,
  `pcdrv=close fd=N name="..." read=R written=W`,
  `pcdrv=error op=open|create mode=... name="..." reason="..."`.
- `-pcdrvany`: the file server opens any path (otherwise only files in the served folder).
- `-flash rom.rom`: flashes a cartridge ROM with the flasher (`ar_flash.ps-exe`, next to `ps1transfer`) and reads it
  back to check it.
- The exit code is 0 on success, 1 after an error.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
