/*
* PS1 DEV CART sound tester
* Uploaded and executed by "ps1transfer -snd file [-snd file ...]" (VH + VB, VAB, SEQ, VAG)
*
* ps1transfer uploads a data block at TESTER_DATA_ADRS (header + files) before executing this program.
* - VAB sound bank (VH + VB files, or one VAB file): play the instruments (programs) with the controller
* - SEQ music (with a VAB): start/stop
* - VAG sample (without VAB): play it at different notes, the original sample rate is note C4
*
* Controls:	Left/Right: program, Up/Down: note, L1/R1: octave, Cross: play (while held),
*			Circle: SEQ start/stop, Select: the next SEQ of a SEP, Triangle/Square: volume, Start: stop all sounds
* The buttons can also be pressed from the computer: PsyQ pad bits written to REMOTE_PAD_ADRS (e.g. with
* "ps1transfer -poke"), added to the controller ones.
* PS1 Dev Cart Manager ("ps1transfer -snd ... -sndlive", see ps1transfer/sndlive.c): the program, note, volume, SEQ and
* voices are shared with the computer (SoundSync, written at each frame), which can change them (requests applied at
* the next frame) and play the note (Cross, with REMOTE_PAD_ADRS): its controls and the controller's act on the same
* state, each shows what the other one does.
* VAB editor (the manager's Sound Tools): the VAB player reads the program and tone attributes from the VH in RAM (its
* address in SoundSync.vh_adrs) at each key on, so the computer edits them there; req_refresh applies them at once to
* the notes sounding too (volume, pan, pitch, ADSR).
* Effects (the manager's Sound Tools, req_fx): SPU reverb (libspu's types, depth, on the notes of the tones with
* reverb, all, none or the SEQ channels chosen; chaos echo and delay: libspu's delay time and feedback), SEQ transpose,
* tempo and channels muted, mono.
*
* Built with PSn00bSDK (open source) instead of PsyQ. PsyQ's libsnd is replaced by our own VAB player
* (programs, tones with their key range, pitch from the center note and fine tune, volume and pan, ADSR) and SEQ player
* (note on/off, program change, volume/pan/sustain controllers, pitch bend, tempo, loop), on the 24 SPU voices.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxspu.h>
#include "psyq_compat.h"
#include "devcart_ui.h"

// Data block written by ps1transfer, see "-snd" in ps1transfer/main.c
#define	TESTER_DATA_ADRS	0x80100000
#define	TESTER_MAGIC		0x54444E53	// "SNDT"
#define	TESTER_MAX_FILES	8
#define	REMOTE_PAD_ADRS		0x800FFFF0	// Buttons pressed from the computer (PsyQ pad bits)

// State shared with the PS1 Dev Cart Manager ("ps1transfer -sndlive", see ps1transfer/sndlive.c), in the free end of the
// data block header (0x100 bytes, the file table uses 0x68): written at each frame, and the changes the computer asks
// for (applied at the next frame, then set back to -1)
#define	TESTER_SYNC_ADRS	(TESTER_DATA_ADRS + 0x80)
#define	TESTER_SYNC_MAGIC	0x534E4453	// "SDNS": the tester has the shared state

typedef struct
{
	uint32_t	magic;
	int16_t		kind;				// 0: no sound data, 1: VAB, 2: VAG
	int16_t		n_programs;
	int16_t		prog_index;			// Program chosen (index in the programs with tones), its number
	int16_t		program;
	int16_t		note;				// 0-127, 60 = C4
	int16_t		volume;				// Tester volume 0-127
	int16_t		playing;			// The note is played (Cross held, on the controller or from the computer)
	int16_t		seq;				// 0: no SEQ, 1: stopped, 2: playing
	int16_t		seq_bar, seq_loop;
	uint32_t	voices;				// Voices sounding (bit n: voice n, envelope not ended)
	uint32_t	vag_rate;			// VAG sample rate (Hz)
	int16_t		vab_ps, vab_ts, vab_vs;	// VAB programs, tones, VAGs
	uint16_t	frame;				// Counts the frames
	uint8_t		prog_map[16];		// Programs with tones (bit n: program n)
	int16_t		req_prog, req_note, req_volume;	// Asked by the computer, -1: nothing
	int16_t		req_seq;			// 0: stop, 1: start, -1: nothing
	int16_t		req_stop;			// 1: stop all sounds, -1: nothing
	int16_t		req_refresh;		// 1: the VAB attributes edited by the computer applied to the notes sounding
	uint32_t	vh_adrs, vh_size;	// The VH in RAM (VAB editor), 0: no VAB
	// Effects, written by the computer then req_fx set (applied at the next frame, req_fx set back to -1)
	int16_t		req_fx;				// 1: the fx_ values applied, -1: nothing
	int16_t		fx_reverb;			// Reverb type (REVERB_ names), 0: off
	int16_t		fx_depth;			// Reverb depth (output volume) 0-127
	int16_t		fx_rev_voices;		// Notes with reverb: 0 as the bank's tones (mode 4), 1 all, 2 none, 3 the SEQ
									// channels chosen (SoundFx2; the keyboard's as the bank's tones)
	int16_t		fx_transpose;		// SEQ: semitones (-24..24)
	int16_t		fx_tempo;			// SEQ: tempo in percent (25-400)
	int16_t		fx_mono;			// 1: mono (every note at the center)
	uint16_t	fx_mute;			// SEQ: channels muted (bit n: channel n)
	// Written by the tester
	int16_t		rev_active;			// Reverb type in use, 0: off, -1: not enough SPU RAM (the bank uses it)
	uint16_t	ch_used;			// SEQ channels with notes (bit n: channel n)
	uint16_t	ch_active;			// SEQ channels with notes sounding
	uint8_t		ch_prog[16];		// Program of each SEQ channel
	uint16_t	fx_version;			// 1: the effects above, 2: also SoundFx2 (0: an older tester)
	uint32_t	samples_end;		// SPU RAM used by the samples (from 0 to this address)
	int16_t		req_sep;			// SEQ of a SEP (multi-track file) played: its index, -1: nothing
	int16_t		sep_count;			// SEQs in the music file (1: a SEQ, more: a SEP)
	int16_t		sep_index;			// The one chosen
	int16_t		req_repeat;			// At the end of the music: 1 played again (default), 0 stopped; -1: nothing asked
} SoundSync;

// (offsets used by ps1transfer/sndlive.c: SYNC_)
#define	SYNC_CHECK(_n_, _c_)	typedef char sync_check_##_n_[(_c_) ? 1 : -1]
SYNC_CHECK(fx, __builtin_offsetof(SoundSync, req_fx) == 76);
SYNC_CHECK(chprog, __builtin_offsetof(SoundSync, ch_prog) == 98);
SYNC_CHECK(version, __builtin_offsetof(SoundSync, fx_version) == 114);
SYNC_CHECK(sep, __builtin_offsetof(SoundSync, req_sep) == 120);
SYNC_CHECK(repeat, __builtin_offsetof(SoundSync, req_repeat) == 126);
SYNC_CHECK(size, sizeof(SoundSync) == 128);	// (the data block header's free end: 0x80 bytes)

// The SEQ position (Manager: its progress bar, seek and pause), in the header's free bytes before SoundSync (the file
// table ends at 0x68; ps1transfer writes them as 0, a tester without it leaves version 0)
#define	TESTER_POS_ADRS		(TESTER_DATA_ADRS + 0x68)

typedef struct
{
	uint32_t	pos_ms;				// Position in the music (from its start, back at a loop's start when it jumps)
	uint32_t	len_ms;				// Length of the music (to its end, its loops played once)
	int32_t		req_seek;			// Asked by the computer: played from this time (ms), -1: nothing
	int16_t		req_pause;			// 1: pause, 0: resume, -1: nothing
	int16_t		paused;				// 1: the music is paused
	uint16_t	version;			// 1: this block, 2: also the SEQ reload, 3: also the bank reload
	int16_t		req_reload;			// A new SEQ written by the computer, played from the same time: 1 at the SEQ
									// file's address, 2 at TESTER_SEQ_SPARE (the computer writes the one not played);
									// 3: new sound files (the file table and the files rewritten, BankReload)
	uint32_t	reload_size;		// Its size (3: 1 the music stays paused)
} SoundPos;

// The second place of a reloaded SEQ (the end of the data block: ps1transfer checks the files end below it)
#define	TESTER_SEQ_SPARE	0x801C0000
#define	TESTER_SEQ_SPARE_SIZE	0x20000

SYNC_CHECK(pos, sizeof(SoundPos) == 0x18);

// More effects (SoundSync is full), after it: the data block header is 0x120 bytes for a ps1transfer that knows this
// block (its files start after it; an older one puts the first file here: the block not used, fx_version 1). Written
// by the computer before req_fx
#define	TESTER_FX2_ADRS		(TESTER_DATA_ADRS + 0x100)
#define	TESTER_FX2_END		(TESTER_DATA_ADRS + 0x120)

typedef struct
{
	uint16_t	version;			// 1: this block (written by the tester)
	int16_t		rev_delay;			// Chaos echo and delay: libspu's delay time 0-127, -1: libspu's default (127)
	int16_t		rev_feedback;		// Chaos echo and delay: libspu's feedback 0-127, -1: libspu's default (127, 0)
	uint16_t	rev_channels;		// fx_rev_voices 3: the SEQ channels whose notes have reverb (bit n: channel n)
	uint8_t		spare[24];
} SoundFx2;

SYNC_CHECK(fx2, sizeof(SoundFx2) == TESTER_FX2_END - TESTER_FX2_ADRS);

enum
{
	FILE_VH = 1,
	FILE_VB,
	FILE_VAB,	// VH followed by VB
	FILE_SEQ,
	FILE_VAG
};

typedef struct
{
	uint32_t	type;
	uint32_t	adrs;
	uint32_t	size;
} TesterFile;

typedef struct
{
	uint32_t	magic;
	uint32_t	count;
	TesterFile	files[TESTER_MAX_FILES];
} TesterData;

// VAB header (VH): header, 128 program attributes, 16 tone attributes per program with tones, VAG sizes (8 bytes units)
typedef struct
{
	char		form[4];	// "pBAV"
	uint32_t	ver, id, fsize;
	uint16_t	reserved0, ps, ts, vs;	// Programs, tones, VAGs
	uint8_t		mvol, pan, attr1, attr2;
	uint32_t	reserved1;
} VabHdr;

typedef struct
{
	uint8_t		tones, mvol, prior, mode, mpan, reserved0;
	uint16_t	attr;
	uint32_t	reserved1, reserved2;
} ProgAtr;

typedef struct
{
	uint8_t		prior, mode, vol, pan, center, shift, min, max;	// shift: fine tune in 1/128 semitone
	uint8_t		vibW, vibT, porW, porT, pbmin, pbmax, reserved1, reserved2;
	uint16_t	adsr1, adsr2;
	int16_t		prog, vag, reserved[4];
} VagAtr;

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	VOICES			24
#define	VAG_VOICE		23			// Voice used to play a VAG sample (no VAB)
#define	SPU_DATA_ADRS	0x1010		// First SPU RAM address after the reserved area
#define	VAG_HEADER_SIZE	48
#define	SPU_VOICE(_v_)	((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))	// volL, volR, pitch, start, adsr1, adsr2, envx
#define	SPU_REG(_a_)	(*((volatile uint16_t *)(_a_)))

// Reverb types (the registers 0x1F801DC0-0x1F801DFF of libspu's modes, as listed in psx-spx; Hall checked on the
// console), work area at the top of SPU RAM: dAPF1/2, vIIR, vCOMB1-4, vWALL, vAPF1/2, mLSAME..mRAPF2, vLIN/vRIN
#define	REVERB_TYPES	10
static const struct
{
	const char	*name;
	uint32_t	size;		// Work area (bytes)
	uint16_t	regs[32];
}	reverb_types[REVERB_TYPES] = {
	{"OFF", 0, {0}},
	{"ROOM", 0x26C0, {
		0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
		0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136,
		0x00B8, 0x005C, 0x8000, 0x8000}},
	{"STUDIO SMALL", 0x1F40, {
		0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF,
		0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080,
		0x004C, 0x0026, 0x8000, 0x8000}},
	{"STUDIO MEDIUM", 0x4840, {
		0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F,
		0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2,
		0x0100, 0x0080, 0x8000, 0x8000}},
	{"STUDIO LARGE", 0x6FE0, {
		0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C,
		0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238,
		0x0154, 0x00AA, 0x8000, 0x8000}},
	{"HALL", 0xADE0, {
		0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD,
		0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A,
		0x0274, 0x013A, 0x8000, 0x8000}},
	{"HALF ECHO", 0x3C00, {
		0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF,
		0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040,
		0x0028, 0x0014, 0x8000, 0x8000}},
	{"SPACE ECHO", 0xF6C0, {
		0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B,
		0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2,
		0x0464, 0x0232, 0x8000, 0x8000}},
	{"CHAOS ECHO", 0x18040, {
		0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
		0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002,
		0x0004, 0x0002, 0x8000, 0x8000}},
	{"DELAY", 0x18040, {
		0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005,
		0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002,
		0x0004, 0x0002, 0x8000, 0x8000}},
};

// Effects (asked by the computer, see SoundSync)
static uint32_t	seq_notes;			// Notes the music has played since its start
static int		seq_repeat = 1;		// At the end of the music: played again (1) or stopped (0)
static int		start_clear = 3;	// Frames before the reverb work area is cleared at the start
// A reload while the computer is linked (BankReload): nothing printed (the text and the computer's transfers would
// meet on the USB link: ps1transfer reads the state meanwhile)
static int		quiet_load;
static int		fx_reverb, fx_depth = 64, fx_rev_voices, fx_transpose, fx_tempo = 100, fx_mono;
static uint16_t	fx_mute;
static int		rev_active;				// Reverb type in use, -1: not enough SPU RAM
static SoundFx2	*fx2;					// The computer's SoundFx2, NULL: not in the data block (older ps1transfer)
static int		fx_delay = -1, fx_feedback = -1;	// Chaos echo, delay: libspu's delay time and feedback, -1 default
static uint16_t	fx_rev_channels = 0xFFFF;		// fx_rev_voices 3: the SEQ channels with reverb
static uint32_t	samples_end = 0x1010;	// SPU RAM used by the samples
static uint32_t	eon;					// Voices in the reverb (shadow of 0x1F801D98/9A)

// Key on / off: the SPU reads its key registers at its own pace (each sample), so voices keyed one write at a time lose
// some (a write replaces the previous one before it was taken: notes never released, Stop all sounds ignored), and a
// key off just after the key on of the same voice is not taken (a short SEQ note: never released). The voices are
// gathered and written at once each frame (SpuKeys), a key off of a voice keyed on in the frame at the next one.
static uint32_t	key_on, key_off, key_off_next;
#define	SPU_KEY_ON(_v_)		(key_on |= 1 << (_v_), key_off &= ~(1 << (_v_)), key_off_next &= ~(1 << (_v_)))
#define	SPU_KEY_OFF(_v_)	((key_on & (1 << (_v_))) ? (key_off_next |= 1 << (_v_)) : (key_off |= 1 << (_v_)))

static void	SpuKeys(void)
{
	volatile int	i;

	if (key_off)
	{
		SPU_REG(0x1F801D8C) = key_off;
		SPU_REG(0x1F801D8E) = key_off >> 16;
		for (i = 0; i < 2000; i++)	// (some SPU samples: the key off taken before a key on of the same voice)
			;
	}
	if (key_on)
	{
		SPU_REG(0x1F801D88) = key_on;
		SPU_REG(0x1F801D8A) = key_on >> 16;
	}
	key_off = key_off_next;
	key_on = key_off_next = 0;
}

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			fnt_data, fnt_play, fnt_voices;	// Text of the panels

static const char	*note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const uint16_t	semitone[12] = {4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732};

static int	Clamp(int v, int min, int max)
{
	return ((v < min) ? min : ((v > max) ? max : v));
}

// SPU pitch (0x1000 = 44100 Hz) for a sample rate and a number of cents from the note of that rate
static int	PitchCents(int rate, int cents)
{
	int	octave = (cents >= 0) ? cents / 1200 : -((-cents + 1199) / 1200);
	int	rest = cents - octave * 1200;
	int	pitch = (rate * 4096 / 44100) * semitone[rest / 100] >> 12;

	pitch = pitch * (100000 + (rest % 100) * 58) / 100000;	// 2^(c/1200) ~ 1 + c * 0.000578
	pitch = (octave >= 0) ? pitch << octave : pitch >> -octave;
	return (Clamp(pitch, 1, 0x3FFF));
}

/****************************************/
// VAB bank

