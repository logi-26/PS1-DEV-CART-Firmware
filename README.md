# PS1 DEV CART

PS1 DEV CART is an alternative firmware and set of tools for **PS1USB** cartridges produced by OrionSoft.

The cartridge plugs into the parallel port of the PlayStation and connects to your computer with a USB mini-B cable.
With this firmware you can:
- upload and run programs;
- read and write the memory and take screenshots while your program runs;
- debug it with GDB or VS Code;
- hot reload graphics and sounds;
- link two consoles without a cable;
- work with memory cards, 3D models, textures and sounds.

Transfers are also several times faster than with the original firmware.

The firmware and all the PlayStation programs are built with the open source
[PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK). No PsyQ (Sony's SDK) is needed.

## Screenshots

### On the PlayStation

**PS1 DEV CART menu**: The cartridge's menu: firmware version, device ID, and ready for the computer.

<p align="center">
  <img src="screenshots/ps1/menu.png" alt="The cartridge's menu: firmware version, device ID, and ready for the computer." width="640">
</p>

**Video streamer**: The Sintel trailer streamed from the internet, decoded by the PlayStation's MDEC.

<p align="center">
  <img src="screenshots/ps1/videostreamer.png" alt="The Sintel trailer streamed from the internet, decoded by the PlayStation's MDEC." width="640">
</p>

**SPU synth**: Six samples loaded into the synth's slots, the selected slot's settings and the keyboard.

<p align="center">
  <img src="screenshots/ps1/synth.png" alt="Six samples loaded into the synth's slots, the selected slot's settings and the keyboard." width="640">
</p>


**TIM viewer/Palette editor**: The TIM viewer's palette editor: each colour of the palette changed with the pad, the image redrawn live, saved back to the computer.

<p align="center">
  <img src="screenshots/ps1/paletteeditor.png" alt="The TIM viewer's palette editor: each colour of the palette changed with the pad, the image redrawn live, saved back to the computer." width="640">
</p>

**TMD model viewer**: A textured car model, rotated and zoomed with the pad.

<p align="center">
  <img src="screenshots/ps1/tmdviewer.png" alt="A textured car model, rotated and zoomed with the pad." width="640">
</p>


<details>
<summary>More PlayStation tools</summary>

**Crash screen**: A program that crashed: the cause, the registers and a stack dump.

<p align="center">
  <img src="screenshots/ps1/crash.png" alt="A program that crashed: the cause, the registers and a stack dump." width="640">
</p>

**Sound tester**: A VAG sample loaded, with the note, the volume and the 24 voices.

<p align="center">
  <img src="screenshots/ps1/soundtest.png" alt="A VAG sample loaded, with the note, the volume and the 24 voices." width="640">
</p>

**Audio streamer**: An internet radio station playing, with the song title and the buffer.

<p align="center">
  <img src="screenshots/ps1/audiostreamer.png" alt="An internet radio station playing, with the song title and the buffer." width="640">
</p>

**Link cable tester**: Two consoles linked through their cartridges: packets, losses and round trip time.

<p align="center">
  <img src="screenshots/ps1/linktest.png" alt="Two consoles linked through their cartridges: packets, losses and round trip time." width="640">
</p>

**Memory card tool**: A whole memory card backed up to the computer.

<p align="center">
  <img src="screenshots/ps1/cardtool.png" alt="A whole memory card backed up to the computer." width="640">
</p>

**Flasher**: The firmware written to the cartridge, then checked.

<p align="center">
  <img src="screenshots/ps1/flasher.png" alt="The firmware written to the cartridge, then checked." width="640">
</p>

</details>

### PS1 Dev Cart Manager

**Profiler**: A 3D racing game profiled while it runs: CPU and GPU load, frames, RAM, and the functions where the time goes.

<p align="center">
  <img src="screenshots/manager/profiler.png" alt="A 3D racing game profiled while it runs: CPU and GPU load, frames, RAM, and the functions where the time goes." width="100%">
</p>

**Netplay**: A lobby with a chat, and two players playing the link tester over the internet.

<p align="center">
  <img src="screenshots/manager/netplay.png" alt="A lobby with a chat, and two players playing the link tester over the internet." width="100%">
</p>

**Debug**: A crash report with the call stack and the function names.

<p align="center">
  <img src="screenshots/manager/debug.png" alt="A crash report with the call stack and the function names." width="100%">
</p>

**Synth**: Samples loaded into the PlayStation's SPU synth, with each slot's settings and a keyboard.

<p align="center">
  <img src="screenshots/manager/synth.png" alt="Samples loaded into the PlayStation's SPU synth, with each slot's settings and a keyboard." width="100%">
</p>

<details>
<summary>All the Manager pages</summary>

**Run**: A program run with its printf sent to the computer and the PCDRV file server.

<p align="center">
  <img src="screenshots/manager/run.png" alt="A program run with its printf sent to the computer and the PCDRV file server." width="100%">
</p>

**Console**: The program's printf and the files it opened on the computer.

<p align="center">
  <img src="screenshots/manager/console.png" alt="The program's printf and the files it opened on the computer." width="100%">
</p>

**Memory**: A variable of the running game read by its symbol name.

<p align="center">
  <img src="screenshots/manager/memory.png" alt="A variable of the running game read by its symbol name." width="100%">
</p>

**Screen**: A live screenshot of the running game.

<p align="center">
  <img src="screenshots/manager/screen.png" alt="A live screenshot of the running game." width="100%">
</p>

**Hot reload**: Textures sent to the running game without restarting it.

<p align="center">
  <img src="screenshots/manager/hotreload.png" alt="Textures sent to the running game without restarting it." width="100%">
</p>

**Viewers**: A TMD model with its texture, and TIM files for the TIM viewer.

<p align="center">
  <img src="screenshots/manager/viewers.png" alt="A TMD model with its texture, and TIM files for the TIM viewer." width="100%">
</p>

**Sound**: VAG samples for the sound tester.

<p align="center">
  <img src="screenshots/manager/sound.png" alt="VAG samples for the sound tester." width="100%">
</p>

**Memory card**: The saves on the card in slot 1, with their icons.

<p align="center">
  <img src="screenshots/manager/memorycard.png" alt="The saves on the card in slot 1, with their icons." width="100%">
</p>

**Link test**: The link tester running on two consoles through the USB link relay.

<p align="center">
  <img src="screenshots/manager/linktest.png" alt="The link tester running on two consoles through the USB link relay." width="100%">
</p>

**Video streamer**: The playlist and the statistics of the video being streamed.

<p align="center">
  <img src="screenshots/manager/video.png" alt="The playlist and the statistics of the video being streamed." width="100%">
</p>

**Audio streamer**: Internet radio stations, with the song playing.

<p align="center">
  <img src="screenshots/manager/audio.png" alt="Internet radio stations, with the song playing." width="100%">
</p>

**Firmware**: The cartridge's information, and the ROM ready to flash.

<p align="center">
  <img src="screenshots/manager/firmware.png" alt="The cartridge's information, and the ROM ready to flash." width="100%">
</p>

**Settings**: The ps1transfer program, the theme and the third-party licences.

<p align="center">
  <img src="screenshots/manager/settings.png" alt="The ps1transfer program, the theme and the third-party licences." width="100%">
</p>

</details>

## About AI use in this project

Much of the code in this project was written by an AI assistant, working under the direction and guidance of a
human developer. Every feature was a human idea, the design decisions were made by a human, and much of the testing
was done by a human on real PlayStation hardware.

AI was used because writing and testing a firmware and tool set of this size by hand would have taken a very long
time, and most people in the PlayStation homebrew scene would rather spend their time making PlayStation software
than building the development tools for it. The aim of this project is to give them those tools.


## Comparison with the original PS1USB firmware

| Feature                              | PS1USB (OrionSoft firmware)                  | PS1 DEV CART firmware                                      |
|--------------------------------------|----------------------------------------------|---------------------------------------------------|
| **Transfer speed (upload)**          | ~122 KB/s                                    | ~655 KB/s (~5× faster)                            |
| **Transfer speed (download)**        | ~103 KB/s                                    | ~840 KB/s (~8× faster)                            |
| **Packet size**                      | 2 KB                                         | Up to 16 KB (still works with original 2 KB)      |
| **Upload & run PS-EXE**              | Yes                                          | Yes                                               |
| **Reset console from PC**            | Yes                                          | Yes                                               |
| **printf / TTY redirect** | Yes (to the PC only) | Yes (PC, BIOS device, serial, or USB) |
| **PCDRV file server** | Yes (`libsn_usb`) | Yes (`psxpcdrv`: same functions, `libsn_usb.h`/`libsn.h` still work) |
| **Net Yaroze support** | No | Yes (`libps` uploaded and started, serial `tty` driver) |
| **Live memory access** | No (upload/download from the menu only) | Yes, while the program runs: peek/poke/upload/download, with symbols from `.map`/`.elf` |
| **Live screenshots / VRAM** | From the menu only (VRAM saved at boot) | Yes (live while the program runs, or the saved VRAM from the menu) |
| **Hot reload** (TIM, sounds, data) | No | Yes (TIM to VRAM, sounds to SPU RAM, data to RAM; sent again on each save) |
| **Crash screen + report** | Crash screen (cause, registers, stack dump) | Crash screen, and a report on the PC (cause, registers, call stack with function names) |
| **GDB / VS Code debugging**          | No                                           | Yes (full GDB server, breakpoints, step, watchpoints, etc.) |
| **Profiler** | No | Yes, without changing the program: CPU load and the functions where the time goes, GPU busy, frames per second, RAM used, stack, DMA, SPU voices (1000 samples a second) |
| **Multi-cartridge support** | No (first USB device only) | Yes (select by USB serial / device ID) |
| **USB link (two consoles)**          | No                                           | Yes (no physical link cable needed - PC relay)               |
| **Link cable library** | No | Yes (`psxcomb`, compatible with libcomb, cable or USB) |
| **Internet through the computer** | No | Yes (`-net`: the program's USB link connected to a server on the computer, which does the internet side) |
| **Netplay** (link play over the internet) | No | Yes (Net Yaroze games and psxcomb homebrew; lobby with chat and invites hosted in the GUI, direct links, encrypted link keys) |
| **Flasher** | Flashes the ROM built into it (run over USB or from a CD); checks the flash chip ID, verifies the flash, retry with X | Same, plus any ROM uploaded by `ps1transfer` or the GUI (checked first), status on the PC, flash read back and compared on the PC |
| **PCDRV example program** | Yes (loads files from the PC or the CD) | Yes (same) |
| **TMD model viewer** | No | 3D models with up to 8 textures (TIM): rotate, zoom, all TMD polygon types (flat/gouraud, textured, lit/unlit, gradation) |
| **Sound tester** | No | VAB banks (VH + VB), SEQ music and VAG samples: plays each instrument by program and note, SEQ start/stop, volume |
| **TIM / VRAM viewer** | No | Up to 16 TIMs: VRAM map with overlaps, TIM view with zoom and CLUT rows, warnings (overlaps, texture page crossing, CLUT position), palette editor, CLUT swap |
| **Memory card tool** | No | Backup/restore of a whole card; save browser with icons; export, import, copy, delete single saves on the card (writes confirmed on the console); card image files |
| **SPU synth** | No | 16 sample slots played over MIDI (one channel each), VAG/WAV and SysEx upload, ADSR, reverb, pan, fine tune, MIDI input |
| **Link cable tester** | No | Packets both ways each frame between two consoles: throughput, losses, corrupted packets, round trip time; serial like libcomb or Net Yaroze, or the USB link |
| **Audio streamer** | No | Internet radio stations and audio files played by the PS1 (SPU ADPCM, 22050 Hz), with titles; playlist in the GUI |
| **Video streamer** | No | Video files and links played by the PS1 with its MDEC (as game FMVs) and sound, up to 320x240 and 15 frames per second; playlist in the GUI |
| **GUI**                              | No (command-line only)                       | Yes (PS1 Dev Cart Manager)                        |
| **Firmware built with**              | PsyQ                                         | PSn00bSDK (no PsyQ required)                      |
| **RAM used** | Menu: 65.2 KB at the top of RAM; while a program runs: none (kernel hook in the unused kernel RAM) | Menu: 34.1 KB at the top of RAM; while a program runs: none (kernel hook in the unused kernel RAM; crash screen, debugger and profiler run from the cartridge flash) |
| **Flashing / recovery** | Yes (flasher run over USB or from a CD, checks the flash on the console) | Yes (from `ps1transfer` or the GUI with the ROM uploaded, read back and compared on the PC; or from a CD) |
| **License**                          | MPL-2.0                                      | MPL-2.0                                           |

## Contents

- [Transfer speed](#transfer-speed)
- [Firmware features](#firmware-features)
- [Tools run on the PlayStation](#tools-run-on-the-playstation)
- [Link cable library (psxcomb)](#link-cable-library-psxcomb)
- [Internet through the computer (-net), audio and video streamers](#internet-through-the-computer--net-audio-and-video-streamers)
- [Netplay](#netplay)
- [Profiler](#profiler)
- [ps1transfer](#ps1transfer)
- [PS1 Dev Cart Manager (GUI)](#ps1-dev-cart-manager-gui)
- [Installing the firmware](#installing-the-firmware)
- [Building](#building)
- [Technical notes](#technical-notes)
- [License](#license)

## Transfer speed

Measured with 1.5MB transferred from the cartridge menu (the original firmware measured on the same cartridge and
computer):

| | Original firmware | PS1 DEV CART | |
|---|---|---|---|
| Download (PlayStation to computer) | 103 KB/s | 840 KB/s | about 8× faster |
| Upload (computer to PlayStation) | 122 KB/s | 655 KB/s | about 5× faster |

What makes the difference:
- `ps1transfer` sets the FTDI latency timer to 1 ms (the default 16 ms delayed the CRC byte the PlayStation sends after
  each packet), and reads/writes whole packets.
- The firmware reads/writes the USB FIFO with one status check per byte, and keeps each byte in a register for the CRC.
- Uploads and downloads from the menu use 16KB packets. `ps1transfer` asks the firmware first, and still works with
  the original firmware (2KB packets).

While a program runs, the kernel hook answers (memory access, file server, synth samples, memory card tools). It
uses 16KB packets too, and the CRC inlined in its transfer loops (the hook has no room for the fast CRC table of the
menu firmware):

| While a program runs | PS1 DEV CART |
|---|---|
| Memory download (`-d`) | 602 KB/s |
| Memory upload (`-u`) | 418 KB/s |
| File server: program reads a file | 341 KB/s |
| File server: program writes a file | 483 KB/s |

The memory card tool reads and writes the card through the controller port registers itself, one 128-byte sector at
a time, with the BIOS driver as a fallback. Measured on an official 128KB card (1024 sectors):

| Memory card | BIOS driver | PS1 DEV CART | |
|---|---|---|---|
| Read | about 42 ms per sector (3 KB/s) | about 10 ms per sector (12.8 KB/s) | about 4× faster |
| Whole card backup (`-mcbackup`) | 43.6 s | 12.5 s | about 3.5× faster |
| Write | – | about 26 ms per sector (4.9 KB/s) | |
| Whole card restore (`-mcrestore`) | – | about 30 s (estimated from the sector writes) | |

The backup times include starting the tool and sending the image to the computer (about 1 s). The card itself is the
limit: it takes about 1 ms to read a sector from its flash, and longer to write one.

Between two consoles, the psxcomb link library is about 2.5× faster than the original link cable library over the
cable, and about 29× faster over the USB link (see [Link cable library](#link-cable-library-psxcomb)).

## Firmware features

**Menu**
- Firmware version and **device ID** (the USB serial number of the cartridge, so you know which
  console is which when several are connected).
- Hold **X** at boot to initialise the CD drive.

**Running programs**
- Upload and execute PS-X EXE programs, reset the console from the computer.
- **printf / TTY** of your program sent to the computer, or kept on the BIOS device, or a serial port "tty" driver like
  the Net Yaroze, or the same driver over USB (see the USB link below).
- **PCDRV file server**: your program reads and writes files on the computer (`psxpcdrv` folder: the usual PCDRV
  functions `PCopen`, `PCread`, ... over USB; programs including `libsn_usb.h` or `libsn.h` build unchanged).
  Only the served folder (`-p`) can be opened: paths going outside it are refused (`-pcdrvany` allows any path).
- **Net Yaroze**: the Net Yaroze library (`libps`) is uploaded and started before your
  program, so Net Yaroze games run from the cartridge.

**While a program runs**
- **Live memory access**: peek, poke, upload and download, with symbol names from your program's `.map`/`.elf`.
- **Live screenshots** of the screen or any VRAM area.
- **Hot reload**: TIM images to VRAM, sounds to SPU RAM and data to RAM, sent again each time you save the file.

**Debugging**

The crash screen, the GDB debugger and the profiler run from the cartridge flash (the debug monitor), not from the
PlayStation's RAM: your program keeps all its RAM.
- **Crash screen** with the cause and the CPU registers, and a **crash report** on the computer with the call stack
  and function names.
- **GDB server** (`ps1transfer -gdb`), for GDB or VS Code. It supports:
  - breakpoints (also in interrupt callbacks), step/next/finish, backtrace, registers, memory, variables;
  - one hardware breakpoint and one watchpoint (read/write/access);
  - pause, then continue;
  - your program's printf shown in the debugger.
- **Profiler** (`ps1transfer -prof`, or the Profiler page of the GUI), without changing your program, see
  [below](#profiler).

**Two consoles**
- Several cartridges on one computer, chosen by serial number.
- **USB link**: two consoles linked through their cartridges and the computer, without a link cable
  (`ps1transfer -relay`).
  - A "tty" driver over USB (`-c link`) lets **Net Yaroze games** that use the serial port play
    over USB.
  - Measured to stay in sync like a link cable.
- **Netplay**: two consoles in different places linked through the internet (Netplay page of the GUI, or
  `ps1transfer -netlink`), see [below](#netplay).
- **psxcomb**: an open source link cable library for your PSn00bSDK programs, compatible with the PlayStation link
  cable library (libcomb), over the cable or over USB (see [below](#link-cable-library-psxcomb)).

**Internet**
- **Internet through the computer** (`ps1transfer -net host:port`): the program's USB link connected to a server on
  the computer, which does the internet side (see [below](#internet-through-the-computer--net-audio-and-video-streamers)).
- **Audio streamer** and **video streamer**: internet radio, audio and video files played by the PlayStation
  (Audio streamer and Video streamer pages of the GUI).
- **Netplay** (above): link play through the internet.

**Flashing**
- `ps1transfer -flash` (or the Firmware page of the GUI) flashes a ROM, then reads the flash back and compares it.

## Tools run on the PlayStation

`ps1transfer` uploads these programs (found next to it) with your files. They share one look (`devcart_ui.h`): a
dark blue screen, a title bar with the tool name and its state, panels for the information, and the controller
buttons in a help bar at the bottom.

| Tool | Option | What it does |
|---|---|---|
| TMD model viewer | `-m model.tmd -t texture.tim` | Shows a 3D model with its textures: rotate, zoom. Its own GTE renderer handles every TMD polygon type (flat/gouraud, textured, lit/unlit, gradation). |
| Sound tester | `-snd bank.vh -snd bank.vb -snd music.seq` | Plays the instruments of a VAB sound bank, SEQ music and VAG samples. Its own VAB/SEQ player matches Sony's libsnd. |
| TIM and VRAM viewer | `-tim file.tim` | Up to 16 TIMs: VRAM map with overlaps, TIM view with zoom and CLUTs, warnings (overlaps, texture page crossing, CLUT position). **Palette editor** and **CLUT swap**, save/reload through the file server. |
| Memory card tool | `-mcbackup`, `-mcrestore`, `-mclist slot1`, ... | Backup (about 12 s) and restore (about 30 s) of a whole card. Browse the saves (with icons) and export, import, copy or delete one save directly on the card (only the sectors needed; writes confirmed on the console). Also works on card image files without a console. |
| SPU synth | `-synth` | The PlayStation as a 16-slot sample synth: slot n plays MIDI channel n. Samples (VAG) uploaded at any time or sent as SysEx; ADSR, reverb, pan, fine tune per slot; MIDI input. |
| Link cable tester | `-link [yaroze\|usb\|yarozeusb]` | Packets sent both ways every frame between two consoles: throughput, losses, round trip time, flow control lines. The serial cable can be used with libcomb (as games using the PlayStation link cable library) or like a Net Yaroze game (tty), or the USB link can be used instead of a cable. |
| Flasher | `-flash rom.rom` | Writes the cartridge flash. Also builds a CD image to repair a cartridge (see below). |

**Palette editor** (TIM viewer): change the colours of a TIM's palette on the PlayStation and see the image redrawn
live, on the real hardware.
- Circle in the TIM view opens it. D-pad: choose the entry, L2/R2: red, green or blue, Triangle/Cross: +/- (held:
  repeats), Square: restore the row's original colours, Circle: exit.
- Start saves the TIM with its edited palette as `NAME_edit.tim` on the computer, Select reloads `NAME.tim` (e.g. after
  editing it in a paint program). Both use the file server: add `-c` (`ps1transfer -tim car.tim -c`), or tick "Palette
  editor save / reload" on the Viewers page of the GUI.

## Link cable library (psxcomb)

The `psxcomb` folder is a link cable library for PSn00bSDK programs, written for this project. It has the same
functions as the PlayStation link cable library (libcomb), so programs written for it build unchanged: they can keep
`#include <libcomb.h>` (a compatibility header in the folder) or include `psxcomb.h`.
- `AddCOMB()`, `DelCOMB()`, `_comb_control()` and all the `Comb...()` macros;
- the `sio:` device with `open()`, and asynchronous `read()`/`write()` (`O_NOWAIT`) ended by the `HwSIO` events
  (`EvSpIOER`, `EvSpIOEW`, `EvSpERROR`, `EvSpTIMOUT`);
- synchronous reads and writes with the wait callback.

**Over the cable** it sends the same signals as the original library (one byte per handshake with DTR/DSR), so it
links with a console running a game built with the original library. Tested with the link tester: one console on the
original library, the other on ours, no packet lost. It doesn't get stuck when an acknowledge arrives late.

**Over USB**, `CombSetLink(COMB_LINK_USB)` uses the cartridges and the computer instead of the cable, with the same
calls, no cable needed (`ps1transfer -n SERIAL1 -relay SERIAL2`).

**Speed**, measured with two consoles sending to each other at the same time:

| | Each way | |
|---|---|---|
| libcomb, link cable, 115200 bps | about 1.6 KB/s | |
| psxcomb, link cable, 115200 bps | about 4 KB/s | about 2.5× faster |
| psxcomb, USB link | about 46 KB/s | about 29× faster |

The original library checks the other console's acknowledge only in its transmit interrupt, so it often waits for
the next chance to send a byte; psxcomb checks it on a timer about twice per byte time. Both send the same signals on
the cable.

    #include "psxcomb/psxcomb.h"        // and ../psxcomb/psxcomb.c in your SRCS

    CombSetLink(COMB_LINK_USB);         // optional: USB link instead of the cable
    AddCOMB();
    ev_read = OpenEvent(HwSIO, EvSpIOER, EvMdNOINTR, NULL);
    EnableEvent(ev_read);
    fd_read = open("sio:", O_RDONLY | O_NOWAIT);
    fd_write = open("sio:", O_WRONLY | O_NOWAIT);
    CombSetBPS(115200);
    read(fd_read, buffer, size);        // ends with the event, like write()

It uses root counter 2 while a serial write is in progress, and all the time with the USB link.

## Internet through the computer (-net), audio and video streamers

`ps1transfer -net host:port` connects the USB link of the running program (psxcomb with `COMB_LINK_USB`, or a Net
Yaroze game run with `-c link`) to a TCP server: what the program writes goes to the server, what the server sends
arrives in the program's reads. The internet side (HTTP, TLS, decoding, game servers) stays on the computer; the
PlayStation only sees bytes on its link. With `-e -f program.ps-exe` it executes the program first. It runs until
stopped (Ctrl+C, or a line on its input), and prints the throughput every second.

    ps1transfer -r -e -f game.ps-exe -net 127.0.0.1:7777     run the game, connect its link to a local server

### PS1 DEV CART AUDIO STREAMER

The `ps1-tools/audiostreamer` folder plays internet radio stations and audio files streamed by the computer (a sample of -net).
- `audio_server.py` (on the computer, `pip install miniaudio numpy av`) plays a playlist of station links and audio
  files: by default six UK stations (Smooth, Capital London, Heart, Classic FM, Heart 80s and Radio X Classic Rock).
  Icecast/SHOUTcast MP3 stations are decoded with miniaudio (with the song titles of the stream); local files of any
  format FFmpeg reads (MP3, FLAC, WAV, OGG, M4A/AAC, Opus, ...) and links in other formats with PyAV (with the file's
  artist/title tags). The sound is resampled to 22050 Hz mono (`--rate`), encoded to SPU ADPCM (the PlayStation's own
  sound format, 12.6 KB/s) and sent at most 2.5 s ahead of real time. At the end of a file the next one plays.
- `audiostreamer.ps-exe` (PSn00bSDK, psxcomb) buffers about a second, plays it on one SPU voice from a looping buffer
  in SPU RAM refilled half by half, and shows the station or file, the song, the buffer and the SPU RAM used.
  Left/Right goes to the previous/next in the playlist (sent back to the server), Up/Down changes the volume.

      python ps1-tools/audiostreamer/audio_server.py [station links or audio files]
      ps1transfer -r -e -f ps1-tools/audiostreamer/audiostreamer.ps-exe -net 127.0.0.1:7777

The PS1 Dev Cart Manager's Audio streamer page does both, with a playlist you can edit (add files or links).

Measured: 12.3 to 13.4 KB/s steady through the bridge (the stream's real time rate). The link itself carries about
160 KB/s into the PlayStation (bridge + psxcomb with a 16KB ring, measured with a program that only reads).

### PS1 DEV CART VIDEO STREAMER

The `ps1-tools/videostreamer` folder plays videos streamed by the computer, with the PlayStation's own video hardware (the MDEC,
as the FMVs of the games) and sound (SPU ADPCM).
- `video_server.py` (on the computer, `pip install av numpy miniaudio`) opens any video file or stream FFmpeg can open
  (PyAV), fits the picture in a 320x240 screen (full resolution, `--size 160x120` for a smaller stream shown 2x) and
  sends only the picture (not the letterbox bars, which cost the MDEC as much as the picture), at the source's frame
  rate divided by a whole number up to 15 (24 -> 12, 30 -> 15: even steps). The frames are encoded to MDEC frames
  (BS version 2, the frame format of STR videos) with `mdec_encoder.py` (numpy), within a budget of 90 KB/s (the
  quantization goes up for busy frames). The sound is resampled to 22050 Hz mono and encoded like the audio streamer's.
  By default it plays the Sintel trailer and film (Blender Foundation, CC BY 3.0), streamed from blender.org; or give
  files/URLs.
- `videostreamer.ps-exe` buffers about a second and plays the sound as the clock. The next frame is decoded ahead
  (PSn00bSDK's `DecDCTvlc`, then the MDEC decodes the whole frame in the background while the link, the sound and the
  screen go on), into the texture not shown, and shown at its time; late frames are dropped. Select shows/hides the
  information (video, picture size, frame rate, buffer, KB/s, dropped frames), Left/Right changes the video, Up/Down
  the volume.

      python ps1-tools/videostreamer/video_server.py [video files or URLs]
      ps1transfer -r -e -f ps1-tools/videostreamer/videostreamer.ps-exe -net 127.0.0.1:7778

Measured with the Sintel trailer (320x134 picture, 12 frames per second): about 80-95 KB/s through the link, quantization
scale 2-4 (high quality), 306 frames shown and 2 dropped in 30 s, no sound underrun. The MDEC takes about 32 ms for a
320x240 frame on its own.

## Netplay

Link play between two consoles in different places, through the internet: **Net Yaroze games** using the serial
link (`tty:`, run with `-c link`) and **homebrew using psxcomb's USB link**.

In the GUI's **Netplay** page, one player **hosts** the lobby (no server to rent: it runs in their PS1 Dev Cart
Manager), the others **join** it with the host's internet address. The lobby lists the players and the game each one
has chosen, with a chat (to everyone or one player) and **invites**. When an invite is accepted, both consoles are
reset, the game is started and linked. Both players need the same game file (checked).

The host's router is asked to open the ports itself (UPnP, on by default on most home routers) and the page shows
the address the other players join with (from the router, else from a STUN server), with a Copy button. Without
UPnP, the host forwards TCP and UDP 7010 and UDP 7011-7014 to their computer; the page says so, and warns when the
provider shares the internet address between customers (carrier-grade NAT: nobody can reach you, another player
must host). The guests need nothing. Two guests playing together start through the host's relay and try a direct
link through their routers (UDP hole punching), the relay staying as the fallback. A dropout of up to 30 seconds
doesn't end a game (the data waits and is sent again), and a lost lobby connection reconnects by itself without
stopping the game.

The link (`ps1transfer -netlink`) is built for latency: direct between the two computers (UDP), each packet carries
everything not acknowledged yet (a lost packet is covered by the next one, without waiting), units sent as soon as
the console gives them. Measured with the link tester (round trip, 60 Hz frames): 2.5 frames on the same network
(the local relay: 3.0), 3.0 at 15 ms each way, 3.9 at 30 ms, 4.2 at 30 ms with jitter and 5% packets lost (none
lost by the program), 6.2 at 70 ms. Games that send every frame and use what arrived (like YarIO) run at full speed,
the other player's moves appear a little later; games that wait for each answer slow down above ~15 ms each way.

    ps1transfer -n SERIAL -r -y libps.exe -e -f game.exe -c link -netlink :7010 -netkey KEY        host
    ps1transfer -n SERIAL -r -y libps.exe -e -f game.exe -c link -netlink 1.2.3.4:7010 -netkey KEY  guest
    ... -netdelay 30          try it as if the other console was 30 ms away (both consoles on one computer)

**Security.** Each game gets its own random 128-bit link key from the lobby, given only to its two players; every
link packet carries a tag made with it (HMAC-SHA256), and ps1transfer, the host's relay and the bot ignore packets
without the right tag: nobody else can link in, send data to a console or stop a game, even knowing the ports. The
lobby password is never sent (the lobby sends a random challenge, the Manager answers with a hash made with the
password), and with a password the games' keys are sent encrypted with it. An address that gets the password wrong
5 times is refused for a minute; at most 4 connections from one address, and a connection that doesn't log in
within 5 seconds is closed. Hosting without a password asks first: then anyone with your address can join and
chat (the games stay protected). Chat, names and game names are not encrypted. Use a password of 10 characters or
more: someone who can see your traffic could try passwords against the challenge offline. Without the lobby,
`-netkey` takes the same 32 hex digit key on both computers (e.g. made with
`python -c "import os; print(os.urandom(16).hex())"`); without `-netkey` anyone can link in first.

**Testing without a second player:** `gui/netplay_bot.py` (Python 3 only, nothing to install) is a pretend player.
Run it on another network (a laptop on your phone's hotspot, an Android phone with Termux on mobile data, any PC):
`python netplay_bot.py YOUR.INTERNET.ADDRESS` (`--delay`, `--jitter`, `--loss` to make it worse). It joins your
lobby, accepts invites and plays the other console of the link tester: host, choose `ps1transfer/linktest.ps-exe`
with the link "Net Yaroze game (tty, -c link)", invite the bot. The tester on your console shows the round trip and
lost packets of the real connection, the bot posts its own figures in the chat at the end. Testing from your own
network through your internet address often fails (routers rarely loop back): use another network.

## Profiler

`ps1transfer -prof` samples the running program 1000 times a second, through the cartridge, and shows every second:
- **CPU load** and **where the time goes**, by function (with the program's symbols: the `.elf` or `.map` next to it,
  or `-sym`). Time in the functions where a program waits (VSync, DrawSync, CdSync...) is not CPU load; `-idle` adds
  your own (or a double click in the GUI), e.g. for a program without symbols;
- **GPU busy** (drawing or its DMA running), **frames per second** (swaps of the draw buffers);
- **DMA activity** (GPU, SPU, CD-ROM, MDEC, ordering table clear) and the CD-ROM controller busy;
- **RAM used** of the PlayStation's 2 MB: the kernel (64 KB) and your program with its variables, plus the free RAM it
  has written since it started. With `-e`, the firmware fills the free RAM above the program with a pattern before
  starting it, and the words written since are counted, e.g. "RAM 1980/2048 KB (program 1518 KB + 398/398 KB free
  written)". Attaching to a running program measures it too if it was started with `-prof`. `-nofill`: not filled
  (not measured), e.g. if you uploaded data there yourself;
- **stack**: the lowest stack pointer seen;
- **SPU voices** playing (envelope of the 24 voices).

At the end, the functions with their share of the time.

    ps1transfer -e -f game.exe -prof              run the game and profile it until Ctrl+C
    ps1transfer -prof 500                         profile the program already running, 500 samples a second
    ps1transfer -e -f game.exe -prof -csv prof.csv  also one line per second in a CSV file

How it works: a root counter the program doesn't use (0 by default, `-prof rate,counter`) interrupts it. The debug
monitor (in the cartridge flash) copies a small sampler to its work area in the unused kernel RAM, not the
program's RAM, and points the exception vector at it, so the ticks don't go through the BIOS
interrupt handling (about 90 µs each). At each tick the sampler sends the program counter, return address, stack
pointer and the hardware state to the computer. Measured cost: about 4% of the program's time at 1000 samples a
second (2% at 500). The sampler (hand scheduled MIPS assembly, 632 bytes) uses the debug monitor's RAM, so it can't
be used during a `-gdb` session; it works with the link tty drivers (`-c sio`, `-c link`: link play). Statistical
sampling attributes interrupt handlers' time to the code they interrupted.

## ps1transfer

The command line tool on the computer (Windows and Linux). All the options are in
[ps1transfer/README](ps1transfer/README). Some examples:

    ps1transfer -e -f game.exe -c                 run a program and show its printf
    ps1transfer -v 0 0 320 240 -f shot.png        screenshot (live while a program runs)
    ps1transfer -peek @lives -s 1                 read a variable of the running program
    ps1transfer -gdb -f game.exe                  debug with GDB / VS Code (target remote localhost:3333)
    ps1transfer -hot sprite.tim -watch            hot reload a texture each time it is saved
    ps1transfer -e -f game.exe -prof              profile a program (CPU/GPU load, functions, RAM...)
    ps1transfer -mclist slot1                     list the saves of the memory card in slot 1
    ps1transfer -n SERIAL1 -relay SERIAL2         USB link between two consoles
    ps1transfer -l                                list the connected cartridges

Windows needs the FTDI D2XX driver; Linux needs libftdi1.

## PS1 Dev Cart Manager (GUI)

A window for `ps1transfer` (Python, or a single `.exe` without Python), so you don't have to remember the options.
Each page shows the command it runs. See [gui/README.md](gui/README.md).

| Page | |
|---|---|
| Top bar | Cartridge chooser (by device ID), its state (menu, running, stopped, crashed) and firmware; the folder served while the file server runs; stop task, reset. |
| Run | Upload and run a program, TTY mode, Net Yaroze library, file server. |
| Console | printf of the running program; file server activity (files opened, created and refused, bytes read and written). |
| Memory | Peek, poke, upload, download, with symbols. |
| Screen | Screenshots, live while a program runs, with automatic refresh. |
| Debug | Crash report, GDB server. |
| Hot reload | Files and where they go, sent at once or each time they are saved. |
| Viewers | TMD model viewer, TIM and VRAM viewer. |
| Sound | Sound tester with up to 8 files. |
| Synth | 16 slots, keyboard, WAV to VAG conversion, MIDI input, SysEx, kits. |
| Memory card | Backup/restore, save browser with icons, on a card or an image file. |
| Link test | Link tester on two consoles, USB link relay for your own programs. |
| Video streamer | Playlist of video files or links (Blender examples filled in), picture/frame rate/quality options, start/stop with the server in the GUI. |
| Audio streamer | Playlist of radio station links and audio files (UK stations filled in), sound rate, start/stop with the server in the GUI. |
| Netplay | Hosts or joins a lobby: players and their games, chat, invites; starts and links both consoles over the internet. |
| Profiler | Runs (or attaches to) a program and shows CPU/GPU load, frames, RAM, stack, DMA, SPU voices and the functions where the time goes, live. |
| Firmware | Cartridge information, flashing with a typed confirmation and a readback check. |

## Installing the firmware

**Warning:** flashing the wrong file, or interrupting a flash, can leave the cartridge unable to boot. If that
happens, it can be repaired with the flasher CD image (below).

- **From the computer:** with the cartridge on its menu, run `ps1transfer -flash cartrom/ps1devcart.rom`, or use the
  Firmware page of the GUI. The ROM is checked first, flashed, then read back and compared.
- **Repair or install from a CD:** `make` in the `ps1-tools/ar_flash` folder also builds `ar_flash.bin`/`ar_flash.cue`, a CD image
  with the flasher. Burn it to a CD-R or use a CD emulator. Then:
  1. Start the console with the cartridge switch **OFF**.
  2. Boot the CD.
  3. Switch the cartridge **ON** before flashing.

## Building

**Requirements**
- [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) (tested with the v0.24 release package and its GCC 12
  toolchain) for the firmware and the PlayStation tools.
- [nolibgs_hello_worlds](https://github.com/ABelliqueux/nolibgs_hello_worlds) for the build rules of the kernel hook
  (no library of it is linked).
- `make`, Python 3 (menu images, GUI), a host C compiler for `ps1transfer` and `cartrom/rom_rounder`.
- Copy `config/Makefile.conf.example` to `config/Makefile.conf` and set `PSN00BSDK` and `PSXDIR`.

**Build steps**

    make                    firmware (ps1devcart.ps-exe), kernel hook, debug monitor
    make -C cartrom         cartridge ROM (cartrom/ps1devcart.rom)
    make -C ar_flash        flasher, with the ROM built in, and its CD image
    make -C tmdviewer       (same for soundtest, timviewer, cardtool, synth, linktest)

`build.sh` / `build.bat` build everything. `ps1transfer` has Makefiles for Windows and Linux
(`Makefile_win`, `Makefile_linux`) and Code::Blocks projects.

Windows (MSYS make): if the compiler says "Cannot create temporary file in C:\Windows\\", pass the temp folder to make:
`make TMP="$(cygpath -w $TEMP)" TEMP="$(cygpath -w $TEMP)"`.

**Testing a firmware without flashing:** `make RAMTEST=1` builds a version that runs from RAM (0x801D0000), with the
debug monitor copied to RAM. Run it from the cartridge menu with `ps1transfer -r -e -f ps1devcart.ps-exe -c off`.

**Optional:** `make psyq` in `linktest` builds `linktest_psyq.ps-exe`, the link tester with the original PsyQ
library, to compare with ours (`ps1transfer -link -f linktest_psyq.ps-exe`). It needs your own PsyQ and is not
included.

## Technical notes

- The menu runs from RAM at 0x801EF000: it is copied there from the cartridge flash at boot (`cartrom`, based on
  PSn00bSDK's cartrom example), as the original PS1USB firmware does at the same address. It takes 34.1 KB there
  (0x801EF000 to 0x801F7854, code, data and variables), the original firmware 65.2 KB (up to 0x801FF4E8). This RAM
  is only used while the menu is shown: a program you run gets all the PlayStation's RAM (it can load over the
  menu). What stays while it runs is the kernel hook, in the BIOS's unused kernel RAM (below), as with the original.
- Flash (128KB):
  - firmware at the start;
  - menu images at 0x1F010000 (a small file system of TIMs, edited with `cartrom/tfs_tool.py`);
  - debug monitor at 0x1F01C800;
  - device ID saved in the last sector (0x1F01F000).
- The **kernel hook** (0x8000C160 to 0x8000DF7F, unused kernel RAM) answers the computer while a program runs: memory
  access, reset, TTY, USB link. Its parameters are at 0x8000DF70 (TTY mode, packet size, features, pointer to its USB
  service, which tools also call directly to answer faster).
- The VRAM is saved to RAM (0x80020000) at boot, so `-v` from the menu shows the last screen before a reset or crash.
- The transfer protocol is in `usb.h` (commands with CRC8, data packets with CRC and acknowledge). This firmware also
  has escaped answers: a `Q` byte before a command makes its answer "DLE g" / "DLE b" instead of `G` / `B`, so text a
  program prints on the USB tty can't be taken for it. Our ps1transfer asks for them; without `Q` (e.g. the original
  ps1transfer) the answers stay the same.
- `psyq_compat.h` holds the few PsyQ calls the programs used that PSn00bSDK doesn't have (pads in PsyQ's bit layout,
  `ClearImage`, display set-up). `psn00b_tool.mk` has the shared build rules of the tools.

## License

Mozilla Public License 2.0, see [LICENSE](LICENSE). Some source files keep the notices of their original authors, as
the license requires. Third-party parts:
- `cartrom/nugget`: MIT license (PCSX-Redux authors).
- `ps1transfer/lodepng.c`, `lodepng.h`: LodePNG by Lode Vandevenne, zlib license (notice in the files).
- `ps1transfer/ftd2xx.dll`, `ftd2xx.h`, `ftd2xx.lib`: FTDI D2XX driver, FTDI's driver licence (redistributable with
  products using FTDI chips: https://ftdichip.com/driver-licence-terms/).
- The firmware and the PS1 tools are built with PSn00bSDK (Mozilla Public License 2.0), parts of its libraries are in
  them.
- The PS1 Dev Cart Manager executable (`gui/dist`, made with PyInstaller) contains Python (PSF license), ttkbootstrap
  and Pillow, mido and python-rtmidi, miniaudio (MIT/HPND style licenses), and PyAV with FFmpeg and the libraries it
  is built with (FFmpeg: LGPL 3 or later; x264/x265: GPL 2 or later; others: see the notice), used by the video and
  audio streamer pages to decode files and links. What they are, their licenses and where their source is:
  [gui/licenses/FFMPEG-NOTICE.txt](gui/licenses/FFMPEG-NOTICE.txt), with the license texts in `gui/licenses` (also
  in the executable: Settings page, "Third-party licences"). Running the Manager from the Python sources needs none
  of them bundled.
- The crash screen is based on the PS1 Crash Handler by Spencer Alves (public domain).
- The software reset and the first kernel hook were contributed by david4599 (https://github.com/david4599/ps1usb/).

## Credits
- OrionSoft for the PS1USB device, original firmware, original PS1Transfer tool and the flasher tool (https://github.com/OrionSoftGames/ps1usb)

