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
*			Circle: SEQ start/stop, Triangle/Square: volume, Start: stop all sounds
* The buttons can also be pressed from the computer: PsyQ pad bits written to REMOTE_PAD_ADRS (e.g. with
* "ps1transfer -poke"), added to the controller ones.
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
#define	SPU_KEY_ON(_v_)		SPU_REG(((_v_) < 16) ? 0x1F801D88 : 0x1F801D8A) = 1 << ((_v_) & 15)
#define	SPU_KEY_OFF(_v_)	SPU_REG(((_v_) < 16) ? 0x1F801D8C : 0x1F801D8E) = 1 << ((_v_) & 15)

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
	pan = Clamp(vo->tone->pan + prog_atr[vo->prog].mpan + (c ? c->pan : 64) - 128, 0, 127);
	SPU_VOICE(v)[0] = (pan <= 64) ? vol : vol * (127 - pan) / 63;
	SPU_VOICE(v)[1] = (pan >= 64) ? vol : vol * pan / 64;
}

// Pitch: 44100 Hz at the tone's center note, its fine tune, pitch bend (tone range, 2 semitones if none)
static void	VoicePitch(int v)
{
	Voice	*vo = &voices[v];
	int		bend = (vo->channel >= 0) ? channels[vo->channel].bend : 0;
	int		range = vo->tone->pbmax ? vo->tone->pbmax : 2;
	int		cents = (vo->note - vo->tone->center) * 100 + vo->tone->shift * 100 / 128 + bend * range * 100 / 8192;

	SPU_VOICE(v)[2] = PitchCents(44100, cents);
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
	VagAtr	*tone = FindTone(prog, note);
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

static void	AllNotesOff(void)
{
	int	v;

	for (v = 0; v < VOICES; v++)
	{
		voices[v].on = 0;
		SPU_KEY_OFF(v);
	}
}

/****************************************/
// SEQ music: "pQES", version, resolution (ticks per quarter note), tempo (us per quarter note), rhythm, then MIDI events
// (delta time, status with running status, meta events FF 51 tempo / FF 2F end)

#define	SEQ_HEADER_SIZE	15

static uint8_t	*seq_data, *seq_ptr;
static int		seq_playing, seq_resolution, seq_status;
static uint32_t	seq_tempo, seq_wait, seq_acc, seq_ticks, seq_loops;
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
	seq_resolution = (seq_data[8] << 8) | seq_data[9];
	seq_tempo = (seq_data[10] << 16) | (seq_data[11] << 8) | seq_data[12];
	if (!seq_resolution)
		seq_resolution = 480;
	if (!seq_tempo)
		seq_tempo = 500000;
	seq_ptr = seq_data + SEQ_HEADER_SIZE;
	seq_status = 0;
	seq_acc = 0;
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
	uint8_t	status = *seq_ptr, d1, d2;
	int		c, v;

	if (status & 0x80)
		seq_ptr++;
	else
		status = seq_status;		// Running status
	if (status == 0xFF)				// Meta event
	{
		uint8_t		type = *seq_ptr++;
		uint32_t	len = SeqVarLen();

		if (type == 0x2F)			// End of track
			return (0);
		if ((type == 0x51) && (len == 3))
			seq_tempo = (seq_ptr[0] << 16) | (seq_ptr[1] << 8) | seq_ptr[2];
		seq_ptr += len;
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
	seq_acc += frame_us * seq_resolution;
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
			case FILE_SEQ:	seq = p;	break;
			case FILE_VAG:	vag_data = p;	break;
		}
	}

	if (vh && vb && LoadVab(vh, vb))
	{
		if (seq && !memcmp(seq, "pQES", 4))
		{
			seq_data = seq;
			SeqStart();
			printf("SEQ: resolution %d, tempo %d us\n", seq_resolution, (int)seq_tempo);
		}
	}
	else if (vag_data)
	{
		// VAG header: "VAGp", version, reserved, data size (big endian), sample rate (big endian), ..., data at 48
		vag_rate = BigEndian32(vag_data + 16);
		SpuUpload(SPU_DATA_ADRS, vag_data + VAG_HEADER_SIZE, BigEndian32(vag_data + 12));
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
	SPU_KEY_ON(VAG_VOICE);
}

static void	VagKeyOff(void)
{
	SPU_KEY_OFF(VAG_VOICE);
}

int	main(void)
{
	TesterData	*td = (TesterData *)TESTER_DATA_ADRS;
	uint32_t	pad, opad = 0;
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

	SetDispMask(1);

	while (1)
	{
		pad = (PadRead(1) & 0xFFFF) | *((volatile uint32_t *)REMOTE_PAD_ADRS);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))

		if (PRESSED(PADLright) && n_programs)	prog_index = (prog_index + 1) % n_programs;
		if (PRESSED(PADLleft) && n_programs)	prog_index = (prog_index + n_programs - 1) % n_programs;
		if (PRESSED(PADLup) && (note < 127))	note++;
		if (PRESSED(PADLdown) && (note > 0))	note--;
		if (PRESSED(PADR1))						note = (note + 12 > 127) ? 127 : note + 12;
		if (PRESSED(PADL1))						note = (note < 12) ? 0 : note - 12;
		if ((pad & PADRup) && (master_volume < 127))	master_volume++;
		if ((pad & PADRleft) && (master_volume > 0))	master_volume--;

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

		if (PRESSED(PADRright) && seq_data)
		{
			if (seq_playing)
				SeqStop();
			else
			{
				SeqStart();
				seq_playing = 1;
			}
		}
		if (PRESSED(PADstart))
		{
			SeqStop();
			AllNotesOff();
		}
		opad = pad;
		SeqUpdate();

		// Screen (devcart_ui.h): sound data, note/volume, voices (lit while sounding), controls
		UiTitle("PS1 DEV CART SOUND TESTER", vab ? "VAB" : (vag_data ? "VAG" : "NO DATA"));
		UiTextPanel(8, 20, 304, 3);
		UiTextPanel(8, 62, 304, 3);
		UiTextPanel(8, 104, 304, 2);
		for (i = active = 0; i < VOICES; i++)
		{
			int	on = (SPU_VOICE(i)[6] != 0);	// Envelope level: voice sounding

			active += on;
			if (on)
				UiTile(16 + i * 12, 120, 10, 6, UI_OK);
			else
				UiTile(16 + i * 12, 120, 10, 6, UI_TRACK);
		}
		UiProgress(176, 77, 120, 6, master_volume, 127);
		if (vab)
		{
			FntPrint(fnt_data, "VAB  %d PROGRAMS %d TONES %d VAGS\n", vab->ps, vab->ts, vab->vs);
			FntPrint(fnt_data, "SEQ  %s", !seq_data ? "NONE\n" : (seq_playing ? "PLAYING" : "STOPPED\n"));
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
		FntPrint(fnt_voices, "VOICES   %d/%d", active, VOICES);
		UiHelp("</>:PROGRAM UP/DOWN:NOTE L1/R1:OCTAVE\nX:PLAY O:SEQ TRI/SQR:VOLUME START:STOP");

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
