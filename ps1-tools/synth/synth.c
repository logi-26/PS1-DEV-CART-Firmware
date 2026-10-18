/*
* PS1 DEV CART SPU synth: plays samples from MIDI sent by the computer ("ps1transfer -synth", or the GUI Synth page)
*
* - 16 sample slots, slot n plays the notes of MIDI channel n. Samples (VAG ADPCM) are sent by the computer at any
*   time (staging area in RAM, then copied to the SPU RAM).
* - 24 voices: note on/off with velocity, pitch bend (+/-2 semitones), sustain pedal (CC64), channel volume (CC7),
*   pan (CC10), all notes off (CC123). The oldest voice is reused when all are busy.
* - Per slot: root note (the note played at the sample rate), volume, pan, ADSR envelope, reverb, fine tune.
* - Screen: slots with their activity, the parameters of the selected slot, SPU RAM, keyboard of the notes playing.
* - Pad: Up/Down slot, L1/R1 parameter, Left/Right value, Cross/Triangle/Square: play the root note / octave up /
*   octave down, Start: all notes off.
* The kernel hook's USB service is called between the frames, so the computer is answered quickly (MIDI latency about
* 1-2 ms instead of a frame), and the MIDI ring is read continuously.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
// Built with PSn00bSDK (open source) instead of PsyQ
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxspu.h>
#include "psyq_compat.h"
#include "hook_usb.h"
#include "synth.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	OT_LEN			4
#define	PRIM_SIZE		(32 * 1024)
#define	VOICES			24

// SPU registers (voices driven directly: pitch, envelope and volume per note)
#define	SPU_VOICE(_v_)	((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))	// volL, volR, pitch, start, adsr1, adsr2, envx
#define	SPU_REG(_a_)	(*((volatile uint16_t *)(_a_)))
#define	SPU_KEY_ON(_v_)		SPU_REG(((_v_) < 16) ? 0x1F801D88 : 0x1F801D8A) = 1 << ((_v_) & 15)
#define	SPU_KEY_OFF(_v_)	SPU_REG(((_v_) < 16) ? 0x1F801D8C : 0x1F801D8E) = 1 << ((_v_) & 15)
#define	SPU_ENDX(_v_)		(SPU_REG(((_v_) < 16) ? 0x1F801D9C : 0x1F801D9E) & (1 << ((_v_) & 15)))
#define	SPU_EON(_v_)		SPU_REG(((_v_) < 16) ? 0x1F801D98 : 0x1F801D9A)	// Reverb on, bit (_v_) & 15

// SPU RAM: samples after the capture buffers and the first block (as libspu's allocator), reverb work area at the top
#define	SPU_SAMPLES_START	0x1010
#define	SPU_REVERB_SIZE		0xADE0		// Hall
#define	SPU_SAMPLES_END		(0x80000 - SPU_REVERB_SIZE)

// Hall reverb (the registers libspu's SPU_REV_MODE_HALL sets, read on the console): dAPF1/2, vIIR, vCOMB1-4, vWALL,
// vAPF1/2, mLSAME..mRAPF2, vLIN/vRIN at 0x1F801DC0
static const uint16_t	reverb_hall[32] = {
	0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD,
	0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A,
	0x0274, 0x013A, 0x8000, 0x8000};

#define	MB				((SynthMailbox *)SYNTH_MAILBOX_ADRS)
#define	VOLATILE32(_x_)	(*((volatile uint32_t *)&(_x_)))

enum	{VOICE_FREE, VOICE_ON, VOICE_RELEASED};

typedef struct
{
	int		state;
	int		channel;	// 0-15 = slot
	int		note;
	int		velocity;
	int		sustained;	// Note off received while the sustain pedal is down
	uint32_t	age;
	int		start;		// VSync count at key on (ENDX is only cleared once the SPU has run the key on)
} Voice;

typedef struct
{
	int		volume;		// CC7
	int		pan;		// CC10
	int		bend;		// -8192..8191
	int		sustain;	// CC64 down
} Channel;

static Voice		voices[VOICES];
static Channel		channels[SYNTH_SLOTS];
static uint32_t		note_age;

static const uint8_t	default_param[SYNTH_PARAMS] = {60, 100, 64, 0, 0, 99, 20, 0, 50};
static const char		*param_names[SYNTH_PARAMS] = {"ROOT", "VOLUME", "PAN", "ATTACK", "DECAY", "SUSTAIN",
														"RELEASE", "REVERB", "TUNE"};
static const char		*note_names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
// 2^(i/12) * 4096
static const uint16_t	semitones[12] = {4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732};

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][OT_LEN];
static uint8_t		prims[2][PRIM_SIZE];
static uint8_t		*prim_ptr;

/****************************************/
// Voices

