# Changelog

## 2.6.1

- The link cable tester that comes with the extension is the latest one: baud rates up to 2073600 (L1/R1), and a
  read cancelled by a receive error (e.g. bytes at the old baud rate while the other console changes it) is started
  again instead of leaving that direction of the link stopped.

## 2.6.0

- **Named PS1 DEV CART everywhere**: the settings, commands, views, `launch.json` debug type, task type and problem
  matcher are named `ps1devcart` instead of `netyaroze` (the extension's name when it was only for Net Yaroze), e.g.
  `ps1devcart.emulatorPath`, `ps1devcart.build`, `"type": "ps1devcart"`, `$ps1devcart-gcc`.
  - Settings made with earlier versions (`netyaroze.*`) are still read when the new one isn't set; changes are saved
    with the new names.
  - A workspace whose `launch.json` or `tasks.json` has the earlier names is offered an update when it is opened.
  - Keybindings to the earlier command names need the new ones.
  - The settings `netyaroze.ps1usbSerial` and `netyaroze.ps1usbTty` (replaced by the devcart ones in 2.0) are no
    longer listed, but still read.
- The bundled `ps1transfer` sends each upload packet in one USB write (uploads about 4% faster).

## 2.5.2

- **Link play of unmodified Net Yaroze games in the emulator**: Run Linked and Debug Linked run a copy of `psx.exe`
  with the link driver added (`yario_emu.c` from yaroze-racer, `linkdriver/`): a loader copies it to kernel RAM
  (`0x8000C400`), installs it as the BIOS `tty` device (the serial port, like the boot disc's), then starts the game.
  The same build runs on a console and in the emulator: no `-DYARIO_EMU` build flag needed.

## 2.5.1

- **USB link relay**: games no longer flicker while it runs. Its ps1transfer sends each exchange in one USB write, so
  the PS1 doesn't wait for the computer with the game stopped (the game's CPU time taken by the relay: from 16% to 4%).
- **Debug link game on carts (two)** over the USB link: the relay no longer stops at once with a USB error ("USB link
  relay stopped"): an exchange crossing a stop of the debugged program (breakpoint, pause) is skipped, and the
  firmware's escaped answers are understood.
- **Panel without cartridges**: Cart, Second cart and Link show *None* when the carts aren't connected (Link: fewer
  than two; the Cable / USB link choice is kept for when they are); Deploy to / Debug on selected cart need a cart
  connected, the link game ones on carts two (the emulator ones always work).

## 2.5.0

- **Templates**: new projects from templates, like the PSX.Dev extension (the
  panel's orange **Templates** button, above Build; *PS1 DEV CART: New Project
  from Template...*): Net Yaroze Hello World; PSX.Dev's Empty (Nugget), Psy-Q
  Cube, Net Yaroze Sprite (Psy-Q), PSYQo Hello World and PSYQo Cube (git
  submodules, the converted Psy-Q SDK); PSn00bSDK's Hello World (CMake).

## 2.4.0

- **Net Yaroze build environment installed by the extension** (Tools, *Net
  Yaroze*): no files of your own needed. The toolchain (GCC 2.8.1, binutils
  2.16.1, PSX3's compiler version) comes with the extension; the SDK is read
  from the Net Yaroze Software Development Disc, downloaded from the Internet
  Archive, and set up as PSX3 has it; yarexe is built in WSL. Installed in WSL
  in `/opt/yaroze` (a terminal asks for the WSL password). Programs build the
  same as with a PSX3 SDK.

## 2.3.0

- **Reset cart (SERIAL)**: a Reset button for each connected cart in the panel,
  usable when it is the Cart or the Second cart: no need to switch carts to
  reset the other console.
- **Second cart** can't be the same cartridge as **Cart**: its list leaves out
  the Cart, and choosing the second cart as the Cart sets the Second cart back
  to None.

## 2.2.0

- **No build when nothing changed**: Deploy and Debug (emulator and carts) build
  only when a source file (C, headers, assembly, makefiles, CMake files, data a
  makefile embeds) changed since the last build made here, or the build type
  changed, or the program is missing; files are saved first. **Build** always
  builds, **Clean** forgets the last build.
- **USB link relay** (Deploy link game to carts, Link: USB link) run as the PS1
  Dev Cart Manager runs it: in the background, its output in the PS1 DEV CART
  output, a status bar item while it runs (a click stops it; also *Stop the USB
  Link Relay*). In a terminal it could stop at once (games waiting for the link
  froze). Reset cart and a new deploy stop it first.
- **Debug link game on carts (two)** available whenever two carts are chosen:
  the game runs on the second cart, the main cart is debugged; over the USB link
  the relay runs in the debug session (`ps1transfer -gdb -relay`, firmware with
  the debug monitor version 12).
- **Reset cart** after a deploy: ps1transfer waits for a PS1 still busy with a
  transfer stopped half way (e.g. the game's console ended by the reset), then
  resets it.

## 2.1.0

- **Tools**, like the PSX.Dev extension: the panel lists the tools the
  projects need, found or missing, and installs them in the extension's
  storage (no administrator rights): PCSX-Redux (with the Visual C++ runtime),
  the MIPS toolchain (GCC, make, gdb-multiarch: PSX.Dev's archives), PSn00bSDK,
  CMake and Ninja, the C/C++ extension; gdb-multiarch in WSL. A checkbox for
  each toolchain (Net Yaroze, PsyQ / PsyQo, PSn00bSDK, emulator, debugger):
  *Install selected* installs only the tools of the ticked ones. *Install
  Tools...* command (asks which toolchains), and an offer when a workspace
  needs some, for its projects' toolchains only (`netyaroze.offerTools`). Tools already installed (settings, PSX.Dev's, the
  PATH) are used as they are.
- **ps1transfer comes with the extension** (with its tools and the FTDI
  library): no setting needed.
- **Emulator** row in the panel with **Browse...**: one emulator for every
  kind of project (Net Yaroze makefiles get it as `PCSX_REDUX`); without a
  setting, the PCSX-Redux installed. A missing emulator offers to install
  PCSX-Redux.
- Panel: icons on the Deploy, Debug and Reset cart buttons, and a hint
  shown when the mouse stays over a button; the Tools in a section that
  opens and closes (closed by default); **Clean** under Build; the
  Reset cart button, the cartridge count and the Cart, Second cart and Link
  selections together in the PS1 DEV CART section; only the detected
  cartridges in the Cart list.
- **Reset** works after *Deploy to selected cart*: like the PS1 Dev Cart
  Manager, it first stops what keeps the cartridge open (the Run on PS1
  console terminal, the USB link relay, a debug session on that cart).
- Setting `netyaroze.ninjaPath`; a PSn00bSDK build uses PSn00bSDK's own GCC
  first.
- PsyQ projects build with GCC 15 and later (the toolchain installed is GCC
  16): their C is compiled as `-std=gnu17` (`CFLAGS`), as C23 rejects the
  PsyQ headers' old-style declarations. A `-std` in the Makefile still wins.
- Tools test: everything installed in a new profile, then a PsyQ and a
  PSn00bSDK project built with them and debugged in the PCSX-Redux installed
  (`NETYAROZE_TEST_ONLY=tools`).

## 2.0.0

- Renamed **PS1 DEV CART** (was *Net Yaroze*): it builds, runs and debugs Net
  Yaroze, PsyQ / PsyQo and PSn00bSDK games, in PCSX-Redux or on a PlayStation
  with a PS1 DEV CART. Extension id `logi-26.ps1-dev-cart` (uninstall the
  earlier `logi-26.net-yaroze`); commands in the *PS1 DEV CART* category with
  shorter names (*Run on PS1*, *Select Cart...*, *Reset PS1*...); the activity
  bar panel, output channel and debugger are named PS1 DEV CART.
- The settings (`netyaroze.*`), command ids, `launch.json` type and task type
  (`netyaroze`) are unchanged: existing settings, launch configurations, tasks
  and keybindings still work.

## 1.5.0

- **PsyQ / PsyQo and PSn00bSDK projects**, besides Net Yaroze ones: a Makefile
  using nugget (the PSX.Dev templates) or making a PS-X EXE, or a
  `CMakeLists.txt` with `psn00bsdk_add_executable`. Built on Windows (`make
  BUILD=Debug` with the PSX.Dev MIPS toolchain; CMake and Ninja with
  PSn00bSDK), compiler errors in the Problems panel, and every command: Run in
  PCSX-Redux, Run on PS1, Debug in the emulator and on the PS1 (with the
  program's ELF and Windows' `gdb-multiarch`: full debug information), the link
  play commands and Reset.
- Settings `netyaroze.mipsToolchainPath`, `netyaroze.psn00bsdkPath`,
  `netyaroze.cmakePath`, `netyaroze.windowsGdbPath`,
  `netyaroze.buildConfiguration`.
- The panel and the tasks show the kind of project.
- Tests of a PsyQ project and the PSn00bSDK template (build, Problems, Run on
  PS1, Debug on PS1 and in PCSX-Redux, Debug Linked on Two PS1s).

## 1.4.0

- **PS1 DEV CART panel** (the Net Yaroze icon in the activity bar): Build,
  the cartridges connected in two drop-down lists (the cart used, and the
  second one of link play), the link (cable or USB link), and a button for
  each action: deploy to the emulator, to the selected cart, the link game to
  two emulators or two carts; debug in the emulator, on the selected cart,
  the link game in two emulators or on two carts; reset the cart; clean.
- **Run Linked** works without a `link` make target: the extension starts the
  two linked emulators itself (`debug/run-linked.ps1`, SIO1 server and
  client).
- **Debug Linked (Two Emulators)**: the two linked emulators with their GDB
  servers, and a debug session for each.
- **Run Linked on Two PS1s**: the game on two PS1 DEV CART cartridges at once,
  over a link cable (serial port TTY driver) or the USB link (with the
  ps1transfer relay in a terminal).
- **Debug Linked on Two PS1s**: `ps1transfer -gdb` on both cartridges and a
  debug session for each (link cable).
- **Reset PS1 (PS1 DEV CART)** and **Select Second PS1 DEV CART...** commands;
  settings `netyaroze.devcartSerial2` and `netyaroze.devcartLink`.
- A cartridge another program has open is no longer listed with "(in" as its
  serial number.
- Packaging finds the build environment repo next to the repo holding the
  extension (or `NETYAROZE_REPO`).
- Tests of link play on two consoles (`NETYAROZE_TEST_DEVCART2`).

## 1.3.0

- **Build CD Image** and **Run CD Image** (*Net Yaroze: Build CD Image*,
  *Net Yaroze: Run CD Image in Emulator*, `iso` and `runIso` tasks): run
  `make iso` and `make run-iso`, which build a bootable CD image of the game
  with mkpsxiso and start it in the emulator. Offered only for projects whose
  makefile has both targets; the emulator setting is passed as `PCSX_REDUX`,
  as for Run.
- New projects include `iso.xml` and `system.cnf` (the CD image's layout)
  and the makefile's `iso` and `run-iso` targets.
- **PS1 DEV CART**: running and debugging on a PlayStation with a PS1USB
  cartridge and the PS1 DEV CART firmware, through `ps1transfer`:
  - **Run on PS1** (*Net Yaroze: Run on PS1 (PS1 DEV CART)*): builds and runs
    `psx.exe` on the console, with printf and the PCDRV file server in a
    terminal.
  - **Debug on PS1** (*Net Yaroze: Debug on PS1 (PS1 DEV CART)*, launch
    configurations with `"target": "devcart"`): `ps1transfer -gdb`.
    Breakpoints, stepping, a hardware breakpoint and a watchpoint, pause and
    printf. When debugging stops, ps1transfer is given time to end by itself
    (it removes the breakpoints on the console) before it is stopped.
  - A status bar button chooses the cartridge among those connected (by USB
    serial number); the Run on PS1 and Debug on PS1 buttons appear once
    `netyaroze.ps1transferPath` is set.
  - Settings `netyaroze.ps1transferPath`, `netyaroze.devcartSerial` and
    `netyaroze.devcartTty` (`usb`, `sio`, `link` for the USB link, `bios`).
- Tests of Run and Debug on PS1 on a real console (`NETYAROZE_TEST_DEVCART`,
  `NETYAROZE_TEST_PS1TRANSFER`).

## 1.2.0

- **Run Linked** (*Net Yaroze: Run Linked (Two Emulators)*, editor and
  status bar buttons, `link` task): runs `make link`, for link play in two
  emulators linked through their serial ports. Shown only for projects whose
  makefile has a `link` target; the emulator setting is passed to it as
  `PCSX_REDUX`, as for Run.

## 1.1.2

- Pause while the game runs no longer leaves the debugger in the
  interrupt handler or libps/BIOS code (no source; stepping failed with
  "Unable to step next ... 0x80004004"): it runs on to the next function of
  your program (or line of `main`) and stops there, so Step Over works.
  Setting `netyaroze.pauseInProgramCode` (on by default).
- `yaroze_debuginfo.py --locations FILE` writes the program's functions and
  `main`'s line addresses as JSON (used for the above).

## 1.1.1

- Configure IntelliSense also turns off the C/C++ extension's "Run/Debug
  C/C++ File" button (▷ on C files) for the workspace: it builds Windows
  programs and is easily mistaken for Net Yaroze's Run and Debug.
- "Net Yaroze: Debug in PCSX-Redux" is listed in Run and Debug without a
  launch.json.
- New projects (and the repo's example) include `.vscode/launch.json`, so F5
  debugs, and `.vscode/settings.json` with the button above turned off.

## 1.1.0

- Debugging in PCSX-Redux: Net Yaroze: Debug in Emulator (editor and
  status bar buttons, F5, `netyaroze` launch configurations). Builds, makes
  debug symbols with `yaroze_debuginfo.py`, starts PCSX-Redux with its GDB
  server, and debugs with gdb-multiarch in WSL through the C/C++ extension:
  source line and function breakpoints, stepping (also over libps calls),
  call stack, registers, memory; `printf` output in the Debug Console.
- `netyaroze.gdbPath` and `netyaroze.gdbPort` settings.

## 1.0.3

- Run checks that the emulator exists (the setting, or the one the
  makefile's run target starts) and, if not, offers to choose it.
- Net Yaroze: Choose Emulator... command.

## 1.0.2

- Projects in subfolders of the opened folder are found (e.g. `example/`
  when the whole repo is open); with several, the one of the file being
  edited is used, otherwise you are asked.
- Ctrl+Shift+B runs Net Yaroze: Build when a project is open.
- Build and Run buttons at the top right of the editor.
- The status bar buttons have their own ids, so they all show.
- Compiler errors in subfolder projects link to the right file.

## 1.0.0

- Build, Clean, Rebuild, Run and Build Models commands, tasks and status bar
  buttons (the makefile run in WSL).
- `$netyaroze-gcc` problem matcher for GCC 2.8.1 errors and warnings.
- Configure IntelliSense for libps (Microsoft C/C++ extension).
- New Project from a template.
- Convert PNG to TIM from the Explorer.
