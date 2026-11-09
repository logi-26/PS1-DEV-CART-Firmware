# Sound tests

Tests of the sound tester's SEQ / VAB player and the Sound Tools' converters. Run them before a release.

## Without a console

The console's own player code (`ps1-tools/soundtest/soundtest.c`, everything before `main`) is compiled for the PC
by `build_host.py`, with the SPU replaced by memory (`seqhost.c`). It is built again when `soundtest.c` changes, with
the 32-bit MinGW of `buildtools`. What it plays is compared with `seqref.py`, a reference player written from Sony's
SEQ format description.

```
python -m unittest tests/sound/test_seq_player.py tests/sound/test_converters.py -v
```

- `test_seq_player.py`
  - SEQs written for the tests (each played at NTSC and PAL frame times):
    - running status, tempo changes, the header variants
    - counted, endless and empty loops
    - a SEQ with no notes, a file cut short, SEPs
    - seeks, the effects, many voices at once
  - The real files of `PS1_SOUND_CORPUS`, each played for up to 3 minutes:
    - every note at its time
    - the length and the channels with notes
    - each voice's pitch, volume and layered tones against the bank
    - the effects, seeks to several places
  - Damaged copies of those files (cut short, bytes changed): no hang, no read past the data.
- `test_converters.py`
  - MIDI → SEQ: notes and tempo map; loops from markers, CC 111, the whole music or a tick, played again as written
  - SEQ → MIDI → SEQ round trip
  - WAV → VAG → WAV: quality, loop points
  - VAB building: each tone at its sample's pitch, the bend range
  - Music tracker songs: the song's loop, MIDI export
  - XA written and read back, bank exports

The real files aren't in the repository (games' and SDKs' music). List their folders in `PS1_SOUND_CORPUS`,
separated by `;`. A SEQ's bank is the VH + VB (or VAB) of the same name in its folder, else the folder's only one.

```
set PS1_SOUND_CORPUS=C:\ps1usb\Alundra;C:\ps1usb\Raw;C:\ps1usb\test-audio
set SOUND_FUZZ=200                  (damaged copies of each file, default 40)
```

Without `PS1_SOUND_CORPUS`, only the tests' own SEQs, MIDIs and sounds are used.

## With a console

```
python tests/sound/live_sound_tools.py --corpus C:\ps1usb
```

Drives the Manager's Sound Tools page with the console connected. Every scenario runs to its end, then a summary of
the checks:
- start, time at real speed, seek, pause and resume
- tempo, transpose, mono, mutes, reverb (and no room for it)
- the VH read back, stop
- the music's end (played again or stopped), an endless loop
- a SEP's SEQs, a VAG alone, a VAB file alone
- no USB error in the log
