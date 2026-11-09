# Tool tests

Tests of the Manager's other tools: what each one reads, writes and sends to the console, checked against references
written in the tests from the formats' descriptions (not from the code tested). Run them before a release.

Console code (the kernel hook's cheat engine, the firmware's cheat screen, the synth) and ps1transfer code (the frame
capture's packet walk, the disc reader's file system) is compiled for the PC from its own source (`cbuild.py`,
functions taken by name), with the 32-bit MinGW of `buildtools`.

## Without a console

```
python -m unittest discover -s tests/tools -p "test_*.py" -v
```

| File | What |
|---|---|
| `test_memcard.py` | Memory card images (ps1transfer `-mc*`):<br>• format, list (titles, icons), export / import / copy / delete<br>• blocks reused scattered, a full card, duplicate names<br>• DexDrive `.gme` (header kept), damaged images |
| `test_cheats.py` | Cheats:<br>• the console's code engine against a reference (every code type, 2000 random code lists)<br>• the firmware's cheat screen reading the cheats the Manager puts in the flash<br>• the table, the POPStarter / Xploder / GameShark converters, serial codes past the RAM |
| `test_symbols.py` | Symbol files:<br>• ELF against the toolchain's nm<br>• nm output and map files, Net Yaroze executables, damaged files |
| `test_gpu.py` | Frame Inspector:<br>• every GP0 command decoded<br>• the capture's ordering table walk (draw area and offset, loops, bad links) |
| `test_assets.py` | VRAM Viewer and Asset Extractor:<br>• TIM files of each depth (two readers against a reference, real TIMs)<br>• the scanner on data with planted files (TIM, VAB, VH + SPU samples, SEQ, SEP, VAG, TMD)<br>• the textures a game draws |
| `test_spu.py` | SPU RAM Viewer:<br>• the map and its warnings (samples, free memory, data, reverb area, voices)<br>• decoding, exported samples |
| `test_synth.py` | Synth:<br>• the console's slot memory (1000 random loads and clears against a reference)<br>• voices (sustain, stealing), pitch<br>• SysEx samples (7-bit packing, checksum, `.syx`) |
| `test_disc.py` | Asset Extractor, disc: the ISO 9660 file system read from a disc image made here (XA files, nested and large directories), files copied, damaged images |
| `test_misc.py` | • Run pages (program kinds, Net Yaroze auto scripts)<br>• the firmware ROM checks and its cheats area<br>• multitap replies through the console's code engine<br>• damaged settings files |

Real files are used when given:
- `PS1_TIM_DIR`: a folder of TIM files;
- `PS1_NETYAROZE`: the Net Yaroze SDK's `PSX\BIN`.

## With a console

```
python tests/tools/live_tools.py [--cartridge BILL]
```

Each tool run as its page runs it, nothing written to the flash or a memory card:
- firmware information
- a program run with its printf and its file server
- peek / poke by symbol, the live link, a RAM dump searched, the code engine
- a screenshot
- the SPU RAM map and a snapshot of the sound tester
- a TIM hot reloaded and read back from VRAM
- a frame captured, the profiler
- GDB's protocol
- the model viewer
- the synth
- the remote pad in the program's buffer
- the memory card listed

## Known

- **Profiler and Frame Inspector on a program that keeps making file server requests** (e.g. the movie tester reading
  its `@input` every few frames): the PC side now serves those requests while it waits for an answer, tells a file
  close from the profiler's conflict packet, keeps up with the samples and returns from its reads in time, but the
  console still leaves the profiler's or the capture's command unanswered (its debug monitor, busy with the program's
  requests): a firmware fix. Programs that don't use the file server, or attached after it, work. The live test uses
  the control tester for both.
- **Firmware cheat screen:** a damaged cheats area in the flash can make it list cheats it didn't read (marked as an
  expected failure in `test_cheats.py`); the Manager always writes it whole.
- **Code engine (kernel hook):** a serial code running past the 2 MB of RAM writes on. The Manager never sends one
  (`codeengine.safe_codes`).
