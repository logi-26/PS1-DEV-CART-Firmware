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
  - **Run PSX**: upload and execute a PlayStation program (PS-X EXE: PSn00bSDK, PsyQ, PsyQo, also a Net Yaroze
    game converted with yarexe).
  - **Run Net Yaroze**: upload and execute a Net Yaroze game as the Net Yaroze tools make it (its `auto` script or ECOFF
    executable, with `libps.exe` from the Net Yaroze boot disc).
  - On both, with their own options: printf goes to this computer (USB), to the BIOS device or to the serial port
    (Net Yaroze link play), or to the USB link (link play without cable, see Link test). Optionally the console
    with the PCDRV file server.
  - **Run Linked** (on both pages, enabled when two cartridges or more are connected): the program on the selected
    cartridge and on another one (shown next to the button), for link games, without the console; with printf / TTY
    "USB Link", the USB link relay is then started between them (as the VS Code extension does), until it is stopped
    (**Stop task**, or **Stop the relay** on the Link Connection Test page).
  - **Console & File Server**: printf of the running program. **Attach** connects to a program already running. The **Files**
    list shows the file server activity: each file the program opens or creates, with its mode, the bytes read and
    written, and the requests refused (file not found, or outside the PCDRV folder). The program can only open files
    in the PCDRV folder, unless "Let the PS1 open any file of this computer" is on. While the file server runs, the
    top bar shows the folder it serves.
  - **Remote Controller**: a controller driven from this computer, for testing without a controller: hold the
    buttons of the pad drawn with the mouse, or the keyboard (arrows, Z/X/C/S for Square/Cross/Circle/Triangle, Q/E
    L1/R1, 1/3 L2/R2, Enter Start, Backspace Select), or latch them. The program is started with the remote
    controller option (Run PSX, Run Net Yaroze, Frame Inspector: `ps1transfer -pad`, firmware with the debug monitor
    version 8): the cartridge adds the buttons to the pad data the BIOS reads, for programs reading their pad through
    the BIOS (PsyQ's PadInit, Net Yaroze, PSn00bSDK's InitPAD); a real controller still works. The **Controller port
    2** toggle switches the buttons (and recordings played back) between port 1 and port 2, at any time; a real
    controller on the other port keeps working. Port 2 needs the debug monitor version 9 (the page says so with an
    older firmware) and a program that reads a second pad. The buttons go through
    the live link (`ps1transfer -memlive`, shared with the Memory page's watch list, or the frame capture).
    **Controller** chooses the device the program sees (firmware with the debug monitor version 11), like in an
    emulator: **DualShock** and **analog joystick** (the pad drawing, and both sticks dragged with the mouse, L3/R3;
    they go back to the center when released, unless turned off), **NeGcon** (A = Circle, B = Triangle, R = R1 on the
    pad drawing, twist and button I dragged on the steering area, II and L on sliders), **Mouse** (click its area: this
    computer's mouse moves the PS1 mouse and its buttons are the mouse's, speed adjustable, Esc lets it go), **GunCon**
    (the console's picture, the frame being drawn, read with Refresh or every second: aim and pull the trigger with
    the left button, A with the right one, B with the middle one, a shot off-screen to reload; the X/Y ranges set
    where the picture's edges are, games calibrate the gun). Konami's Justifier/Hyperblaster can't be emulated (they
    need the light gun interrupt of the controller port). The device replaces the real one's reply on that port.
    **USB controller**: a game controller plugged into this computer (e.g. a PS1 controller on a USB adapter) drives
    the pad too, together with the mouse and the keyboard, on any page of the window. It is found by itself (Refresh
    after plugging one in); its sticks move the DualShock's, and on a NeGcon the left stick twists and Cross, Square
    and L1 are I, II and L. An analog controller (L3/R3 pressed, or a stick moved) turns a Digital pad into a
    DualShock by itself, unless Controller was chosen by hand. **Map buttons...** asks for each PS1 button in turn, then the sticks, on the controller
    (Skip keeps one as it was), and remembers the mapping for that controller; **Default mapping** goes back to it.
    **Run the control tester** starts the PS1 DEV CART control tester on the console (`ps1transfer -padtest`) and
    connects: it shows both ports as a program sees them (device, buttons, sticks, NeGcon values, the mouse pointer and
    the GunCon sight, shots marked), to try each controller without a game of your own.
    **Record** captures every change (buttons, sticks and other axes, mouse motion) with its time, so **Play** (looping or not) sends them again with the
    same timing; a recording can be saved to a file and loaded back (`.ps1pad`, next to the recorded events list).
  - **Memory Peek/Poke**: the variables of the running program, with its symbol file (ELF, Net Yaroze executable,
    `.map` or `nm` output): **Watch variables** lists them (search, sizes), adds them to a watch list whose values are
    read live (1 to 10 times a second, changes highlighted) as the type chosen (8/16/32-bit, signed or not, float,
    bytes), and a double click on a value writes a new one (`ps1transfer -memlive`, which also shows the program's
    printf and serves its files meanwhile). **Peek / Poke / Files**: peek, poke, upload and download, addresses in
    hexadecimal or as a symbol (`@player`, `@table+0x10`); while watching, peek and poke go through the same link.
  - **Screen Capture**: screenshots of the screen or any VRAM area, live while a program runs, with automatic refresh.
    **Screen on the TV (auto)** finds where the program shows its screen (the TIM viewer tells it, e.g. 0,208 when
    the TIMs fill VRAM, and holds a complete frame: title, panels and text) (`ps1transfer -v auto`).
  - **Debug Server**: crash report (cause, registers, call stack with function names, the last frame the program
    displayed), and the GDB server for GDB or
    VS Code (`target remote localhost:3333`).
  - **Frame Inspector**: the GPU commands of a frame. **Run with frame capture** starts the program in a debug
    session (`ps1transfer -frame`); **Capture a frame** stops it when it hands its ordering tables to the GPU
    (DrawOTag, between two VSync calls: one frame, whether the program waits with VSync(0) or polls VSync(-1); found in its symbol file, or the .elf/.map next to the program (the bundled tools' are next to them in ps1transfer); a symbol file of another program is reported and not used; without one, at its GPU DMA, which misses the lists the library queues: partial frames) and reads
    them: every primitive is listed (polygons, sprites, tiles, lines, fills, draw modes; colors, vertices, texture
    page, CLUT, semi-transparency, the raw words) and outlined on the frame it drew (read from VRAM). A click on the
    frame selects the primitive drawn there (the one on top); a filter shows one kind.
  - **Hot Reload**: a table of files and where they go (TIM: its VRAM position or `x,y`; sounds: SPU RAM
    address; data: RAM address). **Send now**, or **Watch** to send a file again each time it is saved (the
    printf of the program is shown meanwhile).
  - **Model & Animation Viewer**: the TMD model viewer with up to 8 textures and up to 16 TOD animations (`ps1transfer -m ...
    -tod ... -mdllive`). While it runs, its controls are on the page too, linked live with the console's controller:
    the TOD played (list, previous/next), first/previous/next/last frame, play/pause, a frame slider, loop, speed
    (10% to 400%), and the view (X, Y, Z rotation, zoom, reset). A change made with the controller shows on the page,
    and the other way round.
  - **VRAM Viewer**: up to 32 TIMs (Add also takes a Net Yaroze auto script: its TIM files), shown on the computer and in the console's TIM and VRAM viewer. The VRAM view
    (made from the files, or read from the console with **Refresh VRAM**) outlines each image and CLUT; a click
    selects a TIM, drawn with the chosen CLUT row, and a click on a colour of its palette edits it (red, green, blue,
    semi-transparency). **Run on the console** starts the viewer (`ps1transfer -tim ... -c -timlive`): each change
    is sent at once (on the TV, and in the console's own palette editor), the changes made with the console's
    palette editor come back about once a second, and its save/reload works (file server). The TIM and CLUT row
    shown are kept in sync: a TIM chosen here is shown on the console, one chosen with the pad (L1/R1, L2/R2) is
    selected here. **Controls on the TV** hides or shows the console's info panel and controls (Start on the pad)
    for a clear view of the image. **Moving in VRAM** (like Sony's TIM Tool): right-click and drag a TIM's image (solid
    outline) or CLUT (dashed) in the VRAM view to a free place (a left click selects it, also on the console); the outline turns red over another TIM, outside VRAM or over the
    console viewer's own screens and font (grey), and the drop is refused there. A CLUT snaps to x multiples of 16;
    a 4/8-bit image crossing a texture page is warned about. On the console the TIM moves at once (the old area is
    cleared); **Save** / **Save a copy** writes the new x, y in the TIM file, **Restore the position** puts it back.
    **Stop**, the console no longer answering (cartridge unplugged, console off), or Refresh finding no cartridge
    clear the page (TIM list, VRAM view, palette); TIMs edited and not saved can be saved first. Restore the colour, the
    row or all, save a copy (`NAME_edit.tim`) or overwrite the TIM.
  - **SPU RAM Viewer**: the sound RAM (512KB) of the running program, read with **Read SPU RAM** (`ps1transfer -spu`,
    firmware with the debug monitor version 10, about 3 seconds; **Open dump...** reads a saved one). A map (256 bytes
    per cell) shows the capture buffers, the samples (ADPCM up to their end flag; brighter when a voice uses them), the
    free memory, data that isn't ADPCM and the reverb work area; the list gives each one's address, size, the voices
    starting in it, its loop and notes, and the voices table their state, addresses, rate and volume. A sample
    selected (map, list or voice) is drawn with its loop point, played on the computer at its voice's rate (or one
    chosen), through its loop to hear a click there, and saved as WAV or VAG. Warnings: a playing voice starting in
    free memory (silent) or inside a sample, a sample cut or overwritten (no end flag), running into the reverb work
    area. SPU RAM survives a reset: samples no voice uses may be left by an earlier program.
  - **Sound Tools**: up to 8 sound files (VH, VB, VAB, SEQ, VAG) played by the sound tester (`ps1transfer -snd ...
    -sndlive`). While it runs, its controls are on the page too, linked live with the console's controller: program,
    note (buttons as Up/Down and L1/R1, a two-octave keyboard that follows the note: click and hold a key to play it,
    as Cross held), volume (also for the notes sounding), SEQ start/stop with its bar and loop, stop all sounds, and the
    24 voices lit while they sound. A change made with the controller shows on the page, and the other way round, so
    sounds can be tested without a controller.
    The **VAB Editor** tab tunes the bank (the VAB, or VH + VB, of the sound files) on the console, like Sony's
    SoundDelicatessen: bank volume; program volume, pan and priority; for each tone its volume, pan, center note, fine
    tune, key range (drawn on a keyboard strip with the note played), pitch bend range, VAG, and its ADSR envelope
    (attack, decay, sustain level, sustain rate and direction, release, linear or exponential, drawn with its times).
    Each change is written in the bank the console plays: hold a note (Play) while moving a slider to hear it at once
    (key ranges and VAGs: from the next note). Changes made before Play on the PS1 are sent when it starts. **Save
    VAB...** writes the edited bank (VAB: VH + VB, or a VH alone).
  - **Movie Tools**: STR movies (MDEC video frames, XA ADPCM sound), made, checked and played on the PS1:
    - **Convert**: a video file (read by FFmpeg: any format it knows) or pictures (TIM, PNG, JPG, BMP: a slide show,
      each one for some seconds with a cross-fade into the next, or one frame each), with the video's sound, a sound
      file (WAV, or others with FFmpeg) or none, into an STR file: resolution (320x240, 256x240, 320x256 PAL, 640x240,
      custom: multiples of 16), TV shape (4:3, or 16:9 anamorphic) and fit (letterbox, crop, stretch: the PS1's
      pixels aren't square, the picture keeps its shape on the TV), frame rate, CD speed (1x: 75 sectors a second,
      2x: 150), quality (the finest quantization whose frame fits its share of the sectors, or a fixed one), sound
      (37800 or 18900 Hz, mono or stereo, volume), the start and length of the video. The sector budget of a frame
      is shown as the settings change. The file: 2336-byte sectors (subheaders, video and XA sound interleaved as the
      drive reads them, EDC/ECC: mkpsxiso takes it as an "str" file) or 2048 (video only, a plain data file).
      FFmpeg: the settings' path, the PATH, or **Get FFmpeg** (the essentials build of gyan.dev, GPL, downloaded
      into the settings folder; an outside program, run to read the videos). Pictures and WAV sounds don't need it.
    - **Inspect & preview**: any STR (2336, 2352 or 2048-byte sectors, also with a RIFF CDXA header): its sectors,
      frames (sector, sectors, bytes, quantization, BS version, size), sound (rate, channels, interleave), drive
      speed, problems (incomplete frames...), each frame as the MDEC decodes it (BS version 2), played here with its
      sound; a frame saved as PNG, the sound as WAV.
    - **Test on the PS1**: the PS1 DEV CART VIDEO TESTER plays it (`ps1transfer -e -f videotest.ps-exe -c -p
      FOLDER`, the movie read through the file server, the commands through its `@input`): RAM (the movie loaded
      first, up to 1 MB: 3 s at 2x, 6 s at 1x, then played at the exact pace) or stream (read as it plays: about
      400 KB/s with the console busy meanwhile, so 1x movies keep their pace with the picture about a frame late
      at 320x240, 2x ones are too fast: stalls); 15 bits (the information on the screen, the picture 1x, fitted or
      full screen) or 24 bits (as games show their movies); pause, restart, next frame, loop, volume; the frame,
      decode time, bytes, quantization, late and dropped frames, stalls and sound underruns shown here.
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
  - **Memory Card Tools**: backup and restore of the whole card in slot 1 or 2, and a save browser with the icons and
    titles: export (.mcs), import, copy (to an image or a card slot), delete, new blank image. It works on a card
    image file (without the cartridge) or on the card in slot 1 or 2 of the PS1: then only the sectors needed are
    read or written, and writes are confirmed on the PS1 with the pad.
  - **Link Connection Test**: the link tester started on two consoles (two cartridges connected to this computer): serial
    cable used like a PsyQ game or like a Net Yaroze game, or the USB link through this computer (no cable: the
    relay runs until **Stop task**). **Start the relay** links two consoles already running your programs with the
    USB link tty driver (Run PSX or Run Yaroze page, printf / TTY "USB Link"): e.g. Net Yaroze games using YarIO, unmodified.
    **Stop the relay** stops it (also when Run Linked started it).
  - **Profiler**: profiles a program (`ps1transfer -prof -json`, see the main README). **Run and profile** resets
    the console and runs the executable (its free RAM filled first to measure RAM use), **Profile the running
    program** attaches to what runs. A Net Yaroze game (auto script or ECOFF executable) runs with the libps.exe
    entered on the Run Yaroze page (a message asks for it there if it isn't set). Every second: the diagnosis under
    the functions and charts (what limits the frame rate: CPU, GPU, VSync, CD-ROM, with hints and the averages/peaks
    since the start), CPU (ms a frame), GPU, frames and frame time against the 50/60 Hz
    budget, RAM used and free (largest block), stack (and deepest), SPU voices (% of the 24, most at once), dropped
    samples; the functions (last second and since the start) with their caller, a chart of the last minute, where the
    time goes (CPU, VSync, GPU, CD-ROM, SPU waits), DMA activity and the 24 SPU voices. A double click on a function
    marks it as waiting (not CPU load; saved, passed with `-idle`). 1000 samples a second, root counter 0. Needs the
    firmware with debug monitor version 3 (version 7 for the video mode).
  - **Firmware**: the cartridge information (firmware, version, features, debug monitor), and flashing a ROM
    (the repository's `cartrom/ps1devcart.rom` by default). The page checks the ROM file first, the button only works
    once you have typed the serial number of the cartridge, and the flash is read back and compared at the end.
  - **Settings**: `ps1transfer` program, theme (on Windows 10 and 11, the title bars follow it: dark with a dark
    theme).

A cartridge can only be used by one program at a time: while the console or the GDB server runs, starting
something else asks you to stop it first.

Settings and recent files are saved in `%APPDATA%\ps1devcart-gui\settings.json` (Windows) or
`~/.config/ps1devcart-gui/settings.json` (Linux).

## A program without Python

    pip install pyinstaller
    build_exe.bat          (Linux: ./build_exe.sh)

makes the folder `dist/ps1devcart_gui/`: `ps1devcart_gui.exe` and the files it needs in `manager_files/` (a folder
starts much faster than a single file, which unpacks itself to `%TEMP%` at every start). Keep `manager_files/` next to
the exe. It finds `ps1transfer` next to itself, or in `../../../ps1transfer` (when it stays in `gui/dist/ps1devcart_gui`),
or set it in **Settings**.

The app icon (the cartridge of the VS Code extension's panel, light blue) is in `icons/`: `ps1devcart.ico` for the
program, `ps1devcart-SIZE.png` for the windows; `python make_icon.py` makes them again.

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
