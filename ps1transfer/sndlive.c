/*
* PS1 DEV CART Transfer tool: live link of the sound tester with the PS1 Dev Cart Manager ("-snd file ... -sndlive")
*
* The sound tester (ps1-tools/soundtest) shares its state in RAM (SoundSync in soundtest.c): program, note, volume, SEQ,
* the voices sounding, written at each frame. The computer changes them with requests the tester applies at its next
* frame, and plays the note with Cross through the tester's remote pad word (as if held on the controller). So the
* controls of the computer and the controller act on the same state, each one shows what the other does.
*
* Commands, one per line on the standard input:
*	program I		the program of index I (0 = first program with tones, as Left/Right on the controller)
*	note N			the note (0-127, 60 = C4)
*	volume V		the tester volume (0-127), also for the notes sounding
*	play [N]		note N (or the current one) played until "release" (Cross held)
*	release
*	seq 0/1			SEQ music stopped / started
*	sep N			the SEQ N (0...) of a SEP (several SEQs in one file)
*	repeat 0/1		at the end of the music: stopped (0: played once, as a game plays a jingle) or played again (1)
*	seek MS			SEQ music played from this time (milliseconds; paused: stays paused there)
*	pause 0/1		SEQ music resumed / paused
*	seqload FILE	a new SEQ played from the same time (music tracker: the song changed while it plays): "SNDSEQ
*					SIZE" when sent (the SEQ must be the last of the -snd files)
*	bankload BASE	new instruments and music (BASE.vh, BASE.vb, BASE.seq: the sound files replaced, the music played
*					from the same time, paused if it was): "SNDBANK SIZE" when sent
*	stop			all sounds stopped (Start)
*	vabwrite OFFSET HEX	VAB editor: the bytes written in the VH the tester plays (OFFSET in the VH, decimal; HEX:
*					the bytes), applied at once to the notes sounding too: "SNDWROTE OFFSET SIZE"
*	vabread			the VH as the tester has it: "SNDVH HEX"
*	fx REVERB DEPTH VOICES TRANSPOSE TEMPO MONO MUTE [DELAY FEEDBACK REV_CHANNELS]	effects: reverb type (0 off,
*					1 room, 2-4 studio small/medium/large, 5 hall, 6 half echo, 7 space echo, 8 chaos echo, 9 delay),
*					depth 0-127, notes with reverb (0 as the bank's tones, 1 all, 2 none, 3 the SEQ channels in
*					REV_CHANNELS), SEQ transpose (semitones), SEQ tempo (percent), mono 0/1, SEQ channels muted (bit n:
*					channel n); chaos echo and delay: libspu's delay time and feedback 0-127 (-1: libspu's default),
*					REV_CHANNELS: bit n = SEQ channel n (SNDFX version 2)
*	spuregs			the SPU registers (0x1F801C00-0x1F801DFF, read by the debug monitor version 10): "SNDSPU HEX"
*	quit			the link ended: the music stopped, every sound silenced first (as stop)
* Answers (start with "SND"): at the start "SNDLIVE ready", "SNDINFO KIND PROGRAMS VAB_PS VAB_TS VAB_VS VAG_RATE" (KIND 0:
* no sound data, 1: VAB, 2: VAG), "SNDPROGS P0 P1 ..." (program numbers, in index order), "SNDVAB ADDR SIZE" (the VH in
* RAM, VAB editor; 0 0: not editable, an older tester); then
* "SNDSTATE PROG_INDEX PROGRAM NOTE VOLUME PLAYING SEQ BAR LOOP VOICES" each time it changes (SEQ 0: none, 1: stopped,
* 2: playing; VOICES: hexadecimal, bit n = voice n sounding). "SNDFX VERSION SAMPLES_END" at the start (VERSION 0: an
* older tester, no effects; SAMPLES_END: SPU RAM used by the samples, hexadecimal), then
* "SNDCHAN REVERB CH_USED CH_ACTIVE P0 ... P15" each time it changes (REVERB: type in use, -1 not enough SPU RAM;
* CH_USED/CH_ACTIVE: hexadecimal, bit n = SEQ channel n with notes / sounding; Pn: program of channel n), then
* SEP_INDEX SEP_COUNT (the SEQ played, the SEQs in the music file: 1 for a SEQ, 0 for none).
* "SNDPOS POS_MS LEN_MS PAUSED" each time it changes (by 100 ms; a tester with seek and pause only): the SEQ position
* (0 when stopped), its length, 1 when paused. "SNDLIVE disconnected" (then the end) when the cartridge
* stops answering. Errors: "SNDERR text". The tester's printf text goes on meanwhile.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timlive.h"
#include "../monitor/monitor.h"

#define	TESTER_SYNC_ADRS	0x80100080	// SoundSync in soundtest.c (end of the data block header)
#define	TESTER_SYNC_MAGIC	0x534E4453
#define	REMOTE_PAD_ADRS		0x800FFFF0	// Buttons pressed from the computer (PsyQ pad bits)
#define	PAD_CROSS			(1 << 6)

// SoundSync offsets
#define	SYNC_KIND			4
#define	SYNC_N_PROGRAMS		6
#define	SYNC_STATE			8		// prog_index ... seq_loop: 8 int16, then voices (uint32)
#define	SYNC_VOICES			24
#define	SYNC_VAG_RATE		28
#define	SYNC_VAB			32		// ps, ts, vs
#define	SYNC_PROG_MAP		40
#define	SYNC_REQ_PROG		56
#define	SYNC_REQ_NOTE		58
#define	SYNC_REQ_VOLUME		60
#define	SYNC_REQ_SEQ		62
#define	SYNC_REQ_STOP		64
#define	SYNC_REQ_REFRESH	66
#define	SYNC_VH_ADRS		68		// VH in RAM, its size (VAB editor)
#define	SYNC_VH_SIZE		72
#define	SYNC_REQ_FX			76		// Effects (fx_ values from 78, 7 int16)
#define	SYNC_FX				78
#define	SYNC_REV_ACTIVE		92		// rev_active, ch_used, ch_active, ch_prog[16]
#define	SYNC_CH_USED		94
#define	SYNC_CH_ACTIVE		96
#define	SYNC_CH_PROG		98
#define	SYNC_FX_VERSION		114
#define	SYNC_SAMPLES_END	116
#define	SYNC_REQ_SEP		120		// SEP: the SEQ asked for, then sep_count, sep_index
#define	SYNC_SEP_COUNT		122
#define	SYNC_SEP_INDEX		124
#define	SYNC_REQ_REPEAT		126		// At the end of the music: 1 played again, 0 stopped
#define	SYNC_SIZE			128
#define	TESTER_POS_ADRS		0x80100068	// SoundPos in soundtest.c (before SoundSync)
#define	POS_MS				0
#define	POS_LEN_MS			4
#define	POS_REQ_SEEK		8
#define	POS_REQ_PAUSE		12
#define	POS_PAUSED			14
#define	POS_VERSION			16
#define	POS_REQ_RELOAD		18		// 1: the SEQ at its file's address, 2: at TESTER_SEQ_SPARE
#define	POS_RELOAD_SIZE		20
#define	TESTER_DATA_ADRS	0x80100000
#define	TESTER_FILE_TABLE	8			// TesterData: magic, count, then type, address, size of each file
#define	TESTER_MAX_FILES	8
#define	TESTER_FILE_SEQ		4
#define	TESTER_FILE_MAGIC	0x54444E53	// "SNDT" (main.c: TESTER_MAGIC)
#define	TESTER_HEADER_SIZE	0x120		// The files after the header (file table, SoundPos, SoundSync, SoundFx2)
#define	TESTER_FX2_ADRS		0x80100100	// SoundFx2 in soundtest.c: version, rev_delay, rev_feedback, rev_channels
#define	TESTER_SEQ_SPARE	0x801C0000	// soundtest.c: the second place of a reloaded SEQ
#define	TESTER_SEQ_SPARE_SIZE	0x20000
#define	POS_SIZE			(TESTER_SYNC_ADRS - TESTER_POS_ADRS)
#define	MAX_HEX_BYTES		0x11000	// vabwrite, vabread: a whole VH (128 programs: 68128 bytes)

#define	POLL_MS				50		// The state read (and printed when it changes) 20 times a second
#define	RELEASE_MS			40		// A release seen by the tester (2 frames) before the next note is played
#define	USB_LOST			3		// Failures in a row: "SNDLIVE disconnected", the link ends

static int		usb_failures;
static bool		pressed;			// Cross held from here
static uint32_t	vh_adrs, vh_size;	// The VH the tester plays (VAB editor)
static int		fx_version;			// SNDFX: the tester's effects (2: also SoundFx2)

static uint32_t	Get32(const uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}
static int16_t	Get16(const uint8_t *p)	{return ((int16_t)(p[0] | (p[1] << 8)));}

static bool	Usb(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	bool	ok;

	static const int	wait_ms[] = {20, 60, 150};
	int					i;

	// Tried again: a transfer now and then fails, and the tester doesn't answer while it is busy (sounds loaded in
	// the SPU RAM); writing again is harmless. (The answer of a transfer given up may come late: drained.)
	LiveDrain();
	ok = USB_Process(cmd, data, adrs, size);
	for (i = 0; !ok && (i < (int)(sizeof(wait_ms) / sizeof(wait_ms[0]))); i++)
	{
		LiveSleepMs(wait_ms[i]);
		LiveDrain();
		ok = USB_Process(cmd, data, adrs, size);
	}
	usb_failures = ok ? 0 : usb_failures + 1;
	return (ok);
}

// A request of the computer (applied by the tester at its next frame)
static void	Request(int offset, int value)
{
	int16_t	v = value;

	if (!Usb('U', (uint8_t *)&v, TESTER_SYNC_ADRS + offset, sizeof(v)))
		printf("SNDERR USB error\n");
}

// A request of the position block (seek: 32-bit, pause: 16-bit)
static void	PosRequest(int offset, int value, int size)
{
	int32_t	v32 = value;
	int16_t	v16 = value;

	if (!Usb('U', (size == 4) ? (uint8_t *)&v32 : (uint8_t *)&v16, TESTER_POS_ADRS + offset, size))
		printf("SNDERR USB error\n");
}

// seqload FILE: the new SEQ written where the tester doesn't read (its file's place and the spare one in turn), then
// the tester asked to play it from the same time
static int	slot = 1;			// Where the tester plays the SEQ from (seqload; bankload: its file again)

static void	SeqLoad(const char *path)
{
	uint8_t		table[TESTER_FILE_TABLE + TESTER_MAX_FILES * 12], pos[POS_SIZE], *data;
	uint32_t	seq_adrs = 0, seq_size = 0, end = 0, adrs, size, room;
	int			i, count, next = (slot == 1) ? 2 : 1;
	long		length;
	FILE		*f;

	if (!Usb('D', pos, TESTER_POS_ADRS, sizeof(pos)) || (Get16(pos + POS_VERSION) < 2) ||
		!Usb('D', table, TESTER_DATA_ADRS, sizeof(table)))
	{
		printf("SNDERR this sound tester can't reload a SEQ (rebuild soundtest.ps-exe)\n");
		return;
	}
	count = Get32(table + 4);
	for (i = 0; (i < count) && (i < TESTER_MAX_FILES); i++)
	{
		adrs = Get32(table + TESTER_FILE_TABLE + i * 12 + 4);
		size = Get32(table + TESTER_FILE_TABLE + i * 12 + 8);
		if (Get32(table + TESTER_FILE_TABLE + i * 12) == TESTER_FILE_SEQ)
		{
			seq_adrs = adrs;
			seq_size = size;
		}
		if (adrs + size > end)
			end = adrs + size;
	}
	if (!seq_adrs || (end > TESTER_SEQ_SPARE))
	{
		printf("SNDERR %s\n", !seq_adrs ? "no SEQ was sent with the sounds" : "the sound files leave no room for a new SEQ");
		return;
	}
	if (!(f = fopen(path, "rb")))
	{
		printf("SNDERR can't open %s\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, 0, SEEK_SET);
	room = (next == 2) ? TESTER_SEQ_SPARE_SIZE : ((seq_adrs + seq_size == end) ? TESTER_SEQ_SPARE - seq_adrs : seq_size);
	if ((length < 16) || ((uint32_t)length > room) || !(data = malloc(length)))
	{
		printf("SNDERR the SEQ is too big (%ld bytes, %u here)\n", length, room);
		fclose(f);
		return;
	}
	if (fread(data, 1, length, f) != (size_t)length)
	{
		printf("SNDERR can't read %s\n", path);
		fclose(f);
		free(data);
		return;
	}
	fclose(f);
	adrs = (next == 2) ? TESTER_SEQ_SPARE : seq_adrs;
	size = length;
	if (Usb('U', data, adrs, size) && Usb('U', (uint8_t *)&size, TESTER_POS_ADRS + POS_RELOAD_SIZE, 4))
	{
		PosRequest(POS_REQ_RELOAD, next, 2);
		slot = next;
		printf("SNDSEQ %u\n", size);
	}
	else
		printf("SNDERR USB error\n");
	free(data);
}

// A whole file read (NULL: printed why)
static uint8_t	*ReadWhole(const char *path, uint32_t *size)
{
	uint8_t	*data = NULL;
	long	length;
	FILE	*f;

	if (!(f = fopen(path, "rb")))
	{
		printf("SNDERR can't open %s\n", path);
		return (NULL);
	}
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, 0, SEEK_SET);
	if ((length <= 0) || !(data = malloc(length)) || (fread(data, 1, length, f) != (size_t)length))
	{
		printf("SNDERR can't read %s\n", path);
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = (uint32_t)length;
	return (data);
}

// bankload BASE: new instruments and music (BASE.vh, BASE.vb, BASE.seq) while the tester runs: the music paused (so
// the tester reads none of it while it is written), the files and the file table written as -snd does, then the
// tester asked to load them (BankReload in soundtest.c) and play the music from the same time
static void	BankLoad(const char *base)
{
	static const char	*ext[3] = {".vh", ".vb", ".seq"};
	static const int	types[3] = {1, 2, TESTER_FILE_SEQ};
	uint8_t				pos[POS_SIZE], *data[3] = {NULL, NULL, NULL};
	uint32_t			table[2 + 3 * 3], size[3], offset = TESTER_HEADER_SIZE, keep_paused, i;
	char				path[1024];
	bool				ok = true;

	if (!Usb('D', pos, TESTER_POS_ADRS, sizeof(pos)) || (Get16(pos + POS_VERSION) < 3))
	{
		printf("SNDERR this sound tester can't reload the instruments (rebuild soundtest.ps-exe)\n");
		return;
	}
	table[0] = TESTER_FILE_MAGIC;
	table[1] = 3;
	for (i = 0; (i < 3) && ok; i++)
	{
		snprintf(path, sizeof(path), "%s%s", base, ext[i]);
		if (!(data[i] = ReadWhole(path, &size[i])))
			ok = false;
		else
		{
			table[2 + i * 3] = types[i];
			table[2 + i * 3 + 1] = TESTER_DATA_ADRS + offset;
			table[2 + i * 3 + 2] = size[i];
			offset += (size[i] + 3) & ~3;
		}
	}
	if (ok && (TESTER_DATA_ADRS + offset > TESTER_SEQ_SPARE))
	{
		printf("SNDERR the sound files are too big (%u KB, %u KB free)\n", offset / 1024,
			   (TESTER_SEQ_SPARE - TESTER_DATA_ADRS - TESTER_HEADER_SIZE) / 1024);
		ok = false;
	}
	if (ok)
	{
		// The music paused (its notes released), waited for: nothing read while the files are written
		keep_paused = Get16(pos + POS_PAUSED) ? 1 : 0;
		if (!keep_paused)
		{
			PosRequest(POS_REQ_PAUSE, 1, 2);
			for (i = 0; i < 10; i++)
			{
				LiveSleepMs(20);
				if (!Usb('D', pos, TESTER_POS_ADRS, sizeof(pos)) || (Get16(pos + POS_REQ_PAUSE) < 0))
					break;
			}
		}
		for (i = 0; (i < 3) && ok; i++)
			ok = Usb('U', data[i], table[2 + i * 3 + 1], size[i]);
		ok = ok && Usb('U', (uint8_t *)table, TESTER_DATA_ADRS, sizeof(table)) &&
			 Usb('U', (uint8_t *)&keep_paused, TESTER_POS_ADRS + POS_RELOAD_SIZE, 4);
		if (ok)
		{
			PosRequest(POS_REQ_RELOAD, 3, 2);
			// Waited for (up to 2 s): the tester busy loading them doesn't answer, the next commands would fail
			for (i = 0; i < 40; i++)
			{
				LiveSleepMs(50);
				if (USB_Process('D', pos, TESTER_POS_ADRS, sizeof(pos)) && (Get16(pos + POS_REQ_RELOAD) <= 0))
					break;
				LiveDrain();
			}
			slot = 1;
			vh_adrs = table[3];		// (the VAB editor's place of the VH)
			vh_size = size[0];
			printf("SNDBANK %u\n", offset - TESTER_HEADER_SIZE);
		}
		else
			printf("SNDERR USB error\n");
	}
	for (i = 0; i < 3; i++)
		free(data[i]);
}

// Cross held (or released) from the computer
static void	Cross(bool on)
{
	uint32_t	bits = on ? PAD_CROSS : 0;

	if (Usb('U', (uint8_t *)&bits, REMOTE_PAD_ADRS, sizeof(bits)))
		pressed = on;
	else
		printf("SNDERR USB error\n");
}

static int	Arg(int def)
{
	char	*arg = strtok(NULL, " \t");

	return (arg ? atoi(arg) : def);
}

// play [N]: the note (if given), then Cross; a note held from here is released first (seen by the tester), so that
// the new one starts
static void	Play(void)
{
	int	note = Arg(-1);

	if (pressed)
	{
		Cross(false);
		LiveSleepMs(RELEASE_MS);
	}
	if ((note >= 0) && (note <= 127))
		Request(SYNC_REQ_NOTE, note);
	Cross(true);
}

// vabwrite OFFSET HEX: bytes of the VH changed (VAB editor), then heard on the notes sounding (req_refresh)
static void	VabWrite(void)
{
	static uint8_t	buf[MAX_HEX_BYTES];
	char			*arg = strtok(NULL, " \t"), *hex = strtok(NULL, " \t");
	uint32_t		offset, size, i;

	if (!vh_adrs)
	{
		printf("SNDERR no VAB to edit (VH + VB or VAB files, and the new soundtest.ps-exe)\n");
		return;
	}
	if (!arg || !hex || (strlen(hex) & 1) || !(size = strlen(hex) / 2) || (size > MAX_HEX_BYTES) ||
		((offset = strtoul(arg, NULL, 10)) + size > vh_size))
	{
		printf("SNDERR vabwrite OFFSET HEX (inside the VH, %u bytes)\n", vh_size);
		return;
	}
	for (i = 0; i < size; i++)
	{
		char	byte[3] = {hex[i * 2], hex[i * 2 + 1], 0};

		buf[i] = strtoul(byte, NULL, 16);
	}
	if (!Usb('U', buf, vh_adrs + offset, size))
	{
		printf("SNDERR USB error\n");
		return;
	}
	Request(SYNC_REQ_REFRESH, 1);
	printf("SNDWROTE %u %u\n", offset, size);
}

// fx REVERB DEPTH VOICES TRANSPOSE TEMPO MONO MUTE [DELAY FEEDBACK REV_CHANNELS]: the values, then req_fx (the tester
// takes them at its next frame). DELAY, FEEDBACK (-1: libspu's default) and REV_CHANNELS (VOICES 3) in SoundFx2, for a
// tester that has it (SNDFX version 2)
static void	Fx(void)
{
	int16_t	v[7], v2[3];
	int		i;

	for (i = 0; i < 7; i++)
		v[i] = Arg((i == 1) ? 64 : ((i == 4) ? 100 : 0));
	v2[0] = Arg(-1);
	v2[1] = Arg(-1);
	v2[2] = Arg(0xFFFF);
	if ((fx_version >= 2) && !Usb('U', (uint8_t *)v2, TESTER_FX2_ADRS + 2, sizeof(v2)))
		printf("SNDERR USB error\n");
	else if (!Usb('U', (uint8_t *)v, TESTER_SYNC_ADRS + SYNC_FX, sizeof(v)))
		printf("SNDERR USB error\n");
	else
		Request(SYNC_REQ_FX, 1);
}

// A block of the console printed in hexadecimal after a prefix
static void	PrintHex(const char *prefix, const uint8_t *data, uint32_t size)
{
	uint32_t	i;

	printf("%s ", prefix);
	for (i = 0; i < size; i++)
		printf("%02X", data[i]);
	printf("\n");
}

static void	VabRead(void)
{
	static uint8_t	buf[MAX_HEX_BYTES];

	if (!vh_adrs || (vh_size > MAX_HEX_BYTES))
		printf("SNDERR no VAB to read\n");
	else if (!Usb('D', buf, vh_adrs, vh_size))
		printf("SNDERR USB error\n");
	else
		PrintHex("SNDVH", buf, vh_size);
}

// The SPU registers as the SPU has them (what the edits gave): read by the debug monitor ('M' service)
static void	SpuRegs(void)
{
	uint8_t	regs[MON_SPU_REGS_SIZE];

	LiveDrain();
	if (!HotService(MON_CMD_SPU_READ, regs, MON_SPU_REGS, sizeof(regs)))
	{
		usb_failures++;
		printf("SNDERR the SPU registers couldn't be read (firmware with the debug monitor version 10?)\n");
	}
	else
		PrintHex("SNDSPU", regs, sizeof(regs));
}

// The tester's state, printed when it changes
static void	State(bool force)
{
	static char	last[128];
	uint8_t		block[POS_SIZE + SYNC_SIZE], *sync = block + POS_SIZE, *pos = block;
	char		text[128];
	int			i;

	if (!Usb('D', block, TESTER_POS_ADRS, sizeof(block)) || (Get32(sync) != TESTER_SYNC_MAGIC))
		return;
	if (Get16(pos + POS_VERSION) >= 1)
	{
		static char	last_pos[64];

		sprintf(text, " %u %u %d", Get32(pos + POS_MS) / 100 * 100, Get32(pos + POS_LEN_MS), Get16(pos + POS_PAUSED));
		if (force || strcmp(text, last_pos))
		{
			strcpy(last_pos, text);
			printf("SNDPOS%s\n", text);
		}
	}
	text[0] = 0;
	for (i = 0; i < 8; i++)
		sprintf(text + strlen(text), " %d", Get16(sync + SYNC_STATE + i * 2));
	sprintf(text + strlen(text), " %06X", Get32(sync + SYNC_VOICES) & 0xFFFFFF);
	if (force || strcmp(text, last))
	{
		strcpy(last, text);
		printf("SNDSTATE%s\n", text);
	}
	if (Get16(sync + SYNC_FX_VERSION) >= 1)
	{
		static char	last_ch[128];

		sprintf(text, " %d %04X %04X", Get16(sync + SYNC_REV_ACTIVE), Get16(sync + SYNC_CH_USED) & 0xFFFF,
			Get16(sync + SYNC_CH_ACTIVE) & 0xFFFF);
		for (i = 0; i < 16; i++)
			sprintf(text + strlen(text), " %d", sync[SYNC_CH_PROG + i]);
		sprintf(text + strlen(text), " %d %d", Get16(sync + SYNC_SEP_INDEX), Get16(sync + SYNC_SEP_COUNT));
		if (force || strcmp(text, last_ch))
		{
			strcpy(last_ch, text);
			printf("SNDCHAN%s\n", text);
		}
	}
}

int	SndLive(void)
{
	uint8_t		sync[SYNC_SIZE];
	char		*cmd, *text, progs[128 * 4 + 16];
	bool		eof = false;
	uint32_t	waited = 0;
	int			i;

	setvbuf(stdout, NULL, _IONBF, 0);
	ps1_program_running = true;		// Only the kernel hook (and the debug monitor) answer
	usb_big_packet_size = 0;
	usb_quiet = true;
	for (i = 0; i < 30; i++)		// (the tester starts: SPU set up, sounds uploaded to the SPU RAM)
	{
		LiveSleepMs(100);
		if (Usb('D', sync, TESTER_SYNC_ADRS, sizeof(sync)) && (Get32(sync) == TESTER_SYNC_MAGIC))
			break;
	}
	if (Get32(sync) != TESTER_SYNC_MAGIC)
	{
		printf("SNDERR the sound tester doesn't answer (an older soundtest.ps-exe?)\n");
		return (1);
	}
	Cross(false);
	printf("SNDLIVE ready\n");
	printf("SNDINFO %d %d %d %d %d %u\n", Get16(sync + SYNC_KIND), Get16(sync + SYNC_N_PROGRAMS), Get16(sync + SYNC_VAB),
		Get16(sync + SYNC_VAB + 2), Get16(sync + SYNC_VAB + 4), Get32(sync + SYNC_VAG_RATE));
	strcpy(progs, "SNDPROGS");
	for (i = 0; i < 128; i++)
		if (sync[SYNC_PROG_MAP + (i >> 3)] & (1 << (i & 7)))
			sprintf(progs + strlen(progs), " %d", i);
	printf("%s\n", progs);
	vh_adrs = (Get16(sync + SYNC_KIND) == 1) ? Get32(sync + SYNC_VH_ADRS) : 0;
	vh_size = vh_adrs ? Get32(sync + SYNC_VH_SIZE) : 0;
	if ((vh_adrs < 0x80000000) || (vh_adrs >= 0x80200000) || (vh_size > MAX_HEX_BYTES))
		vh_adrs = vh_size = 0;
	printf("SNDVAB %08X %u\n", vh_adrs, vh_size);
	fx_version = Get16(sync + SYNC_FX_VERSION);
	printf("SNDFX %d %05X\n", fx_version, Get32(sync + SYNC_SAMPLES_END));
	State(true);

	while (!eof)
	{
		if (usb_failures >= USB_LOST)
		{
			printf("SNDLIVE disconnected\n");
			return (1);
		}
		if (!LiveNextLine(&text, &eof))
		{
			LiveSleepMs(5);
			waited += 5;
			if (waited >= POLL_MS)
			{
				waited = 0;
				State(false);
			}
			continue;
		}
		if (!(cmd = strtok(text, " \t")))
			continue;
		if (!strcmp(cmd, "program"))
			Request(SYNC_REQ_PROG, Arg(-1));
		else if (!strcmp(cmd, "note"))
			Request(SYNC_REQ_NOTE, Arg(-1));
		else if (!strcmp(cmd, "volume"))
			Request(SYNC_REQ_VOLUME, Arg(-1));
		else if (!strcmp(cmd, "play"))
			Play();
		else if (!strcmp(cmd, "release"))
			Cross(false);
		else if (!strcmp(cmd, "seq"))
			Request(SYNC_REQ_SEQ, Arg(1) ? 1 : 0);
		else if (!strcmp(cmd, "stop"))
		{
			Cross(false);
			Request(SYNC_REQ_STOP, 1);
		}
		else if (!strcmp(cmd, "vabwrite"))
			VabWrite();
		else if (!strcmp(cmd, "vabread"))
			VabRead();
		else if (!strcmp(cmd, "sep"))
			Request(SYNC_REQ_SEP, Arg(0));
		else if (!strcmp(cmd, "repeat"))
			Request(SYNC_REQ_REPEAT, Arg(1) ? 1 : 0);
		else if (!strcmp(cmd, "seek"))
		{
			int	ms = Arg(0);

			PosRequest(POS_REQ_SEEK, (ms < 0) ? 0 : ms, 4);
		}
		else if (!strcmp(cmd, "seqload"))
		{
			char	*path = strtok(NULL, "");		// (the rest of the line: a path with spaces)

			while (path && (*path == ' '))
				path++;
			if (path && *path)
				SeqLoad(path);
			else
				printf("SNDERR seqload: give the SEQ file\n");
		}
		else if (!strcmp(cmd, "bankload"))
		{
			char	*base = strtok(NULL, "");

			while (base && (*base == ' '))
				base++;
			if (base && *base)
				BankLoad(base);
			else
				printf("SNDERR bankload: give the files' name (without .vh, .vb, .seq)\n");
		}
		else if (!strcmp(cmd, "pause"))
			PosRequest(POS_REQ_PAUSE, Arg(1) ? 1 : 0, 2);
		else if (!strcmp(cmd, "fx"))
			Fx();
		else if (!strcmp(cmd, "spuregs"))
			SpuRegs();
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("SNDERR unknown command %s\n", cmd);
	}
	if (pressed)		// (no note left held by the computer)
		Cross(false);
	// The link ends (Stop on the page, quit, the Manager closed): the music stopped and every sound silenced, else the
	// tester played on with nothing left to stop it but a reset
	Request(SYNC_REQ_STOP, 1);
	return (0);
}