static int	Clamp(int v, int min, int max)
{
	return ((v < min) ? min : ((v > max) ? max : v));
}

// SPU pitch: sample rate / 44100 * 4096 at the root note, cents from the note, fine tune and pitch bend
static int	Pitch(SynthSlot *s, int note, int bend)
{
	int	cents = (note - s->param[SYNTH_ROOT]) * 100 + (s->param[SYNTH_TUNE] - 50) + bend * 200 / 8192;
	int	octave = (cents >= 0) ? cents / 1200 : -((-cents + 1199) / 1200);
	int	rest = cents - octave * 1200;
	int	pitch = (int)(s->rate * 4096 / 44100) * semitones[rest / 100] >> 12;

	pitch = pitch * (100000 + (rest % 100) * 58) / 100000;	// 2^(c/1200) ~ 1 + c * 0.000578
	pitch = (octave >= 0) ? pitch << octave : pitch >> -octave;
	return (Clamp(pitch, 1, 0x3FFF));
}

static void	VoiceVolume(int v)
{
	Voice		*vo = &voices[v];
	SynthSlot	*s = &MB->slots[vo->channel];
	Channel		*c = &channels[vo->channel];
	int			vol = s->param[SYNTH_VOLUME] * vo->velocity * c->volume / 127;	// 0..16129
	int			pan = Clamp(s->param[SYNTH_PAN] + c->pan - 64, 0, 127);

	SPU_VOICE(v)[0] = Clamp(vol * (127 - pan) / 64, 0, 0x3FFF);
	SPU_VOICE(v)[1] = Clamp(vol * pan / 64, 0, 0x3FFF);
}

// ADSR registers from the slot parameters (0-99): linear attack, exponential decay/release, sustain held
static void	VoiceEnvelope(int v, SynthSlot *s)
{
	int	attack = s->param[SYNTH_ATTACK] * 127 / 99, decay = s->param[SYNTH_DECAY] * 15 / 99;
	int	sustain = s->param[SYNTH_SUSTAIN] * 15 / 99, release = s->param[SYNTH_RELEASE] * 31 / 99;

	SPU_VOICE(v)[4] = ((attack >> 2) << 10) | ((attack & 3) << 8) | (decay << 4) | sustain;	// Rate: shift*4 + step
	SPU_VOICE(v)[5] = (1 << 14) | (31 << 8) | (3 << 6) | (1 << 5) | release;	// Sustain: slowest decrease
}

static void	NoteOff(int v)
{
	SPU_KEY_OFF(v);
	voices[v].state = VOICE_RELEASED;
}

static void	NoteOn(int channel, int note, int velocity)
{
	SynthSlot	*s = &MB->slots[channel];
	int			v, best = -1;

	if (!s->spu_adrs)
		return;
	for (v = 0; v < VOICES; v++)	// Same note playing on the channel: restarted
		if ((voices[v].state != VOICE_FREE) && (voices[v].channel == channel) && (voices[v].note == note))
			best = v;
	for (v = 0; (best < 0) && (v < VOICES); v++)
		if (voices[v].state == VOICE_FREE)
			best = v;
	for (v = 0; (best < 0) && (v < VOICES); v++)	// Oldest released voice, else oldest voice
		if ((voices[v].state == VOICE_RELEASED) && ((best < 0) || (voices[v].age < voices[best].age)))
			best = v;
	if (best < 0)
		for (v = best = 0; v < VOICES; v++)
			if (voices[v].age < voices[best].age)
				best = v;

	voices[best].state = VOICE_ON;
	voices[best].channel = channel;
	voices[best].note = note;
	voices[best].velocity = velocity;
	voices[best].sustained = 0;
	voices[best].age = ++note_age;
	voices[best].start = VSync(-1);
	SPU_KEY_OFF(best);
	VoiceVolume(best);
	SPU_VOICE(best)[2] = Pitch(s, note, channels[channel].bend);
	SPU_VOICE(best)[3] = s->spu_adrs >> 3;
	VoiceEnvelope(best, s);
	if (s->param[SYNTH_REVERB])
		SPU_EON(best) |= 1 << (best & 15);
	else
		SPU_EON(best) &= ~(1 << (best & 15));
	SPU_KEY_ON(best);
}

