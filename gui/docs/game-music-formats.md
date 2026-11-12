# PS1 game music formats: Crystal Dynamics' AAD, Sony's SQ/HD/BD and Square's AKAO

A reference for three music formats used by commercial PlayStation games, written while making the PS1 DEV CART's
converters (`gui/aad.py`, `gui/sshd.py`, `gui/akao.py`) that turn them into Sony's SEQ + VAB for the sound tester.

Everything here was read from the games' own files and, for the Crystal Dynamics formats, checked against the
decompiled sound library of *Legacy of Kain: Soul Reaver* (github.com/fmil95/soul-re and Gh0stBlade/KAIN2:
`AADLIB.C`, `AADSEQEV.C`, `AADSQCMD.C`, `AADVOICE.C`). Each conversion was played by the console's SEQ player (built
for the PC) and compared note by note with a reference player. Statements marked *(inferred)* are deduced from the
data but not confirmed by code or by ear.

Games examined:

| Format | Games | Discs |
|---|---|---|
| Crystal Dynamics AAD | Gex: Enter the Gecko, Gex 3: Deep Cover Gecko (with an Akuji demo), Legacy of Kain: Soul Reaver | USA |
| Sony SQ/HD/BD | Ape Escape, Alundra 2, The Legend of Dragoon (4 discs), Arc the Lad III (disc 1) | USA / Europe |
| Square AKAO | Final Fantasy VII, Final Fantasy VIII (disc 1 each), Vagrant Story | USA |

All values are little-endian unless stated. Offsets are hexadecimal.

---

## Part 1: Crystal Dynamics' AAD sound library

Crystal Dynamics' games use their own sound library ("AAD"). A **bank** (SND) holds the instruments and the
music; its **samples** (SMP) are loaded into SPU RAM. The four-character IDs are stored as 32-bit numbers, so they
read backwards in a file:

| File | ID in the library | Bytes in the file |
|---|---|---|
| Bank | `aSND` | `DNSa` |
| Samples | `aSMP` | `PMSa` |
| Sequence (one track) | `aSEQ` | `QESa` |
| Sequence (several tracks) | `aMSQ` | `QSMa` |

psx-spx also lists `FNSa` and `FMSa` as unknown. They are the library's sound effect sets (`aSNF` and `aSMF`): see
1.5. Soul Reaver has 885 of them.

### 1.1 The bank (SND, "DNSa")

There are three header layouts. Their tables after the header are the same.

**Soul Reaver** (bank version 262, every field 32-bit; the library's `AadSoundBankHdr`):

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `DNSa` |
| 04 | 4 | Header size (28h): the tables start here |
| 08 | 4 | Bank version (106h = 262) |
| 0C | 4 | Number of programs |
| 10 | 4 | Number of tones |
| 14 | 4 | Number of waves |
| 18 | 4 | Number of sequences |
| 1C | 4 | Number of labels |
| 20 | 4 | Reverb mode |
| 24 | 4 | Reverb depth |

**Gex 3** (the same fields packed):

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `DNSa` |
| 04 | 4 | Header size (80h) |
| 08 | 2 | Bank version (106h) |
| 0A | 1 | Not read by the game (always 3) |
| 0B | 1 | Number of programs |
| 0C | 2 | Number of tones |
| 0E | 2 | Number of waves |
| 10 | 1 | Number of sequences (the game reads one byte) |
| 11 | 1 | Not read |
| 12 | 2 | Number of labels |
| 14 | 1 | Reverb type (byte; 15h not read) |
| 16 | 2 | Reverb depth (signed 16-bit) |
| 18 | 68h | Not read by the game (mostly zero) |

**Gex 2 and Akuji** (older):

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `DNSa` |
| 04 | 2 | Bank version: 43h (Gex 2), 45h (Akuji); the game refuses any other |
| 06 | 1 | Not read by the game |
| 07 | 1 | Number of programs |
| 08 | 2 | Number of tones |
| 0A | 2 | Number of waves |
| 0C | 1 | Number of sequences (one byte; 0D not read) |
| 0E | 2 | Number of labels |
| 10 | 2 | Reverb type |
| 12 | 2 | Reverb depth (e.g. 6000) |

The tables start at a place the code fixes by version: 44h (version 43h), 48h (45h).

How to tell them apart: a 32-bit 106h at offset 08 is Soul Reaver's layout; a 16-bit 106h at 08 (with a non-zero
byte at 0A) is Gex 3's; anything else is the older layout. In every layout the program table can be confirmed: each
program's first tone is the previous program's first tone plus its count. (Fields marked "not read" were checked in
Gex 2's, Gex 3's and Akuji's bank-opening code.)

**Reverb.** When a bank is opened the library calls libspu's `SpuSetReverbModeType(type)`, turns reverb off on all
voices, clears the work area, then `SpuSetReverbModeDepth(depth, depth)` and `SpuSetReverb(on)` (Soul Reaver's
`aadInitReverb`; the same calls in Gex 3's code). The type is libspu's: 0 off, 1 room, 2 studio (small), 3 studio
(medium), 4 studio (large), 5 hall, 6 space, 7 echo, 8 delay, 9 pipe. The depth is libspu's 0-7FFFh (10000 is
about 31%). The banks examined use types 1-5.

**Tables**, one after the other from the header size:

| Table | Entry size | Count |
|---|---|---|
| Programs | 8 | programs |
| Tones | 16 | tones |
| Wave addresses | 4 | waves |
| Sequence offsets (from the end of the label table) | 4 | sequences |
| Label offsets (from the start of a track) | 4 | labels |
| Sequences | | |

**Program** (8 bytes, `AadProgramAtr`):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Number of tones |
| 2 | 2 | First tone (index in the tone table) |
| 4 | 1 | Volume (0-127) |
| 5 | 1 | Pan (0-127, 64 centre) |
| 6 | 2 | Unused |

Some of Gex 3's programs count more tones than the bank holds; the library reads past the table. Those tones are
ignored by the converter.

**Tone** (16 bytes, `AadToneAtr`):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Priority |
| 1 | 1 | Program it belongs to |
| 2 | 1 | Volume (0-127) |
| 3 | 1 | Pan (0-127) |
| 4 | 1 | Centre note: the note at which the wave plays at 44100 Hz |
| 5 | 1 | Fine tune, signed (see below) |
| 6 | 1 | Lowest note |
| 7 | 1 | Highest note |
| 8 | 1 | Mode: 4 = reverb on |
| 9 | 1 | Pitch bend range in semitones for a full bend (0 = no bend) |
| A | 2 | SPU ADSR1 |
| C | 2 | SPU ADSR2 |
| E | 2 | Wave (index in the wave table) |

A note plays every tone of its program whose range includes it (layers and key splits work as in a VAB).

Pitch: the library looks up `aadPitchTable[note + 60 - centre]` (a semitone table, 1000h = 44100 Hz at index 60) and
adds `aadStepsPerCent[index] * fine * 100 >> 23` (a negative fine tune, bit 7 set, subtracts with `0x100 - fine`).
That makes one unit of fine tune about **0.76 cents** (0.74-0.79 depending on the note), so 127 is just under a
semitone. Both tables and this code are identical in Gex 2, Gex 3, Akuji and Soul Reaver. The tables (85 entries,
16-bit then 32-bit) start with 128, 135, 143... and 4587, 5242, 5898...

Volume: the library squares each volume before applying it (`(v + 1)² - 1`, then `>> 14`): note velocity, channel
volume, tone volume, program volume and the slot's volume are all on that curve. A VAB scales linearly, so balances
can differ slightly *(not compared by ear)*.

**Wave addresses** (32-bit each): where each wave starts in the samples.

| Game | Meaning |
|---|---|
| Gex 3, Soul Reaver | Offset in the SMP's data (the first wave: 0) |
| Akuji | SPU RAM address (the first wave: 1010h) |
| Gex 2 | SPU RAM address (the first wave: 3010h) |

Waves usually start just after a block with the ADPCM end flag, but not always: a tone can start inside another
wave's sample (one of Gex 2's does).

