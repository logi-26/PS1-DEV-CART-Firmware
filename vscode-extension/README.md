# PS1 DEV CART for VS Code

(Earlier name: *Net Yaroze*. Since version 2.6 the settings, commands,
`launch.json` type and task type are named `ps1devcart` (before:
`netyaroze`). Settings made with an earlier version are still read, and a
`launch.json` or `tasks.json` with the earlier names is offered an update when
the folder is opened. Keybindings to the earlier command names need the new
ones (`netyaroze.build` is now `ps1devcart.build`). Uninstall the earlier *Net
Yaroze* extension before installing this one: both would add the same
commands.)

Build, clean, run and debug PlayStation projects from VS Code, in PCSX-Redux
or on a PlayStation with a [PS1 DEV CART](https://github.com/logi-26/PS1-DEV-CART-Firmware):

- **Net Yaroze** games, with the
  [Net Yaroze WSL build environment](https://github.com/logi-26/Net-Yaroze-WSL-Build-Environment)
  (installed in WSL by the extension);
- **PsyQ / PsyQo** games, e.g. made from the
  [PSX.Dev](https://marketplace.visualstudio.com/items?itemName=grumpycoders.psx-dev) templates (nugget);
- **PSn00bSDK** games (CMake, or a Makefile).

See [Kinds of projects](#kinds-of-projects).

## Features

- **Build, Clean, Rebuild, Run** and **Build Models** commands and tasks,
  which run the project's makefile in WSL (`make`, `make clean`,
  `make clean all`, `make run`, `make models`).
  - **Ctrl+Shift+B** builds (while a project is open, the extension binds it
    to *PS1 DEV CART: Build*).
  - **Build** and **Run** buttons at the top right of the editor, and
    **Build**, **Run** and **Clean** buttons in the status bar (bottom left).
  - All of them in *Terminal > Run Task... > PS1 DEV CART* (Net Yaroze projects: *Net Yaroze*, PsyQ: *PsyQ / nugget*, PSn00bSDK: *PSn00bSDK*).
- **Build CD Image** and **Run CD Image** (*PS1 DEV CART: Build CD Image*,
  *PS1 DEV CART: Run CD Image in Emulator*): run `make iso` and
  `make run-iso`, which pack the game into a bootable CD image
  (`game.bin`/`game.cue`) with mkpsxiso and start it in the emulator. Offered
  for projects whose makefile has `iso` and `run-iso` targets (new projects
  and the repo's example do); see the repo's `docs/mkpsxiso.md`.
- **Run Linked** for link play: runs `make link`, which should start the game
  in two emulators linked through their serial ports (e.g. PCSX-Redux's SIO1
  server and client). The command, its editor button and its status bar
  button appear only for projects whose makefile has a `link` target.
- **Compiler errors and warnings in the Problems panel**; click one to go to
  the line. (Warnings in the SDK's own headers are left out.)
- **IntelliSense for libps**: *PS1 DEV CART: Configure IntelliSense for libps*
  sets up Microsoft's C/C++ extension with the SDK headers and the compiler's
  predefined macros, so `#include <libps.h>` and the libps functions are
  known. It is offered when a project without a C/C++ configuration is
  opened. It also turns off the C/C++ extension's *Run/Debug C/C++ File*
  button (▷ on C files) for the workspace (`C_Cpp.debugShortcut`): that
  button builds Windows programs, not Net Yaroze ones.
- **New Project**: *PS1 DEV CART: New Project...* creates a small working
  project (`main.c`, `Makefile`, `auto`, `iso.xml` and `system.cnf` for the
  CD image, and `.vscode` settings so that F5
  debugs) in an empty folder.
- **Templates** (the panel's orange button, or *PS1 DEV CART: New Project
  from Template...*), like the PSX.Dev extension: a template, then the folder
  and the project's name (letters, digits, `-`, `_`; no space in the path);
  the project is made in a new folder, then opened or added to the workspace.
  - **Net Yaroze**: Hello World (the project of *New Project*).
  - **Bare metal**: Empty (Nugget); **Psy-Q SDK**: Psy-Q Cube, Net Yaroze
    Sprite (Psy-Q, [psyq_to_netyaroze](https://github.com/gwald/psyq_to_netyaroze));
    **PSYQo SDK**: Hello World, Cube. PSX.Dev's templates (MIT licence,
    `templates/psxdev/LICENSE.txt`): a git repository with the libraries as
    submodules (nugget...; needs [git](https://git-scm.com/downloads)), and
    for the Psy-Q ones the converted Psy-Q SDK PSX.Dev uses, in
    `third_party/psyq` (downloaded once). PSX.Dev's CMake bare metal and
    xmake templates aren't offered (the extension doesn't build those).
  - **PSn00bSDK**: Hello World (CMake), PSn00bSDK's own template (from the
    installed PSn00bSDK).

  The tools the project needs that are missing can be installed at the end.
- **Debugging in PCSX-Redux**: breakpoints on source lines and functions,
  stepping, the call stack, registers and memory, in VS Code (see
  [Debugging](#debugging)).
- **Run and debug on a PlayStation** with a
  [PS1 DEV CART](https://github.com/logi-26/PS1-DEV-CART-Firmware) cartridge:
  **Run on PS1**, **Debug on PS1**, and a status bar button to choose the
  cartridge (see [On a PlayStation](#running-and-debugging-on-a-playstation-ps1-dev-cart)).
- **PNG to TIM**: right-click a `.png` in the Explorer, *Convert PNG to
  TIM...*: with the same settings (and optionally the same palette) as an
  existing `.tim` of the same name, or with new VRAM positions. Uses
  `png2tim` (included); see the repo's `docs/png2tim.md` for more options.

A folder is recognised as a project when it contains an `auto` script and a
makefile (Net Yaroze), a Makefile using nugget or making a PS-X EXE (PsyQ,
PsyQo, PSn00bSDK makefiles), or a `CMakeLists.txt` with
`psn00bsdk_add_executable` (PSn00bSDK). It can be the folder opened in VS Code or any folder
inside it (e.g. `example/` when the whole repo is open); the commands run
in the project's folder. With several projects, the one containing the file
being edited is used, otherwise you are asked (tasks of projects in
subfolders show the folder, e.g. *Build (example)*).

## Installing

The extension is the `.vsix` file in the [release](https://github.com/logi-26/PS1-DEV-CART-Firmware/releases)
zip (or build it: `npm run package` in this folder). In VS Code: **Extensions** view, **...** menu, **Install from VSIX...**, then choose
the file; or from a terminal:

    code --install-extension ps1-dev-cart-2.6.0.vsix

It comes with `ps1transfer` and the PlayStation tools, so nothing else is needed to run and debug on a PS1 DEV
CART. The PS1 DEV CART panel (activity bar) installs the toolchains a project needs.

## Requirements

- Windows with WSL 2 and the Net Yaroze WSL build environment installed
  (toolchain, SDK and `yarexe`; `rsdlink` for Build Models, `mkpsxiso` for
  the CD image commands), see the repo's
  README.
- A makefile with `all`, `clean` and `run` targets, like the repo's example
  (`make run` starts the emulator; `make models` builds models).
- For IntelliSense and debugging: Microsoft's C/C++ extension
  (`ms-vscode.cpptools`).
- For debugging: PCSX-Redux, and `gdb-multiarch` in WSL
  (`sudo apt install gdb-multiarch`).

Also works when the folder is opened in WSL (*Remote - WSL*): the commands
then run there directly.

PsyQ / PSn00bSDK projects need other tools, see below.

## Kinds of projects

| | Net Yaroze | PsyQ / PsyQo / nugget | PSn00bSDK (CMake) |
|---|---|---|---|
| Recognised by | `auto` script and a makefile | Makefile including nugget's `common.mk` (PSX.Dev templates), or with `TYPE = ps-exe`, or using PSn00bSDK | `CMakeLists.txt` with `psn00bsdk_add_executable` |
| Built | in WSL: `make` | on Windows: `make BUILD=Debug` with the MIPS toolchain (`ps1devcart.mipsToolchainPath`, PSX.Dev's by default) | on Windows: the `default` CMake preset (or PSn00bSDK's toolchain file), `cmake --build build`, with `ps1devcart.psn00bsdkPath` |
| Program | `psx.exe` (yarexe, libps included) | `TARGET.ps-exe` of the Makefile | the PS-X EXE in `build/` |
| Debug information | made from `main.exe` (`yaroze_debuginfo.py`) | `TARGET.elf` | the ELF next to it in `build/` |
| GDB | `gdb-multiarch` in WSL | Windows `gdb-multiarch` of the MIPS toolchain (else WSL's) | the same |

Every command works with every kind: Build, Clean, Rebuild, Run (in
PCSX-Redux: `ps1devcart.emulatorPath`, needed for PsyQ / PSn00bSDK projects),
Run on PS1, Debug (emulator and PS1 DEV CART), the link play commands and
Reset. Build Models and the CD image commands are for Net Yaroze projects
(their makefile targets). `ps1devcart.buildConfiguration` chooses `Debug`
(line-by-line debugging, the default) or `Release` for PsyQ / PSn00bSDK
builds.

PsyQ / PSn00bSDK programs have full debug information: variables with their
types, structures and parameters show in the debugger (Net Yaroze programs
have line information only).

Requirements for them: the MIPS toolchain with `make` and `gdb-multiarch`,
for PSn00bSDK its release package, CMake and Ninja; for debugging Microsoft's
C/C++ extension. The extension installs them (see [Tools](#tools)).

## Tools

Like the PSX.Dev extension, the extension downloads and installs the tools
the projects need, in its own storage (nothing system wide, no administrator
rights). You choose which: the **Tools** section of the panel has a checkbox
for each toolchain (Net Yaroze, PsyQ / PsyQo, PSn00bSDK, the emulator, the
debugger), and *Install selected* installs only the missing tools of the
ticked ones, e.g. a PsyQ developer gets the MIPS toolchain without PSn00bSDK,
CMake and Ninja. *Install* next to a tool installs that tool alone, and *PS1
DEV CART: Install Tools...* asks which toolchains to install. The ticked
toolchains are remembered; until you change them, they are the ones of the
workspace's projects. When a workspace opens, it offers to install the tools
its projects need, and only those (`ps1devcart.offerTools`).

| Toolchain | Its tools |
|---|---|
| Net Yaroze | the Net Yaroze build environment (WSL), gdb-multiarch in WSL |
| PsyQ / PsyQo | the MIPS toolchain |
| PSn00bSDK | PSn00bSDK, CMake, Ninja, the MIPS toolchain (for its gdb-multiarch) |
| Emulator | PCSX-Redux |
| Debugger | the C/C++ extension |

| Tool | For | Installed from |
|---|---|---|
| PCSX-Redux | every project (Run, Run Linked, Debug in the emulator) | the PCSX-Redux builds (distrib.app, as PSX.Dev), with the Visual C++ runtime if missing |
| MIPS toolchain (GCC, make, gdb-multiarch) | PsyQ / PsyQo / nugget; debugging PsyQ and PSn00bSDK | the PSX.Dev archives (GCC 16.2, GDB 17.2) |
| PSn00bSDK | PSn00bSDK projects | its latest release (win32) |
| CMake, Ninja | PSn00bSDK projects | their latest releases (portable zips) |
| C/C++ extension | debugging | the Marketplace |
| ps1transfer | the cartridges | comes with the extension |
| Net Yaroze build environment | Net Yaroze projects | in WSL, `/opt/yaroze` (see below); *gdb-multiarch* in WSL (`sudo apt install`) |

**Net Yaroze build environment**: installed in WSL (`/opt/yaroze`, the
[Net Yaroze WSL build environment](https://github.com/logi-26/Net-Yaroze-WSL-Build-Environment)'s
layout) from:

- the toolchain that comes with the extension: GCC 2.8.1 and binutils 2.16.1
  for `mipsel-unknown-ecoff` (PSX3's compiler version; GNU tools only, see
  `yaroze/NOTICE.txt`),
- Sony's SDK (headers, `libps`, linker script) read from the *Net Yaroze
  Software Development Disc* image, downloaded from the Internet Archive
  (`ps1_sdks`, 75 MB; kept in the extension's storage), set up as PSX3 has it
  (with `ioctl()`, used for the tty of link play),
- yarexe, built in WSL from [its source](https://github.com/gwald/Yarexe).

WSL is needed (`wsl --install`). The install runs in a terminal with `sudo`:
it asks for the WSL password, and installs `gcc`, `libc6-dev` and `git` if
missing (for yarexe). It is installed over an existing `/opt/yaroze` (other
tools there, e.g. mkpsxiso or rsdlink, stay).

Tools already installed are used as they are: the paths in the settings,
PSX.Dev's toolchain (`psxDev.toolchainPath` or `%APPDATA%\mips`) and
PCSX-Redux, `PSN00BSDK_LIBS`, CMake in Program Files, the PATH.

## Debugging

Set breakpoints in your C files, then start **PS1 DEV CART: Debug in
Emulator**: the bug button at the top right of the editor, the **Debug**
status bar button, or F5 with a `launch.json` of type `ps1devcart` (below; new
projects have one). Without a `launch.json`, F5 asks for a debugger: choose
*PS1 DEV CART (PCSX-Redux or a PlayStation)*; *PS1 DEV CART: Debug in PCSX-Redux* is also listed
in the Run and Debug view. (Not the C/C++ extension's ▷ *Run/Debug C/C++
File* button: that builds a Windows program.) The extension:

1. builds the project (not again when it is up to date, see below),
2. makes a debug symbol file (`main.elf`) with the build environment's
   `yaroze_debuginfo.py` (included),
3. starts PCSX-Redux with its GDB server on, with `psx.exe` loaded but paused,
4. starts `gdb-multiarch` in WSL through the C/C++ extension, connects it to
   the emulator and runs the game.

The game stops at your breakpoints; step, look at the call stack, registers
and disassembly as usual. The game's `printf` output appears in the Debug
Console. Stopping the debugger closes the emulator.

To keep the settings in `.vscode/launch.json` (e.g. to stop at `main`):

```json
{
    "version": "0.2.0",
    "configurations": [
        { "type": "ps1devcart", "request": "launch", "name": "PS1 DEV CART: Debug in PCSX-Redux", "stopAtEntry": true }
    ]
}
```

Options: `program` (default `main.exe`), `project` (the project's folder,
relative to the workspace folder, `""` for the folder itself; without it
you're asked when the workspace has more than one project), `stopAtEntry`, `debugLog` (shows the communication with GDB in
the Debug Console, for troubleshooting).

What to expect (see the repo's `docs/debugging.md` for why):

- **No variable types.** Local variables can't be shown. Global variables
  work in Watch expressions with a cast, e.g. `(int)gScreenWidth`.
- **Line detail depends on optimisation.** With `-O2`/`-O3` some lines have
  no code of their own and stepping jumps around; build with `-O0 -g` for
  line-by-line stepping.
- The call stack stops at `main`: there is no caller above it that has
  source (the Net Yaroze startup code is in the monitor/BIOS).
- **Pause** usually catches the game in the interrupt handler or libps,
  where there is no source to step through. The extension then lets the game
  run on to the next function of your program (or line of `main`) and stops
  there, usually a moment later. Turn this off with
  `ps1devcart.pauseInProgramCode`.
- The emulator started for debugging is a separate one from **Run**; its
  window shows the game as usual.

## The PS1 DEV CART panel

The **PS1 DEV CART** icon (a cartridge) in the activity bar opens a panel with a button for
each action, and the PS1 DEV CART cartridges connected to the computer:

- **Templates**: a new project from a template (see above).
- **Build**.
- **Emulator**: the emulator used for every kind of project (Net Yaroze,
  PsyQ, PsyQo, PSn00bSDK), with **Browse...** to choose it
  (`ps1devcart.emulatorPath`). Without one, PCSX-Redux installed from Tools.
- **Cart** and **Second cart**: the cartridges found (`ps1transfer -l`, looked
  for again every 15 seconds while the panel is shown, or with the refresh
  button). *Cart* is the one Run, Debug and Reset use
  (`ps1devcart.devcartSerial`), *Second cart* the other console of link play
  (`ps1devcart.devcartSerial2`): never the same cartridge (the Second cart
  list leaves out the Cart). *None* when not connected: no cart (Cart), fewer
  than two (Second cart, Link); the buttons using them are disabled then,
  the emulator ones stay. A cartridge another program has open (the
  PS1 Dev Cart Manager, a Run on PS1 terminal) is shown *(in use)*.
- **Link**: how the two consoles of link play are linked
  (`ps1devcart.devcartLink`): *Cable* (a link cable: the game runs with the
  serial port TTY driver, like the Net Yaroze boot disc) or *USB link* (no
  cable: the same driver over the USB link, and ps1transfer's relay joins the
  two cartridges through the computer).
- **Deploy**: to the emulator (**Run**), to the selected cart (**Run on
  PS1**), the link game to two emulators (**Run Linked**), or to two carts
  (**Run Linked on Two PS1s**).
- **Debug**: in the emulator, on the selected cart, the link game in two
  emulators (**Debug Linked**) or on two carts (**Debug Linked on Two
  PS1s**).
- **Reset cart (SERIAL)**: a button for each connected cart, usable when it
  is the Cart or the Second cart (its console goes back to the PS1 DEV CART
  menu), and **Clean**.

### Link play

- **Run Linked (Two Emulators)** runs the makefile's `link` target if it has
  one; otherwise the extension starts two PCSX-Redux itself
  (`debug/run-linked.ps1`), linked through their serial ports (SIO1): the
  first is the server, the second connects to it. Each gets its own settings
  folder (`.pcsx-link/` in the project), made from your usual PCSX-Redux
  settings; yours aren't changed.
- **Debug Linked (Two Emulators)**: the same two emulators, each with its GDB
  server (`ps1devcart.gdbPort` and the next port), paused until two debug
  sessions (one per emulator, side by side in the Call Stack) run them.
- **Run Linked on Two PS1s**: builds, then starts `psx.exe` on both carts at
  once (`ps1transfer -n CART -r -e -f psx.exe -c sio`, or `-c link`). With the
  USB link, `ps1transfer -n CART1 -relay CART2` then links the two consoles in
  the background (its output in the PS1 DEV CART output, as the PS1 Dev Cart
  Manager runs it), until it is stopped: the status bar item *Relay* (a click),
  *PS1 DEV CART: Stop the USB Link Relay*, Reset cart, or the next deploy.
- **Debug Linked on Two PS1s**: the game runs on the second cart, and is
  debugged on the main cart (`ps1transfer -gdb`, stopped at its entry point, one
  debug session). With the USB link, that GDB server also runs the relay
  between the two carts (`-relay CART2`: the main cart's USB is the
  debugger's; the main cart's firmware needs the debug monitor version 12).

**Up to date**: Deploy and Debug don't build again a program built here (with
the same build type) when no source file changed since: C, headers, assembly,
makefiles, CMake files, and data files a makefile embeds (TIM, TMD, VH/VB, SEQ,
VAG...). Open files are saved first. **Build** always builds, **Clean**
forgets the last build.

The game's link code must use the serial port tty (`open("tty:")`,
`read()`, `write()`, `ioctl()`, e.g. YarIO). On a console the boot disc
provides it (on a PS1 DEV CART, the firmware: `-c sio`); in the emulator
there is neither, so for a Net Yaroze project Run Linked and Debug Linked run
a copy of `psx.exe` with the link driver added (`.pcsx-link/psx-link.exe`):
the copy starts with a small loader that puts the driver (`yario_emu.c` from
yaroze-racer, in `linkdriver/`) in kernel RAM no BIOS uses (`0x8000C400`,
2 KB), installs it as the BIOS `tty` device (PCSX-Redux's serial port, SIO1),
then starts the game at its own entry point. So the same build runs on a
console and in the emulator: no `-DYARIO_EMU` needed (a game built with it
still works: its own driver replaces this one). The game's code doesn't move,
so Debug Linked uses its usual debug symbols. `linkdriver/` is built with the
Net Yaroze toolchain (`make` in WSL); its `.bin` files come with the
extension.

## Running and debugging on a PlayStation (PS1 DEV CART)

With a PS1USB cartridge and the
[PS1 DEV CART](https://github.com/logi-26/PS1-DEV-CART-Firmware) firmware,
games run and debug on a real console. `ps1transfer`, the PS1 DEV CART tool,
comes with the extension (`ps1devcart.ps1transferPath` chooses another one);
three buttons are in the status bar.

- **$(plug) cartridge** (*PS1 DEV CART: Select Cart...*): lists the
  cartridges connected to the computer by their USB serial number (e.g.
  `A10PVL53`) and keeps the one you choose (`ps1devcart.devcartSerial`), or
  *First one found*. The button shows the cartridge in use.
- **Run on PS1** (*PS1 DEV CART: Run on PS1*): builds, then runs
  `psx.exe` on that cartridge (`ps1transfer -r -e -f psx.exe -c`) in a
  terminal: `printf` appears there, and the game can read and write files of
  the project folder (PCDRV file server). Closing the terminal leaves the game
  running.
- **Debug on PS1** (*PS1 DEV CART: Debug on PS1*, or a launch
  configuration with `"target": "devcart"`):

```json
{ "type": "ps1devcart", "request": "launch", "name": "PS1 DEV CART: Debug on PS1", "target": "devcart" }
```

  The extension builds the project and makes the debug symbols as above, then
  runs `ps1transfer -gdb`, which loads `psx.exe` on the console stopped at its
  entry point and serves GDB on `ps1devcart.gdbPort`. The console is reset
  first if a game runs. GDB connects to it like to PCSX-Redux.

  - Any number of breakpoints, stepping, call stack, registers, memory.
    Breakpoints also work in code run by interrupts (e.g. VSync callbacks).
  - One hardware breakpoint and one data watchpoint (*Break When Value
    Changes*), using the console's COP0 debug registers.
  - **Pause** stops the game at its next interrupt (not while interrupts are
    disabled). The extension then runs on to your code as described above.
  - `printf` appears in the Debug Console and in the PS1 DEV CART output.
  - Stopping the debugger resets the console to the PS1 DEV CART menu.

`ps1devcart.devcartTty` chooses where the game's TTY goes: `usb` (to the
computer, the default), `sio` (the serial port driver of the Net Yaroze boot
disc: link play over a link cable), `link` (the same driver over the USB link:
link play without a cable, with `ps1transfer -relay` or the Link test page of
the PS1 Dev Cart Manager) or `bios`. The earlier names (`"target": "ps1usb"`,
`ps1devcart.ps1usbSerial`, `ps1devcart.ps1usbTty`) still work.

## Settings

| Setting | Meaning |
|---|---|
| `ps1devcart.emulatorPath` | The emulator of every kind of project (**Browse...** in the panel), e.g. `C:\pcsx-redux\pcsx-redux.exe`; Net Yaroze makefiles get it as `PCSX_REDUX`. Empty: PCSX-Redux installed from Tools (or PSX.Dev's). |
| `ps1devcart.wslDistribution` | WSL distribution with the build environment. Empty: the default one. |
| `ps1devcart.toolchainPath` | Where the toolchain is installed in WSL (default `/opt/yaroze`). |
| `ps1devcart.gdbPath` | GDB in WSL used for debugging (default `/usr/bin/gdb-multiarch`). |
| `ps1devcart.gdbPort` | Port of the GDB server (PCSX-Redux or ps1transfer) while debugging (default 3333). |
| `ps1devcart.ps1transferPath` | `ps1transfer.exe` of the PS1 DEV CART. Empty: the one that comes with the extension. |
| `ps1devcart.devcartSerial` | USB serial number of the PS1 DEV CART to use, set by the status bar's cartridge button (`ps1transfer -l` lists them). Empty: the first one found. |
| `ps1devcart.devcartTty` | TTY of the game on the PS1 DEV CART: `usb` (printf on this computer, default), `sio` (serial port driver, link cable), `link` (USB link, no cable) or `bios`. |
| `ps1devcart.devcartSerial2` | The second PS1 DEV CART of link play (**Run Linked on Two PS1s**, **Debug Linked on Two PS1s**), chosen in the panel. |
| `ps1devcart.devcartLink` | How the two consoles of link play are linked: `cable` (default) or `usb` (USB link relay, no cable; when debugging, the relay runs in the debug session). |
| `ps1devcart.mipsToolchainPath` | MIPS toolchain of PsyQ / nugget projects (`make`, GCC, `gdb-multiarch`). Empty: PSX.Dev's (`%APPDATA%\mips\mips\bin`). |
| `ps1devcart.psn00bsdkPath` | PSn00bSDK folder (release package) for PSn00bSDK projects. Empty: from `PSN00BSDK_LIBS`. |
| `ps1devcart.cmakePath` | `cmake` for PSn00bSDK projects. Empty: the one installed from Tools, `C:\Program Files\CMake\bin\cmake.exe`, else the PATH. |
| `ps1devcart.ninjaPath` | `ninja` for PSn00bSDK projects. Empty: the one installed from Tools, else the PATH. |
| `ps1devcart.offerTools` | Offer to install the tools a workspace's projects need when it opens (default on). |
| `ps1devcart.windowsGdbPath` | GDB on Windows for PsyQ / PSn00bSDK programs. Empty: the toolchain's `gdb-multiarch`, else WSL's. |
| `ps1devcart.buildConfiguration` | `Debug` (default) or `Release` build of PsyQ / PSn00bSDK projects. |
| `ps1devcart.pauseInProgramCode` | After **Pause** stops outside your code, run on to your program's next function and stop there (default on). |
| `ps1devcart.showStatusBar` | Show the Build, Run, Debug and Clean buttons (and the PS1 DEV CART ones). |
| `ps1devcart.offerIntelliSense` | Offer to configure IntelliSense for new projects. |

## Using the tasks in tasks.json

The tasks have the type `ps1devcart`, e.g. to make **Run** a task you can bind
to a key:

```json
{
    "version": "2.0.0",
    "tasks": [
        { "type": "ps1devcart", "target": "run", "label": "Run game" }
    ]
}
```

`target` is one of `build`, `clean`, `rebuild`, `run`, `link`, `models`,
`iso`, `runIso` (`make run-iso`). For a
project in a subfolder add `"project": "example"` (the folder, relative to
the workspace folder). For your own tasks, the problem matcher is
`$ps1devcart-gcc`.

## Notes

- GCC 2.8.1 reports no column numbers, so errors point at the start of the
  line.
- **Run** keeps running until the emulator is closed. Before starting, it
  checks that the emulator exists (the `ps1devcart.emulatorPath` setting, or
  the one the makefile's `run` target starts); if not, it offers to choose it
  and saves it in the setting. *PS1 DEV CART: Choose Emulator...* does the same
  at any time.
- Opening one of the SDK's headers (e.g. *Go to Definition* on a libps
  function) opens it from `\\wsl.localhost\...`; VS Code may ask whether to
  allow that host.

## Building the extension

In the `vscode-extension` folder of the repo (needs Node.js):

    npm install
    npm run package     # makes ps1-dev-cart-<version>.vsix
    npm test            # runs the tests in a separate VS Code (needs the build environment)

Packaging copies the New Project template and the tools from the
[Net Yaroze WSL build environment](https://github.com/logi-26/Net-Yaroze-WSL-Build-Environment)
repo: the folder above the extension when it is in that repo, otherwise
`Net-Yaroze-WSL-Build-Environment` next to the repo holding it (e.g. next to
the PS1 DEV CART firmware repo), or the folder given with `PS1DEVCART_REPO`.

To include the debugging test (it starts PCSX-Redux), set
`PS1DEVCART_TEST_EMULATOR` to the path of `pcsx-redux.exe` first. On a PS1 DEV
CART: `PS1DEVCART_TEST_DEVCART` (its USB serial number) and
`PS1DEVCART_TEST_PS1TRANSFER`; with a second cartridge
(`PS1DEVCART_TEST_DEVCART2`), link play on two consoles is tested too (Run
Linked on Two PS1s, Reset, Debug Linked on Two PS1s). `PS1DEVCART_TEST_DEBUG`
(`emulator`, `devcart`, `linked`, `linkedcarts`) runs only that debugging
test. PsyQ / PSn00bSDK projects: `PS1DEVCART_TEST_PSYQ` (a PSX.Dev PsyQ
project, e.g. the cube template) and `PS1DEVCART_TEST_PSN00BSDK` (the PSn00bSDK
folder: its template is used); `PS1DEVCART_TEST_ONLY=sdk` runs only them.

Install the `.vsix` with *Extensions > ... > Install from VSIX...*, or
`code --install-extension ps1-dev-cart-2.1.0.vsix`.

## Licence

GPL-3.0, like the rest of the repo.