static void	ChannelNotesOff(int channel, int note)		// note < 0: all the notes of the channel
{
	int	v;

	for (v = 0; v < VOICES; v++)
		if ((voices[v].state == VOICE_ON) && (voices[v].channel == channel) && ((note < 0) || (voices[v].note == note)))
		{
			if (channels[channel].sustain && (note >= 0))
				voices[v].sustained = 1;
			else
				NoteOff(v);
		}
}

static void	AllNotesOff(void)
{
	int	v;

	for (v = 0; v < VOICES; v++)
		if (voices[v].state != VOICE_FREE)
			NoteOff(v);
}

// Voices ended: sample end (no loop) or release finished
static void	UpdateVoices(void)
{
	int		v, i, now = VSync(-1);
	uint8_t	playing[SYNTH_SLOTS];	// Counted first: the computer may read the mailbox at any time

	memset(playing, 0, sizeof(playing));
	for (v = 0; v < VOICES; v++)
	{
		if (voices[v].state == VOICE_FREE)
			continue;
		// (a looping sample sets ENDX at each loop: only the end of the release counts)
		if ((now - voices[v].start > 1) && ((SPU_ENDX(v) && !MB->slots[voices[v].channel].loop) ||
			((voices[v].state == VOICE_RELEASED) && (SPU_VOICE(v)[6] == 0))))
		{
			voices[v].state = VOICE_FREE;
			continue;
		}
		playing[voices[v].channel]++;
	}
	for (i = 0; i < SYNTH_SLOTS; i++)
		MB->slots[i].playing = playing[i];
}

static void	Midi(uint8_t *m)
{
	int	channel = m[0] & 15, v;

	MB->midi_count++;
	switch (m[0] & 0xF0)
	{
		case 0x90:
			if (m[2])
			{
				NoteOn(channel, m[1] & 127, m[2] & 127);
				break;
			}
			// Velocity 0: note off
		case 0x80:
			ChannelNotesOff(channel, m[1] & 127);
			break;
		case 0xB0:
			switch (m[1])
			{
				case 7:		channels[channel].volume = m[2] & 127;	break;
				case 10:	channels[channel].pan = m[2] & 127;		break;
				case 64:
					channels[channel].sustain = (m[2] >= 64);
					if (!channels[channel].sustain)	// Pedal up: sustained notes released
						for (v = 0; v < VOICES; v++)
							if ((voices[v].state == VOICE_ON) && (voices[v].channel == channel) && voices[v].sustained)
								NoteOff(v);
					break;
				case 120:	// All sound off
				case 123:	// All notes off
					ChannelNotesOff(channel, -1);
					break;
			}
			for (v = 0; v < VOICES; v++)
				if ((voices[v].state != VOICE_FREE) && (voices[v].channel == channel))
					VoiceVolume(v);
			break;
		case 0xE0:
			channels[channel].bend = ((m[2] & 127) << 7 | (m[1] & 127)) - 8192;
			for (v = 0; v < VOICES; v++)
				if ((voices[v].state != VOICE_FREE) && (voices[v].channel == channel))
					SPU_VOICE(v)[2] = Pitch(&MB->slots[channel], voices[v].note, channels[channel].bend);
			break;
	}
}

static void	ProcessMidi(void)
{
	uint32_t	head = VOLATILE32(MB->midi_head);

	while (MB->midi_tail != head)
	{
		Midi(MB->midi[MB->midi_tail & (SYNTH_MIDI_RING - 1)]);
		MB->midi_tail++;
	}
}

/****************************************/
// Slots and commands

static void	DefaultChannels(void)
{
	int	i;

	for (i = 0; i < SYNTH_SLOTS; i++)
	{
		channels[i].volume = 127;
		channels[i].pan = 64;
		channels[i].bend = 0;
		channels[i].sustain = 0;
	}
}

// SPU RAM for a sample: first gap big enough between the samples of the other slots (-1: no room)
static int	SpuAlloc(uint32_t size)
{
	uint32_t	adrs = SPU_SAMPLES_START, end;
	int			i, moved;

	do
	{
		moved = 0;
		end = adrs + size;
		for (i = 0; i < SYNTH_SLOTS; i++)
		{
			SynthSlot	*o = &MB->slots[i];

			if (o->spu_adrs && (adrs < o->spu_adrs + o->size) && (o->spu_adrs < end))
			{
				adrs = o->spu_adrs + o->size;	// After this sample, then check all the slots again
				moved = 1;
				break;
			}
		}
	} while (moved && (adrs + size <= SPU_SAMPLES_END));
	return ((adrs + size <= SPU_SAMPLES_END) ? (int)adrs : -1);
}