### 1.2 The samples (SMP)

**"PMSa"** (Gex 3, Soul Reaver, Akuji):

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `PMSa` |
| 04 | 4 | Size of the data after these 8 bytes |
| 08 | ... | SPU ADPCM data, loaded as it is into SPU RAM (its first 16-byte block is zero) |

The wave offsets count from offset 08 (not 10h, as psx-spx states).

Gex 2 and Gex 3 load the samples at SPU RAM address 3010h (Gex 3 adds 3010h to its wave offsets when they are
loaded).

**Gex 2** has no ID:

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | Size of the data after these 4 bytes |
| 04 | ... | SPU ADPCM data (its first 16-byte block is zero) |

psx-spx calls this a "crippled VAB". It sits right after its bank in Gex 2's BIGFILE.DAT.

A bank's samples are not referenced from the bank. On every disc examined, the right SMP is the one where every
wave lands on a sample start: in Gex 2 and Gex 3 it's usually the next file (in Gex 3 not always: another SND may
come in between).

### 1.3 Sequences

**One track ("QESa": Gex 2, Gex 3, Akuji):**

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `QESa` |
| 04 | 4 | Microseconds per quarter note |
| 08 | 2 | Ticks per quarter note (480) |
| 0A | 2 | Version: 106h (Gex 3), 45h (Akuji), 43h (Gex 2) |
| 0C | ... | The track's events |

