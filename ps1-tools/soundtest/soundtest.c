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
* reverb, all or none), SEQ transpose, tempo and channels muted, mono.
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
	int16_t		fx_rev_voices;		// Notes with reverb: 0 as the bank's tones (mode 4), 1 all, 2 none
	int16_t		fx_transpose;		// SEQ: semitones (-24..24)
	int16_t		fx_tempo;			// SEQ: tempo in percent (25-400)
	int16_t		fx_mono;			// 1: mono (every note at the center)
	uint16_t	fx_mute;			// SEQ: channels muted (bit n: channel n)
	// Written by the tester
	int16_t		rev_active;			// Reverb type in use, 0: off, -1: not enough SPU RAM (the bank uses it)
	uint16_t	ch_used;			// SEQ channels with notes (bit n: channel n)
	uint16_t	ch_active;			// SEQ channels with notes sounding
	uint8_t		ch_prog[16];		// Program of each SEQ channel
	uint16_t	fx_version;			// 1: the effects above (0: an older tester)
	uint32_t	samples_end;		// SPU RAM used by the samples (from 0 to this address)
	int16_t		req_sep;			// SEQ of a SEP (multi-track file) played: its index, -1: nothing
	int16_t		sep_count;			// SEQs in the music file (1: a SEQ, more: a SEP)
	int16_t		sep_index;			// The one chosen
} SoundSync;

// (offsets used by ps1transfer/sndlive.c: SYNC_)
#define	SYNC_CHECK(_n_, _c_)	typedef char sync_check_##_n_[(_c_) ? 1 : -1]
SYNC_CHECK(fx, __builtin_offsetof(SoundSync, req_fx) == 76);
SYNC_CHECK(chprog, __builtin_offsetof(SoundSync, ch_prog) == 98);
SYNC_CHECK(version, __builtin_offsetof(SoundSync, fx_version) == 114);
SYNC_CHECK(sep, __builtin_offsetof(SoundSync, req_sep) == 120);
SYNC_CHECK(size, sizeof(SoundSync) == 128);	// (the data block header's free end: 0x80 bytes)

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
static int		fx_reverb, fx_depth = 64, fx_rev_voices, fx_transpose, fx_tempo = 100, fx_mono;
static uint16_t	fx_mute;
static int		rev_active;				// Reverb type in use, -1: not enough SPU RAM
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

static void	SpuUpload(uint32_t adrs, uint8_t *data, uint32_t size)
{
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(adrs);
	SpuWrite((const uint32_t *)data, (size + 63) & ~63);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
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
	printf("VAB: %d programs, %d tones, %d VAGs, %d bytes in SPU RAM\n", vab->ps, vab->ts, vab->vs, (int)vb_size);
	return (1);
}

// Tone of a program for a note (tones have a note range), NULL if none
static VagAtr	*FindTone(int prog, int note)
{
	VagAtr	*t;
	int		i;

	if (!vab || (prog < 0) || (prog > 127) || (prog_block[prog] < 0))
		return (NULL);
	t = tone_atr + prog_block[prog] * 16;
	for (i = 0; i < prog_atr[prog].tones; i++)
		if ((note >= t[i].min) && (note <= t[i].max))
			return (&t[i]);
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
} Voice;