static void	ClearSlot(int i)
{
	SynthSlot	*s = &MB->slots[i];
	int			v;

	for (v = 0; v < VOICES; v++)	// Voices still reading the sample
		if ((voices[v].state != VOICE_FREE) && (voices[v].channel == i))
		{
			SPU_KEY_OFF(v);
			voices[v].state = VOICE_FREE;
		}
	if (s->spu_adrs)
		MB->spu_free += s->size;
	s->spu_adrs = 0;
	s->size = 0;
	s->name[0] = 0;
}

static int	LoadSlot(int slot, uint32_t size, uint32_t rate, char *name)
{
	SynthSlot	*s = &MB->slots[slot];
	long		adrs;
	uint32_t	i;

	if (!size || (size > SYNTH_STAGING_SIZE))
		return (SYNTH_ERR_PARAM);
	ClearSlot(slot);
	if ((adrs = SpuAlloc((size + 63) & ~63)) < 0)
		return (SYNTH_ERR_MEMORY);
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(adrs);
	SpuWrite((const uint32_t *)SYNTH_STAGING_ADRS, (size + 63) & ~63);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	s->spu_adrs = adrs;
	s->size = (size + 63) & ~63;
	s->loop = 0;
	for (i = 0; i + 16 <= size; i += 16)	// ADPCM block flags: bit 1 = repeat (loop)
		if (((uint8_t *)SYNTH_STAGING_ADRS)[i + 1] & 2)
			s->loop = 1;
	s->rate = rate ? rate : 44100;
	memcpy(s->name, name, SYNTH_NAME_LEN);
	s->name[SYNTH_NAME_LEN - 1] = 0;
	MB->spu_free -= s->size;
	return (SYNTH_OK);
}

static void	ResetSlots(void)
{
	int	i;

	AllNotesOff();
	for (i = 0; i < SYNTH_SLOTS; i++)
	{
		ClearSlot(i);
		memcpy(MB->slots[i].param, default_param, SYNTH_PARAMS);
	}
	DefaultChannels();
}

static void	ProcessCommand(void)
{
	uint32_t	seq = VOLATILE32(MB->cmd_seq);
	int			slot = MB->slot, result = SYNTH_OK;

	if (seq == MB->done_seq)
		return;
	if ((MB->cmd != SYNTH_CMD_RESET) && (slot >= SYNTH_SLOTS))
		result = SYNTH_ERR_SLOT;
	else switch (MB->cmd)
	{
		case SYNTH_CMD_LOAD:
			result = LoadSlot(slot, MB->size, MB->rate, MB->name);
			break;
		case SYNTH_CMD_CLEAR:
			ClearSlot(slot);
			break;
		case SYNTH_CMD_SET:
			if (MB->param >= SYNTH_PARAMS)
				result = SYNTH_ERR_PARAM;
			else
				MB->slots[slot].param[MB->param] = MB->value;
			break;
		case SYNTH_CMD_RESET:
			ResetSlots();
			break;
		default:
			result = SYNTH_ERR_PARAM;
	}
	MB->result = result;
	MB->done_seq = seq;
}

/****************************************/
// Screen

static void	Tile(int x, int y, int w, int h, int r, int g, int b, int layer)
{
	TILE	*t = (TILE *)prim_ptr;

	prim_ptr += sizeof(TILE);
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(&ot[0][0] + layer, t);
}

static const char	*NoteName(int note, char *buf)
{
	sprintf(buf, "%s%d", note_names[note % 12], note / 12 - 1);
	return (buf);
}

static int	KeyPlaying(int note)
{
	int	v;

	for (v = 0; v < VOICES; v++)
		if ((voices[v].state == VOICE_ON) && (voices[v].note == note))
			return (1);
	return (0);
}

// Text streams (FntOpen ids): title, slot list, parameters, help line
static int	fnt_title, fnt_slots, fnt_params, fnt_help, fnt_cur;