**Several tracks ("QSMa": Soul Reaver; the library's `AadSequenceHdr`):**

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `QSMa` |
| 04 | 4 | Microseconds per quarter note |
| 08 | 2 | Ticks per quarter note |
| 0A | 2 | Version (106h) |
| 0C | 2 | Number of tracks (1-16) |
| 0E | 2 | Unused |
| 10 | 4 × n | Each track's offset from the sequence's start |

**Events.** Each event is a delta time (MIDI's variable length) **before** the event, then:

- a MIDI channel event, with running status: 9n note on (velocity 0: note off), Bn controller, Cn program change,
  En pitch bend (2 data bytes, 2000h centre). 8n note off exists but the library ignores it (no game examined uses
  it). Of the controllers only 7 (volume) and 10 (pan) do anything; 0 and 32 (bank select) are in the files but
  ignored. Volume changes reach notes already sounding only after an "enable sustain update" command; otherwise
  only new notes.
- `FF`, a command number, the number of data bytes, the data bytes: one of the library's 78 commands (below). After a
  command the running status is kept.

A note starts at its channel's current pitch bend (Sony's libsnd starts new notes unbent). A channel plays nothing
until its first program change (its program starts at 255).

**Tracks play independently.** Each track has its own position, running status and loops. In Soul Reaver, the
tracks of one piece loop over different lengths (e.g. 7680, 11520, 15360 and 92160 ticks) and only line up again
after their least common multiple; some differ by one tick (92161 against 92160), which the game lets drift. An
`EndSequence` in any track resets the whole sequence; several Soul Reaver pieces have a track that does nothing but
wait 1,920,000 ticks before it.

**Banks hold two kinds of sequence**: a short control script, usually sequence 0, which sets the tempo, picks a
"slot", assigns a music sequence to it, starts it and sets the game's variables; and the music.

### 1.4 Sequence commands

Data bytes: `b0`, `b1`, `b2`. "Selected" means the channel or slot chosen by commands 0 and 1. *(No effect)* means
the decompiled function is empty.

| # | Command | Data |
|---|---|---|
| 0 | Select channel | b0: channel (0-15) |
| 1 | Select slot | b0: slot |
| 2 | Assign sequence (to the selected slot) | b0: sequence number |
| 3 | Use primary tempo | |
| 4 | Use secondary tempo | |
| 5 | Set tempo | b0-b2: microseconds per quarter note, **big-endian** |
| 6 | Set tempo from a sequence (its header's) | b0: sequence number |
| 7 | Change tempo | b0: percent (the quarter note's time × 100 / b0) |
| 8 | Start slot | |
| 9 | Stop slot | |
| 10 | Pause slot | |
| 11 | Resume slot | |
| 12 | Set slot bend range | *(no effect)* |
| 13 | Set channel bend range | *(no effect)* |
| 14 | Set slot volume | b0 |
| 15 | Set slot pan | b0 |
| 16 | Set channel volume | b0 |
| 17 | Set channel pan | b0 |
| 18 | Mute channel | |
| 19 | Unmute channel | |
| 20 | Mute channel list | b0-b1: channel mask |
| 21 | Unmute channel list | b0-b1: channel mask |
| 22 | Channel volume fade | *(no effect)* |
| 23 | Channel pan fade | *(no effect)* |
| 24 | Slot volume fade | *(no effect)* |
| 25 | Slot pan fade | *(no effect)* |
| 26 | Set channel program | b0: program |
| 27 | Set channel base priority | *(no effect)* |
| 28 | Set channel transpose | b0: semitones, a byte (F4h = -12); a note's pitch moves, but its tone is still chosen by the untransposed note |
| 29 | Ignore channel transpose | |
| 30 | Respect channel transpose | |
| 31-33 | Set / ignore / respect channel pitch map | *(no effect)* |
| 34 | Get sequence assigned | b0: variable |
| 35 | Get tempo | b0-b2: variables |
| 36 | Get slot status | b0: variable |
| 37-45 | Get slot volume / pan, channel volume / pan / mute / bend range / transpose / program / base priority | b0: variable (some empty) |
| 46 | Loop start | b0: count (0 = endless); at most 4 loops inside each other |
| 47 | Loop end | back to the loop start while the count lasts |
| 48 | Loop break | *(no effect)* |
| 49 | Define label | *(no effect: a label is placed by the bank's label table)* |
| 50 | Go to label | b0: label number |
| 51 | Set variable | b0: value, b1: variable (0-127) |
| 52 | Copy variable | b0: from, b1: to |
| 53 | Add to variable | b0: value, b1: variable |
| 54 | Subtract from variable | b0: value, b1: variable |
| 55 | Set variable bits | b0: bits, b1: variable |
| 56 | Clear variable bits | b0: bits, b1: variable |
| 57-62 | Branch if variable =, ≠, <, >, ≤, ≥ | b0: variable, b1: value, b2: label |
| 63 | Branch if variable bits set | b0: variable, b1: mask, b2: label |
| 64 | Branch if variable bits clear | |
| 65-67 | Substitute a variable for data byte 1 / 2 / 3 of the next command | |
| 68 | End sequence (resets the slot) | |
| 69-71 | Play / stop sound effect, set its volume and pan | |
| 72 | Set sequence position | b0: label |
| 73 | Enable sustain update | |
| 74 | Disable sustain update | |
| 75 | Set channel mute | b0-b1: mask |
| 76 | Delay mute | b0-b1: mask |
| 77 | Update mute | |

**Labels.** Label n's position is `track start + label table[n]`, and it points at the **delta time** of the event
after its `Define label` (the label table is shared by all the bank's sequences). A loop made by `Go to label` back
to a label is the usual way music loops; `Loop start 0` / `Loop end` is the other. A `Go to label` can also jump
forward (Soul Reaver skips parts of a track that way).

### 1.5 Sound effect sets (SNF "FNSa" + SMF "FMSa")

Soul Reaver's objects and areas load their sound effects as a pair of files (the library's "dynamic sound effects":
`\kain2\sfx\object\NAME\NAME.snf` and `.smf`, or `\kain2\area\NAME\bin\NAME.snf`). On the disc each SMF directly
follows its SNF, both at sector starts.

SNF (`_AadDynSfxFileHdr`, then the effects' IDs):

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `FNSa` |
| 04 | 2 | Version (100h) |
| 06 | 2 | Unique ID (the SMF's must match) |
| 08 | 2 | Filled in by the library when loaded (a handle) |
| 0A | 2 | Number of effects |
| 0C | 8 | Filled in by the library (list pointers) |
| 14 | 2 × n | Each effect's ID (the game plays an effect by this ID) |

SMF:

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | ID `FMSa` |
| 04 | 2 | Version (100h) |
| 06 | 2 | Unique ID (the SNF's) |
| 08 | 2 | Number of effects (the SNF's) |
| 0A | 6 | Unused |
| 10 | ... | Each effect: 24 bytes of attributes, then its wave |

Effect attributes (`AadDynSfxAttr`, 24 bytes):

| Offset | Size | Field |
|---|---|---|
| 00 | 2 | Effect ID |
| 02 | 2 | Wave ID (effects can share a wave; the library loads it once) |
| 04 | 16 | A tone, as a bank's (1.1) |
| 14 | 4 | Size of the wave that follows |

The wave is SPU ADPCM (its first block zero), written as it is to SPU RAM. Soul Reaver's 885 pairs are 257
different sets with up to 59 effects each.

### 1.6 Gex's BIGFILE.DAT (Gex 2, Gex 3, Akuji)

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | Number of files |
| 04 | 24 × n | File entries |

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | Hash |
| 04 | 4 | Size (uncompressed) |
| 08 | 4 | Compressed size, or 0 if stored as it is |
| 0C | 4 | Offset (2048-byte aligned; the table isn't always in offset order) |
| 10 | 8 | Unknown |

Compression (psx-spx): 16-bit flags (low bit first); flag 0 copies a byte, flag 1 reads 2 bytes: length − 1 in the
low 4 bits of the first, distance in the other 12 bits (1-4095 back). A compressed bank still shows its `DNSa` at
offset 2, after the first flags.

### 1.7 Soul Reaver's BIGFILE.DAT

A directory of files found by name hash (`_BigFileDir` in the decompilation), stored without compression. Its banks
and samples can be found in it by their IDs at sector starts.

---

## Part 2: Sony's SQ / HD / BD

Sony's later music format (1999; Ape Escape, Alundra 2, The Legend of Dragoon, Wild Arms 2, Arc the Lad 3...).
This section adds to psx-spx's ".SQ .HD .BD (SSsq/SShd)" section; read that for the parts not repeated here.

It is a different format from the PS2's SQ/HD/BD (chunks named `SCEIVers`, `SCEIHead`...), which Sony's "Sound
Data Formats" manual describes; none of these PS1 games use that.

### 2.1 SQ (music, "SSsq")

| Offset | Size | Field |
|---|---|---|
| 000 | 2 | Volume (100; one Dragoon music: 60) |
| 002 | 2 | Ticks per quarter note (480) |
| 004 | 2 | Tempo in **beats per minute** |
| 006 | 6 | Zero |
| 00C | 4 | ID `SSsq` |
| 010 | 16 × 16 | Channels |
| 110 | ... | Events |

Channel (16 bytes): byte 1 channel, byte 2 program (FFh: none), byte 3 volume, byte 4 pan (64 centre); the others as
psx-spx describes (most unknown, constant in these games).

**Events** are MIDI's with these differences:

- The delta time (MIDI's variable length) comes **after** each event.
- Pitch bend has **one** data byte (0-127, 64 centre).
- Tempo is `FF 51` and a 16-bit beats per minute.
- The end is `FF 2F 00`, with no delta time after it.
- Running status: Alundra 2 uses it (and writes note offs as note on with velocity 0, and keeps a channel status
  across a tempo event); Ape Escape writes every status and uses 8n note offs.
- Loops are SEQ's: controller 99 value 20 (start), controller 6 (count, 127 endless), controller 99 value 30 (end).

### 2.2 HD (bank, "SShd")

As psx-spx describes (sizes of the HD and its BD, `SShd` at 0Ch, 28 offsets to data blocks), with these findings:

- **Tone fine tune** (byte 3) is signed, in 1/16 semitone, and **added** to the pitch (measured on the looped
  samples' periods).
- **Tone flags** (byte F), from bit 0, as Ape Escape's sound driver uses them:

| Bit | Meaning |
|---|---|
| 0 | High priority (the voice is allocated as such) |
| 1 | Noise: the voice plays the SPU's noise, its clock set from the tone's **root key byte** (written to bits 8-13 of the SPU control register) |
| 2, 3 | Not used: the driver never tests them |
| 4 | Pitch bend range from the program (program byte 4) instead of the tone's byte D |
| 5 | Modulation on (see below) |
| 6 | Breath wave from the program (program byte 5) instead of the tone's byte E |
| 7 | Reverb: the voice is added to the SPU's reverb voices |

- **Program type FFh is a drum kit**, not only sound effects: byte 6 is its first note, byte 7 its **last** note,
  and it has one tone per note in between (key range bytes zero; FFFFh sample offset for a note with no drum). Most
  kit roots are the note + 12 (22050 Hz samples). The driver plays tone (note - first note), skips notes below the
  first, and doesn't check the last note or an FFFFh sample.
- **Which tones a note plays**: a program with type bit 7 (layered) plays every tone whose key range has the note;
  any other program plays the first one. A note no tone covers plays nothing (so drum notes with no sample are
  silent in the game too).
- Velocity tables (data block 1) are 1:1 in every bank of the three games.
- Bend range: the tone's byte D (or the program's byte 4, with flag bit 4), in semitones.

**Modulation** (flag bit 5; data block 2's "breath waves"), as the driver runs it on each voice:

- The wave: the tone's byte E (or the program's byte 5, with flag bit 6) picks one of block 2's waves (64 bytes,
  80h the centre).
- The speed: the SQ channel's byte C ("breath"). Each update the voice's phase grows by the speed; past 240 it starts
  again at half the speed. The wave is read at phase / 4: only its first 60 values are used (as psx-spx noticed).
- The depth: the SQ channel's byte 9 ("modulation"); a sound effect's voice starts at 7Fh.
- The value: `wave × depth / 255 - (depth + 1) / 2 + 64`, a pitch bend value (64 the centre) added to the channel's
  bend, so the modulation's width is the tone's bend range at full depth.

**Data blocks 3 and 4** (sound effects): block 3 is a two-level table (an upper bound, then each set's offset; the
driver uses the offsets from the start of block 3 with their low bit masked: it is a flag), each set's sequences
ending with `FF 2F 00`. Block 4 (the "embedded SSsq") gives the effects' channels their volume and pan, as an SQ's
channel table. The effect sequences' commands (A0h note on, 80h, B0h... as the data shows) are not decoded.

### 2.3 BD (samples)

Raw SPU ADPCM, as a VAB's VB. The HD's sample offsets (× 8) count from its start.

### 2.4 Where the files are

There is no index linking a music to its bank; each game packs them its own way:

| Game | Packing |
|---|---|
| Ape Escape | `KKIIDDZZ.DAT` (index `KKIIDDZZ.HED`: 16-bit sector, 16-bit size in 128-byte units). A sound file is 5 sizes and 5 offsets (32-bit each) for: HD, effects HD, SQ, BD, effects BD |
| Alundra 2 | SQ, HD, BD in some files; HD, BD, SQ in others. The BD can follow padding |
| Arc the Lad III | `DATA.BIN` (329 MB): **BD, SQ, HD** for each music, each at a 2048-byte boundary: the samples before their bank. Some programs are a tone for each key (one key a tone, some keys with two layered), up to 23 tones |
| The Legend of Dragoon | See below |

**The Legend of Dragoon** keeps its sound in `SECT/DRGN0.BIN`, an **MRG** archive:

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | `MRG` 1Ah |
| 04 | 4 | Number of files |
| 08 | 8 × n | Each file's offset and size |

DRGN0's offsets count 2048-byte sectors (996 of its 7611 entries are empty placeholders). Each music is in a small
MRG inside it whose offsets count bytes: two 2-byte files, the SQ, then usually the HD and the BD. Findings:

- The music's bank is the one in the same small MRG.
- **A BD can be split:** the small MRG's last file holds only the first part (e.g. 6810h of 22100h bytes) and the
  rest is in DRGN0's next entries (e.g. 3 × 8000h + 38F0h bytes), loaded after each other.
- About 40% of the music MRGs have no bank: the game plays them with a bank loaded before. A bank elsewhere on the
  disc has a tone for all their notes for about half of those.
- **Battle music** (seen in the game's RAM in PCSX-Redux): a battle loads a "battle set", a small MRG with a music,
  its HD and its BD (DRGN0 entries 702, 707, 712, 717, ...), and the music played is the set's own. Beside it, right
  after the HD, the game loads one of the six musics with no bank at entries 696-701 (696 with set 702, 698 with set
  712) that doesn't play during the fight. The music after a battle (entry 5955) loads with its own bank.
- **No second bank:** in every scene seen, one music bank was in RAM (with the sound effect banks, whose HDs have no
  programs). Entry 6170, whose bank lacks one drum kit's notes, was played with its bank alone: those notes are
  silent in the game too.
- Entries 696-701 have copies packed with a bank (5905, 6225-6260); most of those banks lack some of their notes
  (as little as 10%), so they may not be the banks they play with. 13 musics have notes their bank lacks.
- Discs 2-4 hold the same music as disc 1.
- `XA/LODXA03.XA`'s directory entry is a placeholder pointing at sector 0 (it overlaps DRGN0.BIN).

---

## Part 3: Square's AKAO

Square's sound driver (AKAO, after its author Akihiko Matsueda) plays the music of Final Fantasy VII, VIII and IX,
Vagrant Story, Chrono Cross, Legend of Mana, SaGa Frontier, Parasite Eve and others. Its files all start with
`AKAO`: a **sequence** (the music) or a **sample set** (instruments and samples, a size of 0 at offset 6). The format
changed over the years; VGMTrans (`src/main/formats/Akao`, zlib licensed) names the versions:

| Version | Games |
|---|---|
| 1.0 | Final Fantasy VII |
| 1.1, 1.2 | SaGa Frontier; Front Mission 2, Chocobo's Mysterious Dungeon |
| 2 | Parasite Eve |
| 3.0, 3.1 | Another Mind; Chocobo Dungeon 2, Final Fantasy VIII, Chocobo Racing, SaGa Frontier 2 |
| 3.2 | Legend of Mana, Front Mission 3, Chrono Cross, Vagrant Story, Final Fantasy IX |

The command set below is VGMTrans's, checked here against every sequence of Final Fantasy VII and VIII and Vagrant
Story (98, 98 and 169 different ones: all read to their ends with it, with the corrections listed).

### 3.1 Sequences

| Offset | Size | Field (version 1.x) | Field (version 3.x) |
|---|---|---|---|
| 00 | 4 | `AKAO` | `AKAO` |
| 04 | 2 | Music ID | Music ID |
| 06 | 2 | Size, from offset 10h | Size, whole |
| 08 | 2 | Reverb type | Reverb type |
| 0A | 6 | Date (BCD) | |
| 10 | 4 | A bit for each track (version 1.x, 2) | |
| 14 | 2 | | ID of the sample set it plays |
| 14 / 20 | 2 x n | Each track's offset, from after itself (version 1.x; version 2: at 20h) | |
| 20 | 4 | | A bit for each track |
| 28-2F, 38-3F | | | Zero (Vagrant Story has a value at 28h) |
| 30 | 4 | | Its instruments, from 30h (0: none) |
| 34 | 4 | | Its drum kit, from 34h (0: none) |
| 40 | 2 x n | | Each track's offset, from itself |

Up to 32 tracks (Vagrant Story uses 32), each one voice. 48 ticks a beat; the tempo command's value is added to a
16-bit counter at each timer tick (33868800 / 8 Hz divided by 43D1h in version 1.0, 44E8h after), a tick of music
at each wrap.

A track is a byte code. **Notes**: 00h-83h a key (byte / 11: C to B in the current octave) with one of 11 lengths
(byte % 11: 192, 96, 48, 24, 12, 6, 3, 32, 16, 8, 4 ticks); 84h-8Eh a tie (the note before goes on), 8Fh-99h a
rest, with the same lengths. Version 3 also has F0h-FDh: the key (F0h + key; FCh a tie, FDh a rest), then the
length in the next byte. In version 1.x a note is released 2 ticks before its length ends (not with slur or legato
on); in version 3 it lasts its whole length. (VGMTrans takes 83h as a tie: it is the B with the shortest length.
Its share of the B notes in the three games, 0.2-7.5%, is that of the shortest length among the other keys,
0.7-9.8%; ties are far more common.)

**Commands** (argument bytes in brackets):

| Byte | Command | Byte | Command |
|---|---|---|---|
| A0 | End of track | C0 [1] | Transpose (signed) |
| A1 [1] | Program: an articulation | C1 [1] | Transpose, relative |
| A2 [1] | Length of the next note | C2 / C3 | Reverb on / off |
| A3 [1] | Volume (0-127) | C4 / C5 | Noise on / off |
| A4 [2] | Pitch slide (length, semitones) | C6 / C7 | Pitch modulation on / off |
| A5 [1] | Octave | C8 | Loop start (4 levels) |
| A6 / A7 | Octave up / down | C9 [1] | Loop end: n times in all (0: 256) |
| A8 [1] | Expression (0-127) | CA | Loop again (for ever) |
| A9 [2] | Expression fade (length, target) | CB | Reset voice effects |
| AA [1] | Pan (0-127, 64 the centre) | CC / CD | Slur on / off |
| AB [2] | Pan fade | CE, CF [1] | Noise toggles |
| AC [1] | Noise clock | D0 / D1 | Legato on / off |
| AD-B2 [1] | Attack, decay, sustain level, decay + level [2], sustain, release rate | D2, D3 [1] | Pitch modulation toggles |
| B3 | ADSR reset | D4-D7 | Side chains on / off |
| B4 [3] | Vibrato (delay, rate, shape) | D8 [1] | Tuning (signed: 127 an octave up, -128 down) |
| B5 [1], B6 | Vibrato depth, off | D9 [1] | Tuning, relative |
| B7 [1], BB [1], BF [1] | Attack, sustain, release mode | DA [1], DB | Portamento on, off |
| B8 [3], B9 [1], BA | Tremolo, depth, off | DC [1] | Fixed note length (relative) |
| BC [2], BD [1], BE | Pan LFO, depth, off | DD-DF [2] | Vibrato, tremolo, pan LFO depth fades |

From E0h the versions differ. Version 1.0 (FF7): E8h [2] tempo, E9h [3] tempo fade, EAh [2] reverb depth, EBh [3]
its fade, ECh [2] drum kit on (its offset), EDh drum kit off, EEh [2] jump (offset from after it), EFh [3]
conditional jump (on a value the game sets), F0h [3] loop branch, F1h [3] loop break, F2h [1] program without
attack, F3h, F4h [2] overlay voice, F5h, F6h [1], F7h [2] overlay balance, F8h [1] / F9h alternate voice, FDh [2]
time signature, FEh [2] measure number. Version 3 moves these under FEh (FEh 00h tempo [2], 01h [3], 02h [2], 03h
[3], 04h drum kit on (the sequence's own), 05h off, 06h jump [2], 07h conditional jump [3], 08h branch [3], 09h break
[3], 0Ah program without attack [1], 10h [1], 11h, 12h volume fade [2], 14h [1] program: one of the sequence's own
instruments, 15h [2], 16h [2], 19h [2] expression fade (per note), 1Ch [1], 1Dh, 1Eh), offsets counted from the
offset's own bytes. Version 3.2 adds E1h [1], E2h, E4h-E6h [2] rate fades, FEh 0Eh [2] play a pattern (a
subroutine) and FEh 0Fh its end, FEh 13h. Corrections to VGMTrans found in these games:

- FEh 1Fh (Vagrant Story, 802 times): no argument (the next command follows at once).
- CAh (loop again) never falls through: the bytes after it may be padding (FFh).

### 3.2 Instruments

An **articulation** is a sample with its loop, root key and the SPU's ADSR registers. The driver keeps a table of
256; the sample sets loaded fill it. A sequence plays them three ways:

- **A1h n**: articulation n, alone.
- **FEh 14h n** (version 3): the sequence's own instrument n: at offset 30h a table of 16 offsets (from 20h after
  the table; FFFFh none), each instrument a list of 8-byte regions ended by 8 zero bytes: articulation, lowest key,
  highest key, attack rate, sustain rate, sustain mode, release rate, volume (0: full).
- **Drum kits**: version 3 (FEh 04h): at offset 34h one 8-byte region a key (all 128): articulation, the key it plays
  at, attack rate, sustain rate, sustain mode, release rate, volume, pan (bit 7: reverb). Version 1 (ECh with an
  offset): 12 regions of 5 bytes (version 2: 6), one a key of the octave: articulation, key, volume (16-bit), pan.

### 3.3 Sample sets (version 3.1 and later)

| Offset | Size | Field |
|---|---|---|
| 00 | 4 | `AKAO` |
| 04 | 2 | Set ID |
| 06 | 2 | 0 (a sample set, not a sequence) |
| 10 | 4 | SPU RAM address it is loaded at |
| 14 | 4 | Size of its samples |
| 18 | 4 | ID of its first articulation |
| 1C | 4 | Number of articulations |
| 40 | 16 x n | Articulations: the sample's offset, the loop's offset (in its samples), fine tune (signed 16-bit: up to 2x up, 1/2 down), root key (16-bit), ADSR 1, ADSR 2 |
| ... | | The samples (SPU ADPCM) |

The games load several sets at once, the lower ones once and for all, each over the table of articulations:

| Articulations | Final Fantasy VIII | Vagrant Story |
|---|---|---|
| 0-31 (or all 256) | set 0 at SPU 3100h: 256 articulations, 17-255 a filler | `SOUND/WAVE0000.DAT` (set 0) at 3100h |
| 32-63 | set 5 at B000h | `SOUND/WAVE0005.DAT` (set 5) at 23100h |
| 64- | the set the sequence names, at 2B000h | the set it names (`SOUND/WAVE00nn.DAT`), at 35100h |

The sets kept loaded are below the music's set in SPU RAM and don't overlap it or each other (Vagrant Story has a
second set of 32-63 at an address overlapping set 0's: not loaded with it).

**Final Fantasy VII** has no sample sets: `SOUND/INSTR.ALL` holds the samples (its SPU address, the samples' size, 8
bytes, the samples) and `SOUND/INSTR.DAT` 128 articulations of 64 bytes (the sample's and its loop's SPU addresses;
attack, decay, sustain level, sustain, release rates; attack, sustain and release modes; then a pitch for each of the
12 keys, the first (C, key 72) 1000h for the sample as it is). `INSTR2.ALL` / `.DAT` replace articulations 53 and
up for One-Winged Angel's choir (VGMTrans).

### 3.4 Where the files are

| Game | Sequences | Instruments |
|---|---|---|
| Final Fantasy VII | Inside the fields' files (`FIELD/*.DAT`, LZS compressed: its size, then flag bytes, bit 0 first: 1 a byte as it is, 0 a copy of (4 bits + 3) bytes from a 12-bit place in a 4 KB ring starting at FEEh), the world map's (`WORLD/*.TXZ`), `.SND` files | `SOUND/INSTR.ALL`, `INSTR.DAT` |
| Final Fantasy VIII | Inside `FF8DISC1.IMG` (620 MB) | Sample sets inside `FF8DISC1.IMG` |
| Vagrant Story | `MUSIC/MUSIC000-191.DAT` (169 sequences) | `SOUND/WAVE*.DAT` |

FF7's 2,019 sequence copies are 97 different sequences (sequence 1 has no notes).

### 3.5 Vibrato and pitch slides (from the driver's code)

Read in Final Fantasy VII's driver (`SCUS_941.63`: command table at 80049AA8h, B4h at 80032614h, the vibrato's update
at 8002ED74h, a note's start at 80031500h-80031800h); Final Fantasy VIII's and Vagrant Story's drivers have the same
waveform tables (but 13-15).

- **Vibrato** (B4h delay, rate, waveform): at each note's start the driver sets the delay counter, the waveform to its
  start and the amplitude from the note's pitch: `depth x pitch >> 7` with B5h's bit 7 set, `depth x (pitch x 15 >>
  8) >> 7` without (bits 0-6: the depth). The delay counts ticks of the music; after it, every `rate` updates of the
  driver (its timer: 33868800 / 8 / 43D1h = 244 a second in FF7, / 44E8h = 240 in the later versions) the next value
  of the waveform: the pitch moves by `amplitude x value >> 16`, doubled when positive (the SPU pitch is linear: up an
  octave doubles it, down halves it). At the 15/256 scale, depth 10 (FF7's usual) is about +6 / -3 cents; at the 1/1
  scale depth 127 is about +1180 / -1130 cents. FF7's usual vibrato (rate 1, waveform 6) is 244 / 39 = 6.3 Hz,
  growing over its first 4 cycles.
- **Waveforms** (16, a table of 16-bit values each, then 0, 0 and how many values back it repeats from): 0-5 steps and
  pulses, 6 a sine of 39 steps growing over 4 cycles, 7 that sine, 8 a 16-step triangle growing, 9 that triangle, 10 /
  11 a 20-step sine growing / not, 12 random values; 13-15 copies of 9, 8, 9 in FF7, in the later versions random
  values below zero, a 10-step sine growing, that sine.
- **Pitch slide** (A4h length, semitones): for the next note only. The note starts at its own pitch and the driver
  steps its pitch (in Hz, a straight line) to the pitch of the note plus the semitones, over the length (ticks of the
  music); the key stays there (ties go on from it). Vagrant Story slides as far as 32 semitones (a drop of almost 3
  octaves in 4 ticks).
- **Tremolo** (B8h delay, rate, waveform; B9h depth 0-255; DEh depth fade): as vibrato (its own counters and
  waveform, restarted at each note), on the volume: `volume x depth / 128 x value / 32768` added, up as much as down
  (FF7's update at 8002EE20h).
- **Pan LFO** (BCh rate, waveform; BDh depth; DFh depth fade): no delay; the pan moved by `depth / 2 x value / 32768`
  (8002EEF8h).
- **Overlay voice** (F4h program, second articulation; F6h / F7h balance; F5h off, FF7): each note plays both
  articulations, the second at the balance's volume.

### 3.6 Playing a track

The tracks loop by themselves (jumps and endless loops), each over its own length: the PS1 DEV CART plays each
until it comes back to a place in the same loop state. An endless loop counts its passes, but only loop branches
compare the count, so it is kept at one past the largest branch count. Vagrant Story's dungeon pieces (MUSIC050-095)
are made of tracks looping over different lengths that drift against each other: they have no common loop.

---

## Part 4: What's still unknown, and how to find out

| Format | Unknown | How it could be settled |
|---|---|---|
| AAD | How much the squared volume curve changes balances | Listen side by side with the game; read SPU voice volumes from a snapshot |
| SQ/HD | The rate of the driver's updates (modulation speed in time: psx-spx's "one cycle a second at the lowest speed" would mean 240 a second) | Read the driver's timer setup, or SPU pitch registers |
| SQ/HD | The effect sequences' commands (data block 3) | The driver's effect sequence interpreter (`driver.psflib`) |
| AKAO | What the commands VGMTrans leaves unknown do (E0h-E2h, FEh 13h, 1Ch, 1Fh in Vagrant Story...) and the exact note release in version 3 | The driver's code (its command table: FF7 80049AA8h; Vagrant Story and FF8 at the start of their 400-word tables, 800301A0h / 80052258h) |
| AKAO | Whether a slurred or legato note restarts its vibrato | The note start code (FF7 80031500h) |
| AKAO | Which songs load FF7's INSTR2 (One-Winged Angel's choir: articulations 53 and up) | The game's code that loads it |
| SQ/HD | When Dragoon plays its musics 696-701, and with which bank (the copies' banks lack notes) | Watch the game's RAM (as above) at the moment one plays: perhaps when a character becomes a Dragoon in battle |

Settled since the first version of this page: the fine-tune unit (the same tables and code in all four games), the
header bytes (read or not by the games' code; the older layout's version field), the reverb fields (libspu's types
and depth), `FNSa` / `FMSa` (the sound effect sets, 1.5); and from Ape Escape's sound driver: the tone flags (bits 2
and 3 unused, the noise flag), modulation, which tones a note plays and that a note with no tone is silent, data
block 4 and block 3's tables; from Dragoon's RAM in an emulator: that it plays a music with its bank alone, and
which music a battle plays.

---

## Part 5: How the PS1 DEV CART converts them

- **AAD → SEQ + VAB** (`gui/aad.py`): the samples are copied as they are; tone fine tune becomes the VAB's centre
  and shift (1/128 semitone); mode 4 becomes VAB reverb. Each track of a sequence is played as the library plays it
  (loops, labels, nested loops), then the tracks are merged: the SEQ's endless loop is where every track with notes
  repeats together (a period within 2 ticks per repeat of the longest counts as fitting it), if that comes before the
  sequence ends and within 15 minutes; otherwise the tracks are written out to the end (at most 15 minutes). A
  channel's bend is written again after each note on (the library starts notes bent) and its transpose is applied to
  the notes. The bank's reverb type and depth are shown (to set the tester's reverb the same). Not converted: the
  control scripts and the game's commands (variables, branches, slots, mutes).
- **Sound effect sets → VAB + SEQ** (`gui/aad.py`): program n is effect n (its tone and wave; a shared wave once),
  and a SEQ plays each effect once, half a second apart, to hear them in the tester.
- **SQ/HD → SEQ + VAB** (`gui/sshd.py`): the samples are copied; the delta times moved, the 1-byte bend widened,
  BPM tempo converted; drum kits become a program with one tone per note. Not converted: modulation, noise, sound
  effects. A music with no bank of its own (Dragoon) takes a copy of it packed with a bank that has all its
  notes, else the first bank after it that has them all (a guess: see Part 4), else the bank with the most. A bank's BD is the nearest place to its HD, after or before it (Arc the Lad III), where each of its
  samples starts. A program of more than 16 tones of one key each keeps its first 16 (a VAB's limit; the notes
  left out are listed).
- **AKAO → SEQ + VAB** (`gui/akao.py`): each track played as the driver plays it (octaves, loops at 4 levels, jumps,
  patterns, the game's condition taken as 0), merged with one loop where the tracks with notes repeat together
  (within 4 ticks a repeat; a track whose repeating part has no notes counts as over), else written out to 15
  minutes. Each note becomes a tone of the VAB with what it starts with: its articulation (the sample copied, its
  loop flags set from the loop), the region's and the track's ADSR changes, the tuning, the reverb (a program for
  each set of them, a drum kit 16 keys a program). AKAO's 32 tracks are given the SEQ's 16 channels note by note:
  a track keeps its channel while it is free, the channel's volume, expression and pan set before its note (a
  note that has to share a channel with another track's playing has that track's settings). Fades are written as
  steps every 3 ticks. Vibrato and pitch slides (3.5) become pitch bends every 2 ticks (when the pitch moved a cent):
  a note bent has a channel of its own, the VAB's bend range is the song's widest bend (2 semitones for most FF7
  songs, up to 33 for Vagrant Story's slides). Tremolo and pan LFO become expression (CC11) and pan (CC10) changes the
  same way (the track's own expression or pan, moved by the LFO), the note on a channel of its own; an overlay voice is
  a second tone in the program, the balance setting both volumes. Files bigger than the sound tester's room (880 KB of
  SEQ + VH + VB) are written again with the effects every 4 or 8 ticks, then a song that doesn't loop shortened to 10
  or 5 minutes (Vagrant Story's dungeon pieces: every 4 ticks). Not converted (none of them in a SEQ + VAB): noise,
  pitch modulation, alternate voices (FF7, a release overlapping the next note); portamento is used by none of the
  three games.
- The scans read disc images sector by sector (`gui/discimage.py`) and never load a whole image.
