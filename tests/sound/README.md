# Sound tests

Tests of the sound tester's SEQ / VAB player and the Sound Tools' converters. Run them before a release.

## Without a console

The console's own player code (`ps1-tools/soundtest/soundtest.c`, everything before `main`) is compiled for the PC
by `build_host.py`, with the SPU replaced by memory (`seqhost.c`). It is built again when `soundtest.c` changes, with
the 32-bit MinGW of `buildtools`. What it plays is compared with `seqref.py`, a reference player written from Sony's
SEQ format description.

```
python -m unittest tests/sound/test_seq_player.py tests/sound/test_converters.py tests/sound/test_reverb.py tests/sound/test_sshd.py tests/sound/test_aad.py tests/sound/test_akao.py -v
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
- `test_reverb.py`
  - chaos echo's and delay's registers for each delay time and feedback, against libspu's SpuSetReverbModeParam
  - the voices sending to the reverb with "SEQ channels chosen"
- `test_sshd.py`
  - SQ → SEQ (Sony's later music format: Ape Escape, Alundra 2...): events, running status, the 1-byte pitch bend,
    tempo, the loop, played by the console's player
  - HD + BD → VAB: key splits, layers, drum kits, the tuning, the samples cut out of the BD
  - finding the musics and their banks inside a game's files or disc image (`gui/sshd.py scan`); The Legend of
    Dragoon's MRG archives: the bank in the music's own archive, a BD split over the archive's files, a music played
    with a bank loaded before it; Arc the Lad III's BD before its HD (BD, SQ, HD), the BD nearest the HD on either
    side
  - a program of more than 16 tones of one key each (Arc the Lad III): the first 16 kept, the rest reported
  - the real files of `PS1_SSHD_CORPUS` (folders of `name.sq` + `name.hd` + `name.bd`), each converted
- `test_aad.py`
  - Crystal Dynamics' SND bank + SMP samples → VAB (Gex 2's, Gex 3's, Akuji's and Soul Reaver's layouts), their
    musics → SEQ: the loop (labels, loop commands), tracks looping apart (Soul Reaver: the SEQ's loop where they
    start again together), a channel's transpose, running status, a note starting bent, played by the console's
    player
  - Gex's BIGFILE.DAT compression, archives found in ISO 9660 images (2048 and 2352-byte sectors), the samples paired
    with their bank, disc images read in pieces (`gui/discimage.py`)
  - the disc images or BIGFILE.DATs of `PS1_AAD_CORPUS` (folders): every bank converted, every music played (its
    notes in the first 2 seconds counted within a frame: the player starts notes at frames)
- `test_akao.py`
  - Square's AKAO (Final Fantasy VII, VIII, Vagrant Story...): sequences (versions 1.0 and 3.x) read and played as the
    driver plays them, sample sets, FF7's INSTR.ALL / INSTR.DAT, LZS, the SEQ + VAB made for the sound tester
  - the disc images of `PS1_AKAO_CORPUS` (folders, `.cue` files searched in them): every music converted, each of its
    notes with a tone
- `test_converters.py`
  - MIDI → SEQ: notes and tempo map; loops from markers, CC 111, the whole music or a tick, played again as written
  - SEQ → MIDI → SEQ round trip
  - WAV → VAG → WAV: quality, loop points
  - VAB building: each tone at its sample's pitch, the bend range
  - Music tracker songs: the song's loop, MIDI export
  - XA written and read back, bank exports

The real files aren't in the repository (games' and SDKs' music). List their folders in `PS1_SOUND_CORPUS`,
separated by `;`. A SEQ's bank is the VH + VB (or VAB) of the same name in its folder, else the folder's only one.
The game discs go in `PS1_AAD_CORPUS` (folders of the `.bin` images) and `PS1_AKAO_CORPUS` (folders searched for
`.cue` files); `PS1_SSHD_CORPUS` takes folders of extracted `.sq` + `.hd` + `.bd` files. For example, with the files
in a `ps1usb` folder on the desktop:

```
set PS1_SOUND_CORPUS=%USERPROFILE%\Desktop\ps1usb\Alundra;%USERPROFILE%\Desktop\ps1usb\Raw;%USERPROFILE%\Desktop\ps1usb\test-audio
set PS1_AAD_CORPUS=%USERPROFILE%\Desktop\ps1usb\PSXSOUND\Gex - Enter the Gecko (USA);%USERPROFILE%\Desktop\ps1usb\PSXSOUND\Gex 3 - Deep Cover Gecko (USA);%USERPROFILE%\Desktop\ps1usb\PSXSOUND\Legacy of Kain - Soul Reaver (USA)
set PS1_AKAO_CORPUS=%USERPROFILE%\Desktop\ps1usb\PSXSOUND\AKAO
set SOUND_FUZZ=200                  (damaged copies of each file, default 40)
```

The disc images are read in pieces (`gui/discimage.py`), never whole: about 300 MB of memory at most (AKAO).
Without these, only the tests' own files are used (the corpus tests are skipped).

## With a console

```
python tests/sound/live_sound_tools.py --corpus C:\ps1usb
```

Drives the Manager's Sound Tools page with the console connected. Every scenario runs to its end, then a summary of
the checks:
- start, time at real speed, seek, pause and resume
- tempo, transpose, mono, mutes, reverb (and no room for it)
- chaos echo's delay time and feedback, reverb on chosen channels (the SPU registers read back)
- the VH read back, stop
- the music's end (played again or stopped), an endless loop
- a SEP's SEQs, a VAG alone, a VAB file alone
- no USB error in the log