static void	DrawScreen(int side, int sel, int param, int preview)
{
	static const int	white_keys[7] = {0, 2, 4, 5, 7, 9, 11};
	char		line[64], buf[8], value[12];
	int			i, x, n, used, on;
	SynthSlot	*s;

	ClearOTagR(ot[side], OT_LEN);
	prim_ptr = prims[side];
	// Layer 0 is drawn last (front), OT_LEN-1 first (back)
	#define	T(_x_, _y_, _w_, _h_, _r_, _g_, _b_, _l_)	Tile(_x_, _y_, _w_, _h_, _r_, _g_, _b_, side * OT_LEN + (_l_))

	T(0, 0, SCREEN_W, 14, 40, 50, 120, 2);					// Title bar
	T(6, 26 + sel * 8, 186, 8, 50, 70, 130, 2);				// Selected slot (8 pixels per text line)
	T(198, 18, 116, 148, 20, 24, 40, 3);					// Parameter panel

	fnt_cur = fnt_title;
	FntPrint(fnt_cur, "PS1 DEV CART SPU SYNTH     MIDI %6d", MB->midi_count % 1000000);

	fnt_cur = fnt_slots;
	FntPrint(fnt_cur, "CH SAMPLE        SIZE\n");
	for (i = 0; i < SYNTH_SLOTS; i++)
	{
		s = &MB->slots[i];
		if (s->spu_adrs)
		{
			// (no precision in the formats: PSn00bSDK's sprintf reads "%-12.12s" as a width of 1212, it overflowed
			// the line on the stack up into the kernel RAM: the name is cut here)
			char	name[13];

			snprintf(name, sizeof(name), "%s", s->name);
			sprintf(line, "%02d %-12s%4dK\n", i + 1, name, (int)((s->size + 1023) / 1024));
		}
		else
			sprintf(line, "%02d ---\n", i + 1);
		FntPrint(fnt_cur, "%s", line);
		if (s->playing)		// Activity
			T(184, 27 + i * 8, 6, 6, 80, 255, 80, 1);
	}

	// Parameters of the selected slot, SPU RAM
	s = &MB->slots[sel];
	fnt_cur = fnt_params;
	sprintf(line, "SLOT %02d  CH %d\n\n", sel + 1, sel + 1);
	FntPrint(fnt_cur, "%s", line);
	for (i = 0; i < SYNTH_PARAMS; i++)
	{
		n = s->param[i];
		if (i == SYNTH_ROOT)
			NoteName(n, value);
		else if (i == SYNTH_REVERB)
			strcpy(value, n ? "ON" : "OFF");
		else if (i == SYNTH_TUNE)
			sprintf(value, "%+d", n - 50);
		else
			sprintf(value, "%d", n);
		sprintf(line, "%c%-8s%s\n", (i == param) ? '>' : ' ', param_names[i], value);
		FntPrint(fnt_cur, "%s", line);
	}
	sprintf(line, "\n RATE %d\n %s\n\nSPU FREE %dK", (int)s->rate, s->loop ? "LOOP" : "", (int)(MB->spu_free / 1024));
	FntPrint(fnt_cur, "%s", line);
	used = MB->spu_total - MB->spu_free;
	T(202, 156, 108, 6, 60, 60, 60, 2);
	T(202, 156, MB->spu_total ? 108 * used / MB->spu_total : 0, 6, 80, 160, 255, 1);

	// Keyboard: C2..B5 (4 octaves, 28 white keys), notes playing in colour
	for (i = 0, x = 6; i < 28; i++, x += 11)
	{
		n = 36 + (i / 7) * 12 + white_keys[i % 7];
		on = KeyPlaying(n);
		T(x, 186, 10, 46, on ? 255 : 230, on ? 200 : 230, on ? 60 : 230, 2);
		if ((i % 7 != 2) && (i % 7 != 6))	// Black key after this white key
		{
			on = KeyPlaying(n + 1);
			T(x + 7, 186, 7, 28, on ? 255 : 30, on ? 160 : 30, on ? 40 : 40, 1);
		}
	}
	fnt_cur = fnt_help;
	sprintf(line, "X:%s TRI/SQR:OCTAVE L1/R1/<>:EDIT", NoteName(preview, buf));
	FntPrint(fnt_cur, "%s", line);
	#undef T
}

/****************************************/