typedef struct
{
	int		prog, volume, pan, bend, sustain;
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

// Pitch: 44100 Hz at the tone's center note, its fine tune, pitch bend (tone range, 2 semitones if none)
static void	VoicePitch(int v)
{
	Voice	*vo = &voices[v];
	int		bend = (vo->channel >= 0) ? channels[vo->channel].bend : 0;
	int		range = vo->tone->pbmax ? vo->tone->pbmax : 2;
	int		cents = (PlayedNote(vo->channel, vo->note) - vo->tone->center) * 100 + vo->tone->shift * 100 / 128 +
					bend * range * 100 / 8192;

	SPU_VOICE(v)[2] = PitchCents(44100, cents);
}

// Reverb of a voice: as its tone (VAB tone mode 4: reverb, as libsnd), all or no notes
static void	VoiceReverb(int v, int bank_reverb)
{
	int	on = (fx_rev_voices == 1) || ((fx_rev_voices == 0) && bank_reverb);

	eon = on ? (eon | (1 << v)) : (eon & ~(1 << v));
	SPU_REG(0x1F801D98) = eon;
	SPU_REG(0x1F801D9A) = eon >> 16;
}

// Reverb type: the work area at the top of SPU RAM (zeroed: no old data heard), above the samples, else none (-1)
static int	ReverbSet(int type)
{
	static const uint32_t	zero[256];
	uint32_t				base, adrs;
	int						i;

	SPU_REG(0x1F801DAA) &= ~(1 << 7);	// Reverb master off (also while its work area is cleared)
	SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = 0;
	if ((type <= 0) || (type >= REVERB_TYPES))
		return (0);
	base = (0x80000 - reverb_types[type].size) & ~1023;	// (zeroed in whole DMA blocks)
	if (base < samples_end)
		return (-1);
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	for (adrs = base; adrs < 0x80000; adrs += sizeof(zero))
	{
		SpuSetTransferStartAddr(adrs);
		SpuWrite(zero, sizeof(zero));
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	}
	for (i = 0; i < 32; i++)
		SPU_REG(0x1F801DC0 + i * 2) = reverb_types[type].regs[i];
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

static void	NoteOn(int channel, int prog, int note, int velocity)
{
	VagAtr	*tone = FindTone(prog, PlayedNote(channel, note));
	int		v;

	if (!tone || (tone->vag < 1) || (tone->vag >= 256))
		return;
	v = FreeVoice();
	SPU_KEY_OFF(v);
	voices[v].on = 1;
	voices[v].channel = channel;
	voices[v].note = note;
	voices[v].velocity = velocity;
	voices[v].prog = prog;
	voices[v].tone = tone;
	voices[v].age = ++voice_age;
	VoiceVolume(v);
	VoicePitch(v);
	SPU_VOICE(v)[3] = vag_adrs[tone->vag] / 8;
	SPU_VOICE(v)[4] = tone->adsr1;
	SPU_VOICE(v)[5] = tone->adsr2;
	VoiceReverb(v, tone->mode & 4);
	SPU_KEY_ON(v);
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
static uint8_t	*seq_data, *seq_ptr, *seq_end, *seq_loop_ptr;
static uint32_t	seq_size;
static uint16_t	seq_channels;		// Channels with notes
static int		seq_playing, seq_resolution, seq_status, seq_loop_status;
static uint32_t	seq_tempo, seq_wait, seq_acc, seq_ticks, seq_loops, seq_loop_ticks;
static uint32_t	frame_us;

static uint32_t	SeqVarLen(void)
{
	uint32_t	v = 0;
	uint8_t		b;

	do
	{
		b = *seq_ptr++;
		v = (v << 7) | (b & 0x7F);
	} while (b & 0x80);
	return (v);
}

static void	SeqStart(void)
{
	int	i;

	for (i = 0; i < 16; i++)
	{
		channels[i].prog = 0;
		channels[i].volume = 127;
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
	seq_status = 0;
	seq_acc = 0;
	seq_ticks = 0;			// (bars counted from the start of the music, at each loop too)
	seq_wait = SeqVarLen();
}

static void	SeqStop(void)
{
	int	c;

	for (c = 0; c < 16; c++)
		NoteOff(c, -1);
	seq_playing = 0;
}

// One event (after its delta time): returns 0 at the end of the music
static int	SeqEvent(void)
{
	uint8_t	status, d1, d2;
	int		c, v;

	if (seq_ptr + 1 >= seq_end)		// (a file cut short)
		return (0);
	status = *seq_ptr;
	if (status & 0x80)
		seq_ptr++;
	else
		status = seq_status;		// Running status
	if (status == 0xFF)				// Meta event (Sony: no length byte)
	{
		uint8_t		type = *seq_ptr++;

		seq_status = 0xFF;			// (running status: a meta event again)
		if (type == 0x2F)			// End of track
			return (0);
		if (type == 0x51)			// Tempo: 3 bytes, us per quarter note
		{
			seq_tempo = (seq_ptr[0] << 16) | (seq_ptr[1] << 8) | seq_ptr[2];
			if (!seq_tempo)
				seq_tempo = 500000;
			seq_ptr += 3;
			return (1);
		}
		seq_ptr += SeqVarLen();		// (other meta events: none in Sony's SEQ, a MIDI file's length taken)
		return (1);
	}
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
				NoteOff(c, d1);
				if (!(fx_mute & (1 << c)))	// (muted channel: its notes not played)
					NoteOn(c, channels[c].prog, d1, d2);
			}
			else if (!channels[c].sustain)
				NoteOff(c, d1);
			break;
		case 0xB0:	// Controller
			d2 = *seq_ptr++;
			if ((d1 == 7) || (d1 == 10))
			{
				if (d1 == 7)
					channels[c].volume = d2;
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
				if ((seq_loop_count == 127) || (seq_loop_count > 0))
				{
					if (seq_loop_count != 127)
						seq_loop_count--;
					seq_ptr = seq_loop_ptr;
					seq_status = seq_loop_status;
					seq_ticks = seq_loop_ticks;
					seq_loops++;
				}
			}
			break;
		case 0xC0:	// Program change
			channels[c].prog = d1;
			break;
		case 0xE0:	// Pitch bend
			d2 = *seq_ptr++;
			channels[c].bend = ((d2 << 7) | d1) - 8192;
			for (v = 0; v < VOICES - 1; v++)
				if (voices[v].on && (voices[v].channel == c))
					VoicePitch(v);
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
	int			n;

	if (!seq_playing)
		return;
	seq_acc += (frame_us * fx_tempo / 100) * seq_resolution;	// (tempo scaled)
	ticks = seq_acc / seq_tempo;
	seq_acc -= ticks * seq_tempo;
	seq_ticks += ticks;
	while (seq_playing)
	{
		if (seq_wait > ticks)
		{
			seq_wait -= ticks;
			break;
		}
		ticks -= seq_wait;
		for (n = 0; ; n++)		// Events at the same time
		{
			if (!SeqEvent())
			{
				// Loop (played until stopped, as SSPLAY_INFINITY)
				SeqStop();
				SeqStart();
				seq_playing = 1;
				seq_loops++;
				return;
			}
			seq_wait = SeqVarLen();
			if (seq_wait || (n > 1000))
				break;
		}
	}
}

// The channels with notes in the music (read once, as SeqEvent without playing)
static void	SeqScan(void)
{
	uint8_t	*end = seq_tracks[seq_index].end, status = 0, s, type;
	int		n;

	seq_channels = 0;
	seq_ptr = seq_tracks[seq_index].data;
	for (n = 0; (n < 500000) && (seq_ptr + 4 < end); n++)
	{
		SeqVarLen();
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
			seq_ptr += (type == 0x51) ? 3 : SeqVarLen();
			continue;
		}
		status = s;
		switch (s & 0xF0)
		{
			case 0x90:
				if (seq_ptr[1])
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
			seq_data = seq;
			seq_index = 0;
			SeqScan();
			SeqStart();
			printf("SEQ: resolution %d, tempo %d us\n", seq_resolution, (int)seq_tempo);
		}
	}
	else if (vag_data)
	{
		// VAG header: "VAGp", version, reserved, data size (big endian), sample rate (big endian), ..., data at 48
		vag_rate = BigEndian32(vag_data + 16);
		SpuUpload(SPU_DATA_ADRS, vag_data + VAG_HEADER_SIZE, BigEndian32(vag_data + 12));
		samples_end = SPU_DATA_ADRS + BigEndian32(vag_data + 12);
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
	VoiceReverb(VAG_VOICE, 1);					// (no tone: reverb unless "no notes")
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
	live->fx_depth = fx_depth;
	live->fx_tempo = fx_tempo;
	live->ch_used = seq_channels;
	live->samples_end = samples_end;
	live->fx_version = 1;
	live->magic = TESTER_SYNC_MAGIC;
}

// The effects asked by the computer, heard at once on the notes sounding too
static void	ApplyFx(SoundSync *live)
{
	int			type = Clamp(live->fx_reverb, 0, REVERB_TYPES - 1), depth = Clamp(live->fx_depth, 0, 127);
	int			voices_mode = Clamp(live->fx_rev_voices, 0, 2), transpose = Clamp(live->fx_transpose, -24, 24);
	int			mono = live->fx_mono ? 1 : 0, v, c;
	uint16_t	muted = live->fx_mute & ~fx_mute;

	fx_tempo = Clamp(live->fx_tempo, 25, 400);
	if ((type != fx_reverb) || ((rev_active < 0) && type))
	{
		fx_depth = depth;
		fx_reverb = type;
		rev_active = ReverbSet(type);
	}
	else if (depth != fx_depth)
	{
		fx_depth = depth;
		if (rev_active > 0)
			SPU_REG(0x1F801D84) = SPU_REG(0x1F801D86) = fx_depth * 0x7FFF / 127;
	}
	if (voices_mode != fx_rev_voices)
	{
		fx_rev_voices = voices_mode;
		for (v = 0; v < VOICES - 1; v++)
			if (voices[v].tone)
				VoiceReverb(v, voices[v].tone->mode & 4);
		VoiceReverb(VAG_VOICE, 1);
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

int	main(void)
{
	TesterData	*td = (TesterData *)TESTER_DATA_ADRS;
	SoundSync	*live = (SoundSync *)TESTER_SYNC_ADRS;
	uint32_t	pad, opad = 0, voice_bits;
	uint16_t	ch_active;
	int			prog_index = 0, note = 60, i, side = 0, is_pal, playing_note = 0, active;

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
	SyncInit(live);

	SetDispMask(1);

	while (1)
	{
		// Changes asked by the computer (before the controller: a note set then played from there is this note)
		if ((live->req_prog >= 0) && (live->req_prog < n_programs))
			prog_index = live->req_prog;
		if ((live->req_note >= 0) && (live->req_note <= 127))
			note = live->req_note;
		if ((live->req_volume >= 0) && (live->req_volume <= 127))
			SetVolume(live->req_volume);
		if (live->req_seq >= 0)
			SeqPlay(live->req_seq);
		if (live->req_stop > 0)
		{
			SeqStop();
			AllNotesOff();
		}
		if (live->req_refresh > 0)
			RefreshVoices();
		if (live->req_fx > 0)
			ApplyFx(live);
		if (live->req_sep >= 0)
			SeqSelect(live->req_sep);
		live->req_fx = live->req_sep = -1;
		live->req_prog = live->req_note = live->req_volume = live->req_seq = live->req_stop = live->req_refresh = -1;

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
			FntPrint(fnt_data, "%s", !seq_data ? "NONE\n" : (seq_playing ? "PLAYING" : "STOPPED\n"));
			if (seq_data && seq_playing)
				FntPrint(fnt_data, "  BAR %d  LOOP %d\n", (int)(seq_ticks / (seq_resolution * 4)) + 1, (int)seq_loops);
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
