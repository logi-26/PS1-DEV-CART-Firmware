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
*	stop			all sounds stopped (Start)
*	vabwrite OFFSET HEX	VAB editor: the bytes written in the VH the tester plays (OFFSET in the VH, decimal; HEX:
*					the bytes), applied at once to the notes sounding too: "SNDWROTE OFFSET SIZE"
*	vabread			the VH as the tester has it: "SNDVH HEX"
*	spuregs			the SPU registers (0x1F801C00-0x1F801DFF, read by the debug monitor version 10): "SNDSPU HEX"
*	quit
* Answers (start with "SND"): at the start "SNDLIVE ready", "SNDINFO KIND PROGRAMS VAB_PS VAB_TS VAB_VS VAG_RATE" (KIND 0:
* no sound data, 1: VAB, 2: VAG), "SNDPROGS P0 P1 ..." (program numbers, in index order), "SNDVAB ADDR SIZE" (the VH in
* RAM, VAB editor; 0 0: not editable, an older tester); then
* "SNDSTATE PROG_INDEX PROGRAM NOTE VOLUME PLAYING SEQ BAR LOOP VOICES" each time it changes (SEQ 0: none, 1: stopped,
* 2: playing; VOICES: hexadecimal, bit n = voice n sounding). "SNDLIVE disconnected" (then the end) when the cartridge
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
#define	SYNC_SIZE			76
#define	MAX_HEX_BYTES		0x11000	// vabwrite, vabread: a whole VH (128 programs: 68128 bytes)

#define	POLL_MS				50		// The state read (and printed when it changes) 20 times a second
#define	RELEASE_MS			40		// A release seen by the tester (2 frames) before the next note is played
#define	USB_LOST			3		// Failures in a row: "SNDLIVE disconnected", the link ends

static int		usb_failures;
static bool		pressed;			// Cross held from here
static uint32_t	vh_adrs, vh_size;	// The VH the tester plays (VAB editor)

static uint32_t	Get32(const uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}
static int16_t	Get16(const uint8_t *p)	{return ((int16_t)(p[0] | (p[1] << 8)));}

static bool	Usb(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	bool	ok;

	LiveDrain();
	ok = USB_Process(cmd, data, adrs, size);
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
	uint8_t		sync[SYNC_SIZE];
	char		text[128];
	int			i;

	if (!Usb('D', sync, TESTER_SYNC_ADRS, sizeof(sync)) || (Get32(sync) != TESTER_SYNC_MAGIC))
		return;
	text[0] = 0;
	for (i = 0; i < 8; i++)
		sprintf(text + strlen(text), " %d", Get16(sync + SYNC_STATE + i * 2));
	sprintf(text + strlen(text), " %06X", Get32(sync + SYNC_VOICES) & 0xFFFFFF);
	if (force || strcmp(text, last))
	{
		strcpy(last, text);
		printf("SNDSTATE%s\n", text);
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
		else if (!strcmp(cmd, "spuregs"))
			SpuRegs();
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("SNDERR unknown command %s\n", cmd);
	}
	if (pressed)		// (no note left held by the computer)
		Cross(false);
	return (0);
}