static VabHdr		*vab;
static ProgAtr		*prog_atr;
static VagAtr		*tone_atr;
static uint32_t		vag_adrs[256];		// SPU RAM address of each VAG (1..vs)
static int16_t		prog_block[128];	// Tone block of each program (-1: no tones)
static int16_t		programs[128];		// Program numbers with tones
static int			n_programs;
static uint8_t		*vag_data;			// VAG sample without VAB
static uint32_t		vag_rate;

static uint32_t	BigEndian32(uint8_t *p)
{
	return ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static uint32_t	VhSize(VabHdr *vh)
{
	return (sizeof(VabHdr) + 128 * sizeof(ProgAtr) + vh->ps * 16 * sizeof(VagAtr) + 256 * 2);
}

// SPU RAM written by hand, as the debug monitor writes it (monitor.c SpuWrite, reliable): PSn00bSDK's DMA (SpuWrite) lost
// about half of a bank's samples after a restart (measured on the console: the old data stayed), and the reverb work
// area wasn't cleared. Transfer stopped, the address set once per 1KB, then 32 halfwords at a time through the transfer
// FIFO, each manual transfer awaited; the control register put back as it was (an SPU left in another state hung the
// monitor's SPU RAM read). data NULL: zeros.
#define	SPU_CNT_REG		SPU_REG(0x1F801DAA)
#define	SPU_STAT_REG	SPU_REG(0x1F801DAE)

static void	SpuWriteRam(uint32_t adrs, const uint8_t *data, uint32_t size)
{
	uint16_t	cnt = SPU_CNT_REG;
	uint32_t	n, i;
	int			t;

	for (t = 1000000; (*((volatile uint32_t *)0x1F8010C8) & (1 << 24)) && t; t--)	// (an SPU DMA going on: ended)
		;
	size = (size + 1) & ~1;
	while (size)
	{
		n = (size > 1024) ? 1024 : size;
		SPU_CNT_REG = cnt & ~0x30;				// Transfer mode: stop
		for (t = 100000; (SPU_STAT_REG & 0x30) && t; t--)
			;
		SPU_REG(0x1F801DA6) = adrs >> 3;
		for (i = 0; i < n; i += 2)
		{
			SPU_REG(0x1F801DA8) = data ? (data[i] | (data[i + 1] << 8)) : 0;
			if (((i & 63) == 62) || (i + 2 >= n))	// FIFO full (32 halfwords) or the end: a manual write
			{
				// Started, then ended: the busy flag isn't up at once, and waiting only for it to be down stopped the
				// transfer before it began (measured: 12% of the samples written). The monitor never saw it: it
				// writes as the USB bytes come, slowly.
				SPU_CNT_REG = (cnt & ~0x30) | 0x10;
				for (t = 2000; !(SPU_STAT_REG & (1 << 10)) && t; t--)
					;
				for (t = 100000; (SPU_STAT_REG & (1 << 10)) && t; t--)
					;
				SPU_CNT_REG = cnt & ~0x30;
			}
		}
		adrs += n;
		if (data)
			data += n;
		size -= n;
	}
	SPU_CNT_REG = cnt;
}

static void	SpuUpload(uint32_t adrs, uint8_t *data, uint32_t size)
{
	SpuWriteRam(adrs, data, size);
}

static int	LoadVab(uint8_t *vh, uint8_t *vb)
{
	uint16_t	*sizes;
	uint32_t	adrs = SPU_DATA_ADRS, vb_size = 0;
	int			i, block = 0;

	vab = (VabHdr *)vh;
	if (memcmp(vab->form, "pBAV", 4))
	{
		vab = NULL;
		return (0);
	}
	prog_atr = (ProgAtr *)(vh + sizeof(VabHdr));
	tone_atr = (VagAtr *)(vh + sizeof(VabHdr) + 128 * sizeof(ProgAtr));
	sizes = (uint16_t *)(vh + VhSize(vab) - 256 * 2);
	for (i = 1; (i <= vab->vs) && (i < 256); i++)	// VAG 0 is not used
	{
		vag_adrs[i] = adrs;
		adrs += sizes[i] * 8;
		vb_size += sizes[i] * 8;
	}
	for (i = 0; i < 128; i++)
	{
		prog_block[i] = -1;
		if (prog_atr[i].tones && (block < vab->ps))
		{
			prog_block[i] = block++;
			programs[n_programs++] = i;
		}
	}
	if (adrs > 0x80000)
		vb_size = 0x80000 - SPU_DATA_ADRS;	// (bank too big for the SPU RAM: the end is lost)
	SpuUpload(SPU_DATA_ADRS, vb, vb_size);
	samples_end = SPU_DATA_ADRS + vb_size;
	if (!quiet_load)
		printf("VAB: %d programs, %d tones, %d VAGs, %d bytes in SPU RAM\n", vab->ps, vab->ts, vab->vs, (int)vb_size);
	return (1);
}

// Tones of a program for a note (tones have a note range): every tone whose range has the note, as libsnd plays them
// (layered sounds: e.g. Alundra's tones over the whole range on top of one per range), from tone 'from' on; NULL if no
// more. *next: the tone to look from for the next one
static VagAtr	*FindTone(int prog, int note, int from, int *next)
{
	VagAtr	*t;
	int		i;

	if (!vab || (prog < 0) || (prog > 127) || (prog_block[prog] < 0))
		return (NULL);
	t = tone_atr + prog_block[prog] * 16;
	for (i = from; i < prog_atr[prog].tones; i++)
		if ((note >= t[i].min) && (note <= t[i].max))
		{
			*next = i + 1;
			return (&t[i]);
		}
	return (NULL);
}

/****************************************/
// Voices

typedef struct
{
	int		on;			// Key on (not released yet)
	int		channel;	// SEQ channel, -1: played with the controller
	int		note;
	int		velocity;
	int		prog;
	VagAtr	*tone;
	uint32_t	age;
	int		bend;		// Pitch bend of the note (-8192..8191): 0 at its key on, the channel's bends while it sounds
} Voice;

typedef struct
{
	int		prog, volume, pan, bend, sustain;
	int		expression;	// Controller 11 (0-127), on top of the volume (e.g. LSD: Dream Emulator's swells)
} Channel;

static Voice	voices[VOICES];
static Channel	channels[16];
static uint32_t	voice_age;
static int		master_volume = 100;	// Triangle/Square

static void	VoiceVolume(int v)
{
	Voice	*vo = &voices[v];
	Channel	*c = (vo->channel >= 0) ? &channels[vo->channel] : NULL;
	int		vol = 0x3FFF, pan;

	// Velocity, channel, tone, program, bank and tester volumes (0-127 each), kept at the SPU scale (same values as
	// libsnd's key on), then the pan: full volume on both sides at the center
	vol = vol * vo->velocity / 127;
	vol = vol * (c ? c->volume : 127) / 127;
	vol = vol * (c ? c->expression : 127) / 127;
	vol = vol * vo->tone->vol / 127;
	vol = vol * prog_atr[vo->prog].mvol / 127;
	vol = vol * vab->mvol / 127;
	vol = vol * master_volume / 127;
	pan = fx_mono ? 64 : Clamp(vo->tone->pan + prog_atr[vo->prog].mpan + (c ? c->pan : 64) - 128, 0, 127);
	SPU_VOICE(v)[0] = (pan <= 64) ? vol : vol * (127 - pan) / 63;
	SPU_VOICE(v)[1] = (pan >= 64) ? vol : vol * pan / 64;
}

// The note a SEQ note is played at (transpose), the ones of the controller as they are
static int	PlayedNote(int channel, int note)
{
	return ((channel >= 0) ? Clamp(note + fx_transpose, 0, 127) : note);
}

// Pitch: 44100 Hz at the tone's center note, its fine tune, pitch bend: the tone's range in semitones, pbmin down and
// pbmax up; 0: no bend (as the games' libsnd: a default of 2 semitones put notes out of tune, commercial SEQs leave a
// channel's bend off-centre on tones with no range, e.g. PoPoLoCrois and Legend of Legaia). The bend is the note's own:
// as libsnd, a bend event bends the notes sounding, a new note starts unbent (Alundra: a quick fall or rise on one
// note, the bend set back only at the next one's; the notes between were heard 1 or 2 semitones off)
static void	VoicePitch(int v)
{
	Voice	*vo = &voices[v];
	int		bend = vo->bend;
	int		range = (bend < 0) ? vo->tone->pbmin : vo->tone->pbmax;
	int		cents = (PlayedNote(vo->channel, vo->note) - vo->tone->center) * 100 + vo->tone->shift * 100 / 128 +
					bend * range * 100 / 8192;

	SPU_VOICE(v)[2] = PitchCents(44100, cents);
}

// Reverb of a voice: as its tone (VAB tone mode 4: reverb, as libsnd), all or no notes, or the SEQ channels chosen
// (a note of the keyboard or a VAG, channel -1: as its tone)
static void	VoiceReverb(int v, int channel, int bank_reverb)
{
	int	on = (fx_rev_voices == 1) || ((fx_rev_voices == 0) && bank_reverb) ||
			 ((fx_rev_voices == 3) && ((channel >= 0) ? (fx_rev_channels >> channel) & 1 : bank_reverb));

	eon = on ? (eon | (1 << v)) : (eon & ~(1 << v));
	SPU_REG(0x1F801D98) = eon;
	SPU_REG(0x1F801D9A) = eon >> 16;
}

#define	REVERB_AREA_MAX		0x18040	// The biggest work area (chaos echo, delay)

// The SPU RAM a reverb work area can be in zeroed: from the biggest one's start (or the end of the samples, if they go
// further) to the end. Only the new type's area was zeroed: what an earlier, bigger type (or the program before the
// tester: a game, the tester run before with another type) had left below it stayed, and the reverb unit, whose
// position isn't reset when mBASE changes, went on through it (old echoes heard on the first notes)
// Written with PSn00bSDK's DMA, as the samples are; each block twice (a DMA block was lost now and then). (Written by
// hand, through the SPU's transfer FIFO, the SPU was left in a state where the debug monitor's SPU RAM read, waiting for
// the SPU without a time limit, hung: the USB link with it, until a reset.)
static void	ReverbClear(void)
{
	uint32_t	start = (0x80000 - REVERB_AREA_MAX) & ~1023;

	if (start < samples_end)
		start = (samples_end + 1023) & ~1023;
	SpuWriteRam(start, NULL, 0x80000 - start);
}

#define	REVERB_CHAOS_ECHO	8	// libspu's SPU_REV_MODE_ECHO and SPU_REV_MODE_DELAY: delay time and feedback
#define	REVERB_DELAY		9

// Chaos echo and delay: libspu's delay time and feedback (SpuSetReverbModeParam, its SPU_REV_DELAYTIME and
// SPU_REV_FEEDBACK). The table is libspu's at delay 127 and feedback 127 (chaos echo) or 0 (delay), its defaults; the
// same sums as libspu, its quirks kept: feedback 127 is vWALL 8100h (-0.99), 126 down to 0 are +0.99 down to 0 (the
// same echoes a game hears with them)
static void	ReverbEcho(int type)
{
	const uint16_t	*r = reverb_types[type].regs;
	int				delay = (fx_delay < 0) ? 127 : fx_delay;
	int				feedback = (fx_feedback >= 0) ? fx_feedback : ((type == REVERB_CHAOS_ECHO) ? 127 : 0);
	uint16_t		d13 = (delay << 13) / 127, d12 = (delay << 12) / 127;

	if ((type != REVERB_CHAOS_ECHO) && (type != REVERB_DELAY))
		return;
	SPU_REG(0x1F801DC0 + 10 * 2) = d13 - r[0];	// mLSAME: delay - dAPF1
	SPU_REG(0x1F801DC0 + 11 * 2) = d12 - r[1];	// mRSAME: - dAPF2
	SPU_REG(0x1F801DC0 + 12 * 2) = d12 + r[13];	// mLCOMB1: + mRCOMB1
	SPU_REG(0x1F801DC0 + 16 * 2) = d12 + r[17];	// dLSAME: + dRSAME
	SPU_REG(0x1F801DC0 + 26 * 2) = d12 + r[28];	// mLAPF1: + mLAPF2
	SPU_REG(0x1F801DC0 + 27 * 2) = d12 + r[29];	// mRAPF1: + mRAPF2
	SPU_REG(0x1F801DC0 + 7 * 2) = (uint16_t)(feedback * 0x8100 / 127);	// vWALL
}

// Reverb type: the work area at the top of SPU RAM (zeroed: no old data heard), above the samples, else none (-1)
static int	ReverbSet(int type)
{
	uint32_t	base;
	int			i;

	SPU_REG(0x1F801DAA) &= ~(1 << 7);	// Reverb master off (also while its work area is cleared)
	SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = 0;
	ReverbClear();
	if ((type <= 0) || (type >= REVERB_TYPES))
		return (0);
	base = (0x80000 - reverb_types[type].size) & ~1023;	// (zeroed in whole DMA blocks)
	if (base < samples_end)
		return (-1);
	for (i = 0; i < 32; i++)
		SPU_REG(0x1F801DC0 + i * 2) = reverb_types[type].regs[i];
	ReverbEcho(type);
	SPU_REG(0x1F801DA2) = base / 8;		// Work area address
	SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = fx_depth * 0x7FFF / 127;	// Depth: reverb output volume
	SPU_REG(0x1F801DAA) |= 1 << 7;		// Reverb master on
	return (type);
}

// The VAB attributes edited by the computer (VAB editor) heard at once on the notes sounding: volume and pan, pitch,
// ADSR (the tone and key range of a note sounding stay: the next notes use the new ones)
static void	RefreshVoices(void)
{
	int	v;

	for (v = 0; v < VOICES - 1; v++)
		if (vab && voices[v].tone && SPU_VOICE(v)[6])
		{
			VoiceVolume(v);
			VoicePitch(v);
			SPU_VOICE(v)[4] = voices[v].tone->adsr1;
			SPU_VOICE(v)[5] = voices[v].tone->adsr2;
		}
}

static int	FreeVoice(void)
{
	int	v, best = -1;

	for (v = 0; v < VOICES - 1; v++)	// (VAG_VOICE kept for a VAG sample)
		if (!voices[v].on && !SPU_VOICE(v)[6])	// Released and envelope ended
			return (v);
	for (v = 0; v < VOICES - 1; v++)	// Oldest released voice, else oldest voice
		if (!voices[v].on && ((best < 0) || (voices[v].age < voices[best].age)))
			best = v;
	if (best < 0)
		for (v = best = 0; v < VOICES - 1; v++)
			if (voices[v].age < voices[best].age)
				best = v;
	return (best);
}

// A voice for each tone of the program that has the note (layered tones: several voices, released together)
static void	NoteOn(int channel, int prog, int note, int velocity)
{
	VagAtr	*tone;
	int		v, next = 0;

	while ((tone = FindTone(prog, PlayedNote(channel, note), next, &next)) != NULL)
	{
		if ((tone->vag < 1) || (tone->vag >= 256))
			continue;
		v = FreeVoice();
		SPU_KEY_OFF(v);
		voices[v].on = 1;
		voices[v].channel = channel;
		voices[v].note = note;
		voices[v].velocity = velocity;
		voices[v].prog = prog;
		voices[v].tone = tone;
		voices[v].age = ++voice_age;
		voices[v].bend = 0;
		VoiceVolume(v);
		VoicePitch(v);
		SPU_VOICE(v)[3] = vag_adrs[tone->vag] / 8;
		SPU_VOICE(v)[4] = tone->adsr1;
		SPU_VOICE(v)[5] = tone->adsr2;
		VoiceReverb(v, channel, tone->mode & 4);
		SPU_KEY_ON(v);
	}
}

static void	NoteOff(int channel, int note)		// note < 0: all the notes of the channel
{
	int	v;

	for (v = 0; v < VOICES - 1; v++)
		if (voices[v].on && (voices[v].channel == channel) && ((note < 0) || (voices[v].note == note)))
		{
			voices[v].on = 0;
			SPU_KEY_OFF(v);
		}
}

// The tester volume, also for the notes sounding (the VAB voices and the VAG voice): heard at once
static void	SetVolume(int volume)
{
	int	v;

	volume = Clamp(volume, 0, 127);
	if (volume == master_volume)
		return;
	master_volume = volume;
	for (v = 0; v < VOICES - 1; v++)
		if (vab && voices[v].tone && SPU_VOICE(v)[6])
			VoiceVolume(v);
	if (!vab && vag_data && SPU_VOICE(VAG_VOICE)[6])
		SPU_VOICE(VAG_VOICE)[0] = SPU_VOICE(VAG_VOICE)[1] = master_volume * 0x3FFF / 127;
}

// Stop all sounds: every voice silenced at once (volume 0, fastest release), not left to its release
static void	AllNotesOff(void)
{
	int	v;

	for (v = 0; v < VOICES; v++)
	{
		voices[v].on = 0;
		SPU_VOICE(v)[0] = SPU_VOICE(v)[1] = 0;
		SPU_VOICE(v)[5] &= ~0x1F;		// ADSR release rate: fastest
	}
	key_on = key_off_next = 0;
	key_off = (1 << VOICES) - 1;
}

/****************************************/
// SEQ music (Sony's File Formats): ID "SEQp" (also stored as "pQES": the same 32-bit ID written the other way round),
// version (32-bit, 1; some tools: 16-bit, 2 bytes less), resolution (ticks per quarter note), tempo (us per quarter
// note), rhythm, then MIDI events (delta time, status with running status), FF 2F 00 at the end. Sony's meta events
// have no length, unlike a MIDI file's: FF 51 and the 3 bytes of the tempo; the running status after one is a meta
// event (FF 51 ttt <delta> 51 ttt: two tempo changes). Loops (Run-Time Library Overview, table 15-4): NRPN 99 = 20
// loop start, its count from the data entry after it (CC 6, or CC 98 as the converted files have it): 127 for good,
// else the times the loop end jumps back; NRPN 99 = 30 loop end (without a count: read on). Several loops one after
// the other, not inside each other. At the end of the music: played again from its start (as SSPLAY_INFINITY).
// SEP: several SEQs in one file: ID, version (16-bit, 0), then for each one its number, resolution, tempo, rhythm,
// data size (32-bit) and data (up to 16).

#define	SEQ_MAX_TRACKS	16
#define	SEQ_FRAME_EVENTS	4000	// Events read in a frame at most (a damaged file: the frame still ends)

typedef struct
{
	uint8_t		*data, *end;		// Events (the first delta time) and their end
	int			resolution;
	uint32_t	tempo;
} SeqTrack;

static SeqTrack	seq_tracks[SEQ_MAX_TRACKS];
static int		seq_count, seq_index;
static int		seq_loop_count;		// Count of the last loop start: -1 none (its end read on), 127 for good
static int		seq_count_pending;	// A loop start just read: the next data entry is its count
static int		seq_loop_empty;		// Jumped back to a loop start, no time waited since
static uint8_t	*seq_data, *seq_ptr, *seq_end, *seq_loop_ptr;
static uint8_t	*seq_file;			// The SEQ file's address (as uploaded with the sounds)
static uint32_t	seq_size;
static uint16_t	seq_channels;		// Channels with notes
static int		seq_playing, seq_resolution, seq_status, seq_loop_status;
static uint32_t	seq_tempo, seq_wait, seq_acc, seq_ticks, seq_loops, seq_loop_ticks;
static uint32_t	seq_us, seq_loop_us, seq_len_us;	// Position (us, from the start), at the loop start, music length
static int		seq_paused;			// Paused: time stopped, notes released
static int		seq_seeking;		// Read through up to a time: no notes played, loop ends read on
static uint32_t	frame_us;

// Ticks to us and back at a tempo (us per quarter note) and resolution (ticks per quarter note), without overflow
static uint32_t	TicksUs(uint32_t ticks, uint32_t tempo, uint32_t res)
{
	return ((ticks / res) * tempo + (ticks % res) * tempo / res);
}

// The same for times added up event after event (a seek, the length): the fraction of a us left by each one carried
// to the next (*frac, in 1/res us). (Dropped at each event, a seek's time fell behind the music's: 20 ms after 4
// minutes of Alundra, the notes just after the time sought read with the seek, not played.)
static uint32_t	TicksUsFrac(uint32_t ticks, uint32_t tempo, uint32_t res, uint32_t *frac)
{
	uint32_t	part = (ticks % res) * tempo + *frac;

	*frac = part % res;
	return ((ticks / res) * tempo + part / res);
}

static uint32_t	UsTicks(uint32_t us, uint32_t tempo, uint32_t res)
{
	return ((us / tempo) * res + (us % tempo) * res / tempo);
}

// A variable-length number (delta time, meta event length): up to 4 bytes, not read past the data's end (a file cut
// short: 0 there, then the end)
static uint32_t	SeqVarLen(void)
{
	uint32_t	v = 0;
	uint8_t		b;
	int			n = 0;

	do
	{
		if (seq_ptr >= seq_end)
			return (v);
		b = *seq_ptr++;
		v = (v << 7) | (b & 0x7F);
	} while ((b & 0x80) && (++n < 4));
	return (v);
}

static void	SeqStart(void)
{
	int	i;

	for (i = 0; i < 16; i++)
	{
		channels[i].prog = 0;
		channels[i].volume = 127;
		channels[i].expression = 127;
		channels[i].pan = 64;
		channels[i].bend = 0;
		channels[i].sustain = 0;
	}
	seq_resolution = seq_tracks[seq_index].resolution;
	seq_tempo = seq_tracks[seq_index].tempo;
	if (!seq_resolution)
		seq_resolution = 480;
	if (!seq_tempo)
		seq_tempo = 500000;
	seq_ptr = seq_tracks[seq_index].data;
	seq_end = seq_tracks[seq_index].end;
	seq_loop_ptr = NULL;
	seq_loop_count = -1;
	seq_count_pending = 0;
	seq_loop_empty = 0;
	seq_status = 0;
	seq_acc = 0;
	seq_ticks = 0;			// (bars counted from the start of the music, at each loop too)
	seq_us = seq_loop_us = 0;
	seq_notes = 0;
	seq_wait = SeqVarLen();
}

static void	SeqStop(void)
{
	int	c;

	for (c = 0; c < 16; c++)
		NoteOff(c, -1);
	seq_playing = 0;
	seq_paused = 0;
}

// One event (after its delta time): returns 0 at the end of the music
static int	SeqEvent(void)
{
	uint8_t	status, d1, d2;
	int		c, v;

	// (a file cut short: the end where an event's bytes would go past it)
	if (seq_ptr >= seq_end)
		return (0);
	status = *seq_ptr;
	if (status & 0x80)
		seq_ptr++;
	else
		status = seq_status;		// Running status
	if (status == 0xFF)				// Meta event (Sony: no length byte)
	{
		uint8_t		type;

		if (seq_ptr >= seq_end)
			return (0);
		type = *seq_ptr++;
		seq_status = 0xFF;			// (running status: a meta event again)
		if (type == 0x2F)			// End of track
			return (0);
		if (type == 0x51)			// Tempo: 3 bytes, us per quarter note
		{
			if (seq_ptr + 3 > seq_end)
				return (0);
			seq_tempo = (seq_ptr[0] << 16) | (seq_ptr[1] << 8) | seq_ptr[2];
			if (!seq_tempo)
				seq_tempo = 500000;
			seq_ptr += 3;
			return (1);
		}
		seq_ptr += SeqVarLen();		// (other meta events: none in Sony's SEQ, a MIDI file's length taken)
		return (1);
	}
	if (seq_ptr + ((((status & 0xF0) == 0xC0) || ((status & 0xF0) == 0xD0)) ? 1 : 2) > seq_end)
		return (0);
	seq_status = status;
	c = status & 15;
	d1 = *seq_ptr++;
	switch (status & 0xF0)
	{
		case 0x80:	// Note off
			seq_ptr++;
			if (!channels[c].sustain)
				NoteOff(c, d1);
			break;
		case 0x90:	// Note on (velocity 0: off)
			d2 = *seq_ptr++;
			if (d2)
			{
				seq_notes++;
				if (seq_seeking)
					break;
				NoteOff(c, d1);
				if (!(fx_mute & (1 << c)))	// (muted channel: its notes not played)
					NoteOn(c, channels[c].prog, d1, d2);
			}
			else if (!channels[c].sustain)
				NoteOff(c, d1);
			break;
		case 0xB0:	// Controller
			d2 = *seq_ptr++;
			if ((d1 == 7) || (d1 == 10) || (d1 == 11))	// Volume, pan, expression: heard at once
			{
				if (d1 == 7)
					channels[c].volume = d2;
				else if (d1 == 11)
					channels[c].expression = d2;
				else
					channels[c].pan = d2;
				for (v = 0; v < VOICES - 1; v++)
					if (voices[v].on && (voices[v].channel == c))
						VoiceVolume(v);
			}
			else if (d1 == 64)	// Sustain pedal
			{
				channels[c].sustain = (d2 >= 64);
				if (!channels[c].sustain)
					NoteOff(c, -1);
			}
			else if ((d1 == 120) || (d1 == 123))	// All sounds / notes off
				NoteOff(c, -1);
			else if ((d1 == 99) && (d2 == 20))		// Loop start (the next delta time), its count next
			{
				seq_loop_ptr = seq_ptr;
				seq_loop_status = status;
				seq_loop_ticks = seq_ticks;
				seq_loop_us = seq_us;
				seq_loop_count = -1;
				seq_count_pending = 1;
			}
			else if (((d1 == 6) || (d1 == 98)) && seq_count_pending)	// The loop's count
			{
				seq_loop_count = d2;
				seq_count_pending = 0;
			}
			else if ((d1 == 99) && (d2 == 30) && seq_loop_ptr)	// Loop end: from the loop start again
			{
				seq_count_pending = 0;
				// (a loop without time inside it, its start and end at the same tick: read on after one jump back;
				// followed, the frame never ended, the tester and its USB link hung)
				if (!seq_seeking && !seq_loop_empty && ((seq_loop_count == 127) || (seq_loop_count > 0)))
				{
					if (seq_loop_count != 127)
						seq_loop_count--;
					seq_ptr = seq_loop_ptr;
					seq_status = seq_loop_status;
					seq_ticks = seq_loop_ticks;
					seq_us = seq_loop_us;
					seq_loops++;
					seq_loop_empty = 1;		// (until a wait is read)
				}
			}
			break;
		case 0xC0:	// Program change
			channels[c].prog = d1;
			break;
		case 0xE0:	// Pitch bend
			d2 = *seq_ptr++;
			channels[c].bend = ((d2 << 7) | d1) - 8192;
			for (v = 0; v < VOICES - 1; v++)	// (the notes sounding only: see VoicePitch)
				if (voices[v].on && (voices[v].channel == c))
				{
					voices[v].bend = channels[c].bend;
					VoicePitch(v);
				}
			break;
		case 0xA0:	// Key pressure (unused)
			seq_ptr++;
			break;
		case 0xD0:	// Channel pressure (unused)
			break;
		default:	// Unknown: end of the music
			return (0);
	}
	return (1);
}

// Once per frame: the ticks of a frame (tempo and resolution), the events that are due
static void	SeqUpdate(void)
{
	uint32_t	ticks;
	int			n, events;

	if (!seq_playing || seq_paused)
		return;
	seq_us += frame_us * fx_tempo / 100;						// (the music's time: tempo scaled)
	seq_acc += (frame_us * fx_tempo / 100) * seq_resolution;
	ticks = seq_acc / seq_tempo;
	seq_acc -= ticks * seq_tempo;
	seq_ticks += ticks;
	for (events = 0; seq_playing && (events < SEQ_FRAME_EVENTS); )	// (a damaged file: the frame still ends)
	{
		if (seq_wait > ticks)
		{
			seq_wait -= ticks;
			break;
		}
		ticks -= seq_wait;
		for (n = 0; ; n++, events++)		// Events at the same time
		{
			uint32_t	tempo = seq_tempo;

			if (!SeqEvent())
			{
				// The end: played again (until stopped, as SSPLAY_INFINITY), or stopped (once, as a game plays a
				// jingle: its code decides; the loops inside the music are played either way)
				SeqStop();
				if (!seq_repeat || !seq_notes)	// (a SEQ with no notes: not started again at every frame)
				{
					seq_playing = 0;
					return;
				}
				SeqStart();
				seq_playing = 1;
				seq_loops++;
				return;
			}
			// The rest of the frame's ticks: at the new tempo, not the one it started with. Their time back into
			// seq_acc (us * resolution: exactly what they were made from) and divided by the new tempo, the part of a
			// tick left over kept there. (Through us and back, each change lost up to a tick: Alundra 2's tempo
			// changes, played again at each loop, put notes 5-14 ms late after 2 minutes.)
			if (seq_tempo != tempo)
			{
				seq_acc += ticks * tempo;
				seq_ticks -= ticks;
				ticks = seq_acc / seq_tempo;
				seq_acc -= ticks * seq_tempo;
				seq_ticks += ticks;
			}
			seq_wait = SeqVarLen();
			if (seq_wait)
				seq_loop_empty = 0;
			if (seq_wait || (n > 1000))
				break;
		}
	}
}

// The channels with notes in the music and its length (read once, as SeqEvent without playing)
static void	SeqScan(void)
{
	uint8_t		*end = seq_tracks[seq_index].end, status = 0, s, type;
	uint32_t	res = seq_tracks[seq_index].resolution, tempo = seq_tracks[seq_index].tempo, frac = 0;
	int			n;

	if (!res)
		res = 480;
	if (!tempo)
		tempo = 500000;
	seq_channels = 0;
	seq_len_us = 0;
	seq_ptr = seq_tracks[seq_index].data;
	seq_end = end;					// (SeqVarLen's limit)
	for (n = 0; (n < 500000) && (seq_ptr + 4 <= end); n++)	// (4: a delta time and FF 2F 00 at least; "<" left a last wait out of the length)
	{
		seq_len_us += TicksUsFrac(SeqVarLen(), tempo, res, &frac);
		if (seq_ptr + 2 > end)		// (no event left: status and data, or meta type)
			break;
		s = *seq_ptr;
		if (s & 0x80)
			seq_ptr++;
		else
			s = status;
		if (s == 0xFF)
		{
			status = 0xFF;
			if ((type = *seq_ptr++) == 0x2F)
				break;
			if ((type == 0x51) && (seq_ptr + 3 > end))
				break;
			if ((type == 0x51) && (seq_ptr[0] | seq_ptr[1] | seq_ptr[2]))
				tempo = (seq_ptr[0] << 16) | (seq_ptr[1] << 8) | seq_ptr[2];
			seq_ptr += (type == 0x51) ? 3 : SeqVarLen();
			continue;
		}
		status = s;
		switch (s & 0xF0)
		{
			case 0x90:
				if ((seq_ptr + 2 <= end) && seq_ptr[1])
					seq_channels |= 1 << (s & 15);
				seq_ptr += 2;
				break;
			case 0x80:	case 0xA0:	case 0xB0:	case 0xE0:
				seq_ptr += 2;
				break;
			case 0xC0:	case 0xD0:
				seq_ptr++;
				break;
			default:
				return;
		}
	}
}

static uint32_t	BigEndian(uint8_t *p, int n)
{
	uint32_t	v = 0;

	while (n--)
		v = (v << 8) | *p++;
	return (v);
}

// The SEQs of the music file (a SEQ: one, a SEP: up to 16): their events, resolution and tempo; 0 if none
static int	SeqParse(uint8_t *file, uint32_t size)
{
	uint8_t		*end = file + size, *p;
	uint32_t	len;
	int			n = 0, head;

	if ((size < 16) || (memcmp(file, "SEQp", 4) && memcmp(file, "pQES", 4)))
		return (0);
	if ((BigEndian(file + 4, 4) == 1) || (BigEndian(file + 4, 2) == 1))	// A SEQ (32-bit version, or 16-bit)
	{
		head = (BigEndian(file + 4, 4) == 1) ? 4 : 2;
		seq_tracks[0].resolution = BigEndian(file + 4 + head, 2);
		seq_tracks[0].tempo = BigEndian(file + 6 + head, 3);
		seq_tracks[0].data = file + 11 + head;
		seq_tracks[0].end = end;
		return (1);
	}
	for (p = file + 6; (p + 13 <= end) && (n < SEQ_MAX_TRACKS); p += 13 + len)	// A SEP
	{
		len = BigEndian(p + 9, 4);
		if (p + 13 + len > end)
			len = end - p - 13;			// (cut short: up to the end of the file)
		seq_tracks[n].resolution = BigEndian(p + 2, 2);
		seq_tracks[n].tempo = BigEndian(p + 4, 3);
		seq_tracks[n].data = p + 13;
		seq_tracks[n].end = p + 13 + len;
		n++;
	}
	return (n);
}

// Another SEQ of a SEP chosen (played if the music was playing)
static void	SeqSelect(int index)
{
	int	was = seq_playing;

	if (!seq_data || (index < 0) || (index >= seq_count) || (index == seq_index))
		return;
	if (seq_playing)
		SeqStop();
	seq_index = index;
	SeqScan();
	SeqStart();
	seq_loops = 0;
	seq_playing = was;
}

// Played from a time (ms): read from the start up to it without notes (programs, volumes, pan, bends, tempo set as
// they would be), loop ends read on. Paused: stays paused there; stopped or playing: plays from there.
static void	SeqSeek(uint32_t ms)
{
	uint32_t	target = ms * 1000, step, ticks, frac = 0, f;
	int			paused = seq_paused;

	if (!seq_data)
		return;
	SeqStop();
	SeqStart();
	if (target >= seq_len_us)
		target = 0;
	seq_seeking = 1;
	for (;;)
	{
		f = frac;
		step = TicksUsFrac(seq_wait, seq_tempo, seq_resolution, &f);
		if (seq_us + step >= target)	// (the time is at or before the next event: its wait shortened, it is played)
		{
			ticks = UsTicks(target - seq_us, seq_tempo, seq_resolution);
			if (ticks > seq_wait)
				ticks = seq_wait;
			seq_wait -= ticks;
			seq_ticks += ticks;
			seq_us = target;
			break;
		}
		seq_us += step;
		frac = f;
		seq_ticks += seq_wait;
		if (!SeqEvent())
		{
			SeqStart();
			break;
		}
		seq_wait = SeqVarLen();
	}
	seq_seeking = 0;
	seq_playing = 1;
	seq_paused = paused;
}

// A new SEQ written by the computer (music tracker: the song edited while it plays), played from the same time
static void	SeqReload(int where, uint32_t size)
{
	uint8_t		*data = (where == 2) ? (uint8_t *)TESTER_SEQ_SPARE : seq_file;
	uint32_t	at = seq_us / 1000;
	int			was = seq_playing, paused = seq_paused, count;

	if (!seq_file || (size < 16) || ((where == 2) && (size > TESTER_SEQ_SPARE_SIZE)))
		return;
	if (!(count = SeqParse(data, size)))
		return;
	if (seq_playing)
		SeqStop();
	seq_data = data;
	seq_size = size;
	seq_count = count;
	if (seq_index >= seq_count)
		seq_index = 0;
	SeqScan();
	SeqStart();
	if (was)
	{
		SeqSeek(at);
		seq_paused = paused;
	}
}

static void	LoadSounds(TesterData *td);

// New instruments and music written by the computer (music tracker: an instrument changed while it plays): the file
// table and the files rewritten (ps1transfer paused the music first and waited for it), the bank uploaded to the SPU
// RAM again (every voice silenced first), the SEQ played from the same time. paused: stays paused there
static void	BankReload(SoundSync *live, int paused)
{
	uint32_t	at = seq_us / 1000;
	int			was = seq_playing, i;

	SeqStop();
	AllNotesOff();
	SpuKeys();
	memset(voices, 0, sizeof(voices));
	vab = NULL;
	vag_data = NULL;
	n_programs = 0;
	seq_data = seq_file = NULL;
	seq_count = 0;
	seq_len_us = 0;
	seq_channels = 0;
	quiet_load = 1;
	LoadSounds((TesterData *)TESTER_DATA_ADRS);
	quiet_load = 0;
	rev_active = ReverbSet(fx_reverb);	// (the samples may now reach the reverb work area: cleared, or no room)
	if (was && seq_data)
	{
		SeqSeek(at);
		seq_paused = paused;
	}
	// The shared state of the sound data
	live->n_programs = n_programs;
	memset(live->prog_map, 0, sizeof(live->prog_map));
	for (i = 0; i < n_programs; i++)
		live->prog_map[programs[i] >> 3] |= 1 << (programs[i] & 7);
	live->vab_ps = vab ? vab->ps : 0;
	live->vab_ts = vab ? vab->ts : 0;
	live->vab_vs = vab ? vab->vs : 0;
	live->vh_adrs = vab ? (uint32_t)vab : 0;
	live->vh_size = vab ? VhSize(vab) : 0;
	live->samples_end = samples_end;
}

static void	SeqPause(int on)
{
	int	c;

	if (!seq_playing || (on == seq_paused))
		return;
	if (on)
		for (c = 0; c < 16; c++)
			NoteOff(c, -1);
	seq_paused = on;
}

/****************************************/

static void	LoadSounds(TesterData *td)
{
	uint8_t	*vh = NULL, *vb = NULL, *seq = NULL;
	int		i;

	for (i = 0; (i < td->count) && (i < TESTER_MAX_FILES); i++)
	{
		uint8_t	*p = (uint8_t *)td->files[i].adrs;

		switch (td->files[i].type)
		{
			case FILE_VH:	vh = p;	break;
			case FILE_VB:	vb = p;	break;
			case FILE_VAB:	vh = p; vb = p + VhSize((VabHdr *)p);	break;
			case FILE_SEQ:	seq = p;	seq_size = td->files[i].size;	break;
			case FILE_VAG:	vag_data = p;	break;
		}
	}

	if (vh && vb && LoadVab(vh, vb))
	{
		if (seq && (seq_count = SeqParse(seq, seq_size)))
		{
			seq_data = seq_file = seq;
			seq_index = 0;
			SeqScan();
			SeqStart();
			if (!quiet_load)
				printf("SEQ: resolution %d, tempo %d us\n", seq_resolution, (int)seq_tempo);
		}
	}
	else if (vag_data)
	{
		// VAG header: "VAGp", version, reserved, data size (big endian), sample rate (big endian), ..., data at 48
		vag_rate = BigEndian32(vag_data + 16);
		SpuUpload(SPU_DATA_ADRS, vag_data + VAG_HEADER_SIZE, BigEndian32(vag_data + 12));
		samples_end = SPU_DATA_ADRS + BigEndian32(vag_data + 12);
		if (!quiet_load)
			printf("VAG: %d Hz, %d bytes\n", (int)vag_rate, (int)BigEndian32(vag_data + 12));
	}
}

static void	VagKeyOn(int note, int volume)
{
	volatile uint16_t	*v = SPU_VOICE(VAG_VOICE);

	v[0] = v[1] = volume * 0x3FFF / 127;		// Volume left/right
	v[2] = PitchCents(vag_rate, (note - 60) * 100);	// Original sample rate at C4
	v[3] = SPU_DATA_ADRS / 8;					// Start address
	v[4] = 0x000F;								// ADSR: fastest attack, sustain level max
	v[5] = 0x0008;								// ADSR: sustain held, short release
	VoiceReverb(VAG_VOICE, -1, 1);				// (no tone: reverb unless "no notes")
	SPU_KEY_ON(VAG_VOICE);
}

static void	VagKeyOff(void)
{
	SPU_KEY_OFF(VAG_VOICE);
}

// The shared state: what does not change (the sound data), the requests cleared, then the magic (the computer waits
// for it)
static void	SyncInit(SoundSync *live)
{
	int	i;

	SoundPos	*pos = (SoundPos *)TESTER_POS_ADRS;
	TesterData	*td = (TesterData *)TESTER_DATA_ADRS;

	memset(pos, 0, sizeof(SoundPos));
	pos->req_seek = -1;
	pos->req_pause = -1;
	pos->req_reload = -1;
	pos->len_ms = seq_len_us / 1000;
	pos->version = seq_file ? 3 : 1;
	memset(live, 0, sizeof(SoundSync));
	live->kind = vab ? 1 : (vag_data ? 2 : 0);
	live->n_programs = n_programs;
	live->vag_rate = vag_data ? vag_rate : 0;
	if (vab)
	{
		live->vab_ps = vab->ps;
		live->vab_ts = vab->ts;
		live->vab_vs = vab->vs;
	}
	for (i = 0; i < n_programs; i++)
		live->prog_map[programs[i] >> 3] |= 1 << (programs[i] & 7);
	if (vab)
	{
		live->vh_adrs = (uint32_t)vab;
		live->vh_size = VhSize(vab);
	}
	live->req_prog = live->req_note = live->req_volume = live->req_seq = live->req_stop = live->req_refresh = -1;
	live->req_fx = -1;
	live->req_sep = -1;
	live->req_repeat = -1;
	live->fx_depth = fx_depth;
	live->fx_tempo = fx_tempo;
	live->ch_used = seq_channels;
	live->samples_end = samples_end;
	// SoundFx2 after the header's SoundSync, if no file is there (an older ps1transfer's header ends at 0x100)
	fx2 = (SoundFx2 *)TESTER_FX2_ADRS;
	if (td->magic == TESTER_MAGIC)
		for (i = 0; (i < (int)td->count) && (i < TESTER_MAX_FILES); i++)
			if (td->files[i].adrs < TESTER_FX2_END)
				fx2 = NULL;
	if (fx2)
	{
		memset(fx2, 0, sizeof(SoundFx2));
		fx2->rev_delay = fx2->rev_feedback = -1;
		fx2->rev_channels = 0xFFFF;
		fx2->version = 1;
	}
	live->fx_version = fx2 ? 2 : 1;
	live->magic = TESTER_SYNC_MAGIC;
}

// The effects asked by the computer, heard at once on the notes sounding too
static void	ApplyFx(SoundSync *live)
{
	int			type = Clamp(live->fx_reverb, 0, REVERB_TYPES - 1), depth = Clamp(live->fx_depth, 0, 127);
	int			voices_mode = Clamp(live->fx_rev_voices, 0, fx2 ? 3 : 2), transpose = Clamp(live->fx_transpose, -24, 24);
	int			mono = live->fx_mono ? 1 : 0, v, c, delay = -1, feedback = -1;
	uint16_t	muted = live->fx_mute & ~fx_mute, rev_channels = 0xFFFF;

	if (fx2)
	{
		delay = (fx2->rev_delay < 0) ? -1 : Clamp(fx2->rev_delay, 0, 127);
		feedback = (fx2->rev_feedback < 0) ? -1 : Clamp(fx2->rev_feedback, 0, 127);
		rev_channels = fx2->rev_channels;
	}
	fx_tempo = Clamp(live->fx_tempo, 25, 400);
	if ((type != fx_reverb) || ((rev_active < 0) && type))
	{
		fx_depth = depth;
		fx_reverb = type;
		fx_delay = delay;
		fx_feedback = feedback;
		rev_active = ReverbSet(type);
	}
	else
	{
		if (depth != fx_depth)
		{
			fx_depth = depth;
			if (rev_active > 0)
				SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = fx_depth * 0x7FFF / 127;
		}
		if ((delay != fx_delay) || (feedback != fx_feedback))	// (as libspu: on the reverb running)
		{
			fx_delay = delay;
			fx_feedback = feedback;
			if (rev_active > 0)
				ReverbEcho(rev_active);
		}
	}
	if ((voices_mode != fx_rev_voices) || (rev_channels != fx_rev_channels))
	{
		fx_rev_voices = voices_mode;
		fx_rev_channels = rev_channels;
		for (v = 0; v < VOICES - 1; v++)
			if (voices[v].tone)
				VoiceReverb(v, voices[v].channel, voices[v].tone->mode & 4);
		VoiceReverb(VAG_VOICE, -1, 1);
	}
	fx_mute = live->fx_mute;
	for (c = 0; c < 16; c++)		// (newly muted: its notes released)
		if (muted & (1 << c))
			NoteOff(c, -1);
	if ((mono != fx_mono) || (transpose != fx_transpose))
	{
		fx_mono = mono;
		fx_transpose = transpose;
		for (v = 0; v < VOICES - 1; v++)
			if (vab && voices[v].tone && SPU_VOICE(v)[6])
			{
				VoiceVolume(v);
				VoicePitch(v);
			}
	}
}

static void	SeqPlay(int on)
{
	if (!seq_data || (on == seq_playing))
		return;
	if (on)
	{
		SeqStart();
		seq_loops = 0;
		seq_playing = 1;
	}
	else
		SeqStop();
}

// A request of the computer taken: its value, the field set back to -1 (nothing asked)
static int	Take(int16_t *field)
{
	volatile int16_t	*f = field;
	int					v = *f;

	if (v != -1)
		*f = -1;
	return (v);
}

int	main(void)
{
	TesterData	*td = (TesterData *)TESTER_DATA_ADRS;
	SoundSync	*live = (SoundSync *)TESTER_SYNC_ADRS;
	SoundPos	*pos = (SoundPos *)TESTER_POS_ADRS;
	uint32_t	pad, opad = 0, voice_bits;
	uint16_t	ch_active;
	int			prog_index = 0, note = 60, i, side = 0, is_pal, playing_note = 0, active;
	int			req_prog, req_note, req_volume, req_seq, req_stop, req_refresh, req_fx, req_sep, req_repeat, req_pause;
	int32_t		req_seek;

	*((volatile uint32_t *)REMOTE_PAD_ADRS) = 0;

	// Same video mode as the console region, like the PS1 DEV CART menu (BIOS version string: 'E' = Europe = PAL)
	is_pal = (*((char *)0xBFC7FF52) == 'E');
	frame_us = is_pal ? 20000 : 16683;
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	fnt_data = UiOpenText(8, 20, 304, 3);
	fnt_play = UiOpenText(8, 62, 304, 3);
	fnt_voices = UiOpenText(8, 104, 304, 1);

	SpuInit();
	SPU_REG(0x1F801D80) = 0x3FFF;	// Master volume
	SPU_REG(0x1F801D82) = 0x3FFF;
	if (td->magic == TESTER_MAGIC)
		LoadSounds(td);
	// No reverb until one is asked for: the one the program before left (on, its registers, its work area's echoes)
	// isn't heard on the first notes; no voice sends to it
	SPU_REG(0x1F801D98) = SPU_REG(0x1F801D9A) = 0;
	eon = 0;
	SPU_REG(0x1F801DAA) &= ~(1 << 7);
	SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = 0;
	SyncInit(live);

	SetDispMask(1);

	while (1)
	{
		// The reverb work area cleared at the start (the previous program's echoes), unless a reverb was chosen
		if (start_clear && !--start_clear && !fx_reverb)
			rev_active = ReverbSet(0);

		// Changes asked by the computer (before the controller: a note set then played from there is this note). Each
		// one taken (set back to -1) before any is applied: one the computer asks while they are (a reverb change
		// clears up to 96 KB of SPU RAM) is kept for the next frame (all set back to -1 after them, a Start asked
		// just after a reverb was lost)
		req_prog = Take(&live->req_prog);
		req_note = Take(&live->req_note);
		req_volume = Take(&live->req_volume);
		req_seq = Take(&live->req_seq);
		req_stop = Take(&live->req_stop);
		req_refresh = Take(&live->req_refresh);
		req_fx = Take(&live->req_fx);
		req_sep = Take(&live->req_sep);
		req_repeat = Take(&live->req_repeat);
		req_seek = pos->req_seek;
		pos->req_seek = -1;
		req_pause = Take(&pos->req_pause);
		if ((req_prog >= 0) && (req_prog < n_programs))
			prog_index = req_prog;
		if ((req_note >= 0) && (req_note <= 127))
			note = req_note;
		if ((req_volume >= 0) && (req_volume <= 127))
			SetVolume(req_volume);
		if (req_seq >= 0)
			SeqPlay(req_seq);
		if (req_stop > 0)
		{
			SeqStop();
			AllNotesOff();
		}
		if (req_refresh > 0)
			RefreshVoices();
		if (req_fx > 0)
			ApplyFx(live);
		if (req_sep >= 0)
			SeqSelect(req_sep);
		if (req_repeat >= 0)
			seq_repeat = req_repeat ? 1 : 0;
		// (req_reload set back once done: the computer waits for it)
		if (pos->req_reload > 0)		// (first: a seek asked with it is in the new SEQ)
		{
			if (pos->req_reload == 3)
			{
				BankReload(live, pos->reload_size & 1);
				if (prog_index >= n_programs)
					prog_index = 0;
			}
			else
				SeqReload(pos->req_reload, pos->reload_size);
			pos->req_reload = -1;
		}
		if (req_seek >= 0)
			SeqSeek(req_seek);
		if (req_pause >= 0)
			SeqPause(req_pause);
		pos->req_reload = -1;

		pad = (PadRead(1) & 0xFFFF) | *((volatile uint32_t *)REMOTE_PAD_ADRS);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))

		if (PRESSED(PADLright) && n_programs)	prog_index = (prog_index + 1) % n_programs;
		if (PRESSED(PADLleft) && n_programs)	prog_index = (prog_index + n_programs - 1) % n_programs;
		if (PRESSED(PADLup) && (note < 127))	note++;
		if (PRESSED(PADLdown) && (note > 0))	note--;
		if (PRESSED(PADR1))						note = (note + 12 > 127) ? 127 : note + 12;
		if (PRESSED(PADL1))						note = (note < 12) ? 0 : note - 12;
		if (pad & PADRup)		SetVolume(master_volume + 1);
		if (pad & PADRleft)		SetVolume(master_volume - 1);

		// Play while Cross is held
		if (PRESSED(PADRdown))
		{
			playing_note = note;
			if (vab && n_programs)
				NoteOn(-1, programs[prog_index], note, 127);
			else if (vag_data)
				VagKeyOn(note, master_volume);
		}
		if (!(pad & PADRdown) && (opad & PADRdown))
		{
			if (vab)
				NoteOff(-1, playing_note);
			else if (vag_data)
				VagKeyOff();
		}

		if (PRESSED(PADRright))
			SeqPlay(!seq_playing);
		if (PRESSED(PADselect) && (seq_count > 1))	// SEP: its next SEQ
			SeqSelect((seq_index + 1) % seq_count);
		if (PRESSED(PADstart))
		{
			SeqStop();
			AllNotesOff();
		}
		opad = pad;
		SeqUpdate();
		SpuKeys();

		// Screen (devcart_ui.h): sound data, note/volume, voices (lit while sounding), controls
		UiTitle("PS1 DEV CART SOUND TESTER", vab ? "VAB" : (vag_data ? "VAG" : "NO DATA"));
		UiTextPanel(8, 20, 304, 3);
		UiTextPanel(8, 62, 304, 3);
		UiTextPanel(8, 104, 304, 2);
		for (i = active = 0, voice_bits = 0, ch_active = 0; i < VOICES; i++)
		{
			int	on = (SPU_VOICE(i)[6] != 0);	// Envelope level: voice sounding

			active += on;
			voice_bits |= on << i;
			if (on && (i < VOICES - 1) && (voices[i].channel >= 0) && voices[i].tone)
				ch_active |= 1 << voices[i].channel;
			if (on)
				UiTile(16 + i * 12, 120, 10, 6, UI_OK);
			else
				UiTile(16 + i * 12, 120, 10, 6, UI_TRACK);
		}
		UiProgress(176, 77, 120, 6, master_volume, 127);
		if (vab)
		{
			FntPrint(fnt_data, "VAB  %d PROGRAMS %d TONES %d VAGS\n", vab->ps, vab->ts, vab->vs);
			if (seq_data && (seq_count > 1))	// (a SEP: its SEQ chosen, Select for the next one)
				FntPrint(fnt_data, "SEQ  %d/%d ", seq_index + 1, seq_count);
			else
				FntPrint(fnt_data, "SEQ  ");
			FntPrint(fnt_data, "%s%s", !seq_data ? "NONE\n" : (seq_playing ? (seq_paused ? "PAUSED" : "PLAYING") : "STOPPED"),
					 !seq_data ? "" : (seq_repeat ? (seq_playing ? "" : "\n") : (seq_playing ? " ONCE" : " ONCE\n")));
			if (seq_data && seq_playing)
				FntPrint(fnt_data, "  %d:%02d/%d:%02d  LOOP %d\n", (int)(seq_us / 60000000), (int)(seq_us / 1000000 % 60),
						 (int)(seq_len_us / 60000000), (int)(seq_len_us / 1000000 % 60), (int)seq_loops);
			FntPrint(fnt_data, "PROGRAM  %d  (%d/%d)", n_programs ? programs[prog_index] : -1, prog_index + 1, n_programs);
		}
		else if (vag_data)
			FntPrint(fnt_data, "VAG  %d HZ\n(C4 = ORIGINAL RATE)", (int)vag_rate);
		else
			FntPrint(fnt_data, "NO SOUND DATA. USE PS1TRANSFER -SND\nBANK.VH BANK.VB [MUSIC.SEQ]\nOR SAMPLE.VAG");
		FntPrint(fnt_play, "NOTE     %s%d  (%d)\n", note_names[note % 12], note / 12 - 1, note);
		FntPrint(fnt_play, "VOLUME   %d\n", master_volume);
		FntPrint(fnt_play, "PLAYING  %s", (pad & PADRdown) ? "YES" : "NO");
		FntPrint(fnt_voices, "VOICES   %d/%d  REVERB %s", active, VOICES,
				 (rev_active < 0) ? "NO ROOM" : reverb_types[rev_active].name);
		UiHelp("</>:PROGRAM UP/DOWN:NOTE L1/R1:OCTAVE\nX:PLAY O:SEQ TRI/SQR:VOLUME START:STOP");

		// The state shown here, for the computer
		live->prog_index = prog_index;
		live->program = n_programs ? programs[prog_index] : -1;
		live->note = note;
		live->volume = master_volume;
		live->playing = (pad & PADRdown) ? 1 : 0;
		live->seq = !seq_data ? 0 : (seq_playing ? 2 : 1);
		live->seq_bar = seq_playing ? (int)(seq_ticks / (seq_resolution * 4)) + 1 : 0;
		live->seq_loop = seq_loops;
		pos->pos_ms = seq_playing ? seq_us / 1000 : 0;
		pos->len_ms = seq_len_us / 1000;
		pos->paused = seq_paused;
		live->voices = voice_bits;
		live->rev_active = rev_active;
		live->ch_used = seq_channels;
		live->sep_count = seq_data ? seq_count : 0;
		live->sep_index = seq_index;
		live->ch_active = ch_active;
		for (i = 0; i < 16; i++)
			live->ch_prog[i] = channels[i].prog;
		live->frame++;

		DrawSync(0);
		VSync(0);
		side ^= 1;
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		UiFlush();
		FntFlush(fnt_data);
		FntFlush(fnt_play);
		FntFlush(fnt_voices);
	}

	return (0);
}