int	main(void)
{
	int		side = 0, i, sel = 0, param = 0, is_pal, held = 0, preview = -1;
	uint32_t	pad, opad = 0, frame = 0;
	long	adrs;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, (i ^ 1) * SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i * SCREEN_H, SCREEN_W, SCREEN_H);
		draw[i].isbg = 1;
		setRGB0(&draw[i], 14, 16, 28);
	}
	FntLoad(960, 256);
	fnt_title = FntOpen(8, 4, 304, 8, 0, 48);
	fnt_slots = FntOpen(8, 18, 184, 140, 0, 400);
	fnt_params = FntOpen(204, 24, 108, 130, 0, 256);
	fnt_help = FntOpen(8, 172, 304, 8, 0, 48);
	SetDispMask(1);

	// SPU: voices driven directly, sample memory in SPU RAM between SPU_SAMPLES_START and the reverb work area, reverb
	// (hall) for the slots that use it
	SpuInit();
	{
		static const uint32_t	zero[256];

		SPU_REG(0x1F801DAA) &= ~(1 << 7);			// Reverb master off while its work area is cleared
		for (adrs = SPU_SAMPLES_END & ~1023; adrs < 0x80000; adrs += sizeof(zero))	// (whole DMA blocks)
		{
			SpuSetTransferStartAddr(adrs);
			SpuWrite(zero, sizeof(zero));
			SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
		}
		for (i = 0; i < 32; i++)
			SPU_REG(0x1F801DC0 + i * 2) = reverb_hall[i];
		SPU_REG(0x1F801DA2) = SPU_SAMPLES_END / 8;	// Work area address
		SPU_REG(0x1F801D84) = 0x2000;				// Reverb output volume (libspu left it at 0 here: no reverb heard)
		SPU_REG(0x1F801D86) = 0x2000;
		SPU_REG(0x1F801D98) = 0;					// No voice in the reverb yet
		SPU_REG(0x1F801D9A) = 0;
		SPU_REG(0x1F801DAA) |= 1 << 7;				// Reverb master on
	}
	SPU_REG(0x1F801D80) = 0x3FFF;					// Master volume
	SPU_REG(0x1F801D82) = 0x3FFF;

	// Mailbox: SPU RAM available for samples
	memset(MB, 0, sizeof(SynthMailbox));
	MB->spu_total = MB->spu_free = (SPU_SAMPLES_END - SPU_SAMPLES_START) & ~0x3FF;
	ResetSlots();
	MB->version = SYNTH_VERSION;
	MB->magic = SYNTH_MAGIC;
	printf("PS1 DEV CART SPU synth: %dK of SPU RAM for samples\n", (int)(MB->spu_total / 1024));

	while (1)
	{
		// MIDI and commands between the frames, the computer is answered meanwhile (see hook_usb.h)
		HookUsbPoll();
		ProcessCommand();
		ProcessMidi();
		UpdateVoices();
		if (VSync(-1) == frame)
			continue;
		frame = VSync(-1);

		// Pad (once per frame)
		pad = PadRead(1);
		#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		if (PRESSED(PADLup))	sel = (sel + SYNTH_SLOTS - 1) % SYNTH_SLOTS;
		if (PRESSED(PADLdown))	sel = (sel + 1) % SYNTH_SLOTS;
		if (PRESSED(PADL1))		param = (param + SYNTH_PARAMS - 1) % SYNTH_PARAMS;
		if (PRESSED(PADR1))		param = (param + 1) % SYNTH_PARAMS;
		held = (pad & (PADLleft | PADLright)) ? held + 1 : 0;
		if ((held == 1) || ((held > 20) && !(held & 3)))
		{
			static const uint8_t	max[SYNTH_PARAMS] = {127, 127, 127, 99, 99, 99, 99, 1, 100};
			uint8_t					*p = &MB->slots[sel].param[param];

			*p = Clamp(*p + ((pad & PADLright) ? 1 : -1), 0, max[param]);
		}
		// Preview notes on the selected slot (as MIDI on its channel)
		if (preview < 0)
		{
			int	root = MB->slots[sel].param[SYNTH_ROOT];

			preview = PRESSED(PADRdown) ? root : (PRESSED(PADRup) ? root + 12 : (PRESSED(PADRleft) ? root - 12 : -1));
			if (preview >= 0)
				NoteOn(sel, Clamp(preview, 0, 127), 100);
		}
		else if (!(pad & (PADRdown | PADRup | PADRleft)))
		{
			ChannelNotesOff(sel, Clamp(preview, 0, 127));
			preview = -1;
		}
		if (PRESSED(PADstart))
			AllNotesOff();
		opad = pad;
		#undef PRESSED

		side ^= 1;
		DrawScreen(side, sel, param, (preview >= 0) ? preview : MB->slots[sel].param[SYNTH_ROOT]);
		DrawSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][OT_LEN - 1]);
		FntFlush(fnt_title);
		FntFlush(fnt_slots);
		FntFlush(fnt_params);
		FntFlush(fnt_help);
	}
	return (0);
}
