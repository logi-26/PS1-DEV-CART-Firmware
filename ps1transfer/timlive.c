/*
* PS1 DEV CART Transfer tool: live link of the TIM viewer with the PS1 Dev Cart Manager ("-tim file.tim -c -timlive")
*
* The TIM viewer (ps1-tools/timviewer) runs with its data block (the TIM files, see UploadTimData in main.c). Its
* palette editor edits the CLUT in the TIM file of the data block and loads the edited row in VRAM. Here the computer
* does the same while the viewer runs: a CLUT row is written in the TIM file (RAM, the kernel hook) and in VRAM (the
* debug monitor's 'M' service), so the picture on the TV and the viewer's own palette editor show it at once. The
* palettes edited on the PS1 are read back from RAM, the whole VRAM can be read for the computer's view.
*
* Commands, one per line on the standard input (answers start with "TIM"):
*	clut I ROW V0 V1 ...	CLUT row ROW of the TIM file I (0 = first -tim): 16-bit colors in hexadecimal (PS1 format:
*							red bits 0-4, green 5-9, blue 10-14, bit 15 semi-transparency), from the start of the row
*	read					the CLUTs as the PS1 has them: "TIMCLUT I ROW V0 V1 ..." lines, then "TIMCLUT end"
*	vram FILE				the whole VRAM (1024x512, 16-bit little endian) written to FILE: "TIMVRAM ok FILE"
*	select I ROW			show the TIM file I on the PS1 (its TIM view), with its CLUT row ROW (-1: unchanged)
*	state					the TIM and CLUT row shown on the PS1, its text shown (1) or hidden (0): "TIMSEL I ROW TEXT"
*							("TIMSEL none": older viewer)
*	text 0/1				the viewer's info panel and controls hidden / shown (Start on the PS1)
*	move I IX IY CX CY		TIM file I moved in VRAM: image at IX, IY, CLUT at CX, CY (-1: no CLUT); written in the
*							file's headers, the viewer clears the old areas and loads it at the new ones
*	quit
* At the start: "TIMLIVE ready N", then "TIMINFO I CLUT_X CLUT_Y CLUT_W CLUT_H" for each file (CLUT_W 0: no CLUT), and
* "TIMAREA X Y W H" for the viewer's screen buffers and font (VRAM areas a TIM must not be moved to).
* "TIMLIVE disconnected" (then the end) when the cartridge stops answering. Errors: "TIMERR text". The program's printf text and the file server (palette editor save/reload) go on meanwhile.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <sys/select.h>
#endif

#include "timlive.h"
#include "../monitor/monitor.h"

#define	TIMVIEW_DATA_ADRS	0x80100000	// Data block of the viewer (see UploadTimData in main.c and timviewer.c)
#define	TIMVIEW_MAGIC		0x564D4954	// "TIMV"
#define	TIMVIEW_MAX_FILES	32
#define	TIMVIEW_ENTRY_SIZE	(8 + 24)	// Address, size, name
#define	TIM_CLUT_DATA		20			// ID, flags, CLUT block: length, x, y, w, h, then the colors
#define	TIMVIEW_SYNC_ADRS	(TIMVIEW_DATA_ADRS + 0x7C0)	// Shared with the viewer (ViewerSync in timviewer.c)
#define	TIMVIEW_SYNC_MAGIC	0x434E5953	// "SYNC"
#define	SYNC_REQ_MOVE		16			// ViewerSync offsets: TIM moved, the viewer's areas (3 x 4 int16)
#define	SYNC_AREAS			20
#define	SYNC_SIZE			44
#define	SYNC_SHOWN			44			// Screen on the TV (x, y), screenshot hold / held
#define	SYNC_HOLD			48
#define	SYNC_HELD			50
#define	SYNC_SIZE_V2		52
#define	VRAM_W				1024
#define	VRAM_H				512
#define	LINE_MAX			16384

typedef struct
{
	uint32_t	adrs;		// TIM file in RAM
	uint16_t	x, y, w, h;	// CLUT in VRAM, w = 0: no CLUT
	uint32_t	img_block;	// Offset of the image block in the file (its x, y at + 4)
} LiveTim;

static LiveTim	tims[TIMVIEW_MAX_FILES];
static int		n_tims;
static char		line[LINE_MAX];
static int		line_len;
static int		usb_failures;	// Commands failed in a row: the cartridge was unplugged, or the console switched off

#define	USB_LOST			3	// Failures in a row: "TIMLIVE disconnected", the link ends

static bool	Usb(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	bool	ok = USB_Process(cmd, data, adrs, size);

	usb_failures = ok ? 0 : usb_failures + 1;
	return (ok);
}

static bool	Service(char cmd, uint8_t *data, uint32_t param, uint32_t size)
{
	bool	ok = HotService(cmd, data, param, size);

	usb_failures = ok ? 0 : usb_failures + 1;
	return (ok);
}

static void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

static uint32_t	Get32(const uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}
static uint16_t	Get16(const uint8_t *p)	{return (p[0] | (p[1] << 8));}

// Text printed by the viewer and file server requests (palette editor save/reload) waiting in the USB FIFO
static void	Drain(void)
{
	uint8_t	c;

	while (USB_ReadByteNoWait(&c))
	{
		if (c == 16)	// Data Link Escape
			PC_FileServer();
		else
			putchar(c);
	}
}

// Bytes waiting on the standard input, without waiting: > 0 read, 0 nothing, -1 end of the input
static int	ReadInput(char *buf, int size)
{
#ifdef _WIN32
	HANDLE	h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	avail = 0, n = 0;

	if (GetFileType(h) == FILE_TYPE_PIPE)
	{
		if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
			return (-1);
		if (!avail)
			return (0);
		if (avail > (DWORD)size)
			avail = size;
		return (ReadFile(h, buf, avail, &n, NULL) ? (int)n : -1);
	}
	if (!_kbhit())		// Console: a line typed
		return (0);
	return (fgets(buf, size, stdin) ? (int)strlen(buf) : -1);
#else
	fd_set			fds;
	struct timeval	tv = {0, 0};
	ssize_t			n;

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	if (select(1, &fds, NULL, NULL, &tv) <= 0)
		return (0);
	n = read(0, buf, size);
	return ((n > 0) ? (int)n : -1);
#endif
}

// A whole line from the standard input in line[] (true), false if none yet; *eof at the end of the input
static bool	NextLine(bool *eof)
{
	static char	in[4096];
	static int	in_len, in_pos;
	int			n;

	for (;;)
	{
		if (in_pos >= in_len)
		{
			if ((n = ReadInput(in, sizeof(in))) < 0)
				*eof = true;
			if (n <= 0)
				return (false);
			in_len = n;
			in_pos = 0;
		}
		while (in_pos < in_len)
		{
			char	c = in[in_pos++];

			if (c == '\n')
			{
				line[line_len] = 0;
				line_len = 0;
				return (true);
			}
			if ((c != '\r') && (line_len < LINE_MAX - 1))
				line[line_len++] = c;
		}
	}
}

// The TIM files of the viewer's data block, their CLUT position in VRAM
static bool	ReadTims(void)
{
	uint8_t		header[8 + TIMVIEW_MAX_FILES * TIMVIEW_ENTRY_SIZE], tim[TIM_CLUT_DATA];
	int			i;

	if (!Usb('D', header, TIMVIEW_DATA_ADRS, sizeof(header)) || (Get32(header) != TIMVIEW_MAGIC))
		return (false);
	n_tims = Get32(header + 4);
	if (n_tims > TIMVIEW_MAX_FILES)
		n_tims = TIMVIEW_MAX_FILES;
	for (i = 0; i < n_tims; i++)
	{
		tims[i].adrs = Get32(header + 8 + i * TIMVIEW_ENTRY_SIZE);
		tims[i].w = 0;
		tims[i].img_block = 8;
		if (!Usb('D', tim, tims[i].adrs, sizeof(tim)))
			return (false);
		if ((Get32(tim) == 0x10) && (Get32(tim + 4) & 8))	// TIM with a CLUT
		{
			tims[i].x = Get16(tim + 12);
			tims[i].y = Get16(tim + 14);
			tims[i].w = Get16(tim + 16);
			tims[i].h = Get16(tim + 18);
			tims[i].img_block = 8 + Get32(tim + 8);		// (after the CLUT block)
		}
	}
	return (true);
}

static LiveTim	*ClutArg(char *text)
{
	int	i = text ? atoi(text) : -1;

	if ((i < 0) || (i >= n_tims) || !tims[i].w)
	{
		printf("TIMERR no TIM file %s with a CLUT\n", text ? text : "");
		return (NULL);
	}
	return (&tims[i]);
}

// clut I ROW V0 V1 ...: the row in the TIM file (the viewer's palette editor) and in VRAM (the picture)
static void	ClutWrite(void)
{
	LiveTim		*t = ClutArg(strtok(NULL, " \t"));
	char		*arg = strtok(NULL, " \t");
	int			row = arg ? atoi(arg) : -1, n = 0;
	uint16_t	*colors;

	if (!t)
		return;
	if ((row < 0) || (row >= t->h))
	{
		printf("TIMERR CLUT row %d (0-%d)\n", row, t->h - 1);
		return;
	}
	colors = malloc(t->w * 2);
	while ((n < t->w) && (arg = strtok(NULL, " \t")))
		colors[n++] = strtoul(arg, NULL, 16);
	Drain();
	if (n && (!Usb('U', (uint8_t *)colors, t->adrs + TIM_CLUT_DATA + row * t->w * 2, n * 2) ||
		!Service(MON_CMD_VRAM_WRITE, (uint8_t *)colors, (t->x << 16) | (t->y + row), (n << 16) | 1)))
		printf("TIMERR USB error\n");
	free(colors);
}

// The CLUTs as the PS1 has them (edited with its palette editor too)
static void	ClutRead(void)
{
	uint16_t	*colors;
	int			i, row, x;

	for (i = 0; i < n_tims; i++)
	{
		if (!tims[i].w)
			continue;
		colors = malloc(tims[i].w * tims[i].h * 2);
		Drain();
		if (!Usb('D', (uint8_t *)colors, tims[i].adrs + TIM_CLUT_DATA, tims[i].w * tims[i].h * 2))
		{
			printf("TIMERR USB error\n");
			free(colors);
			return;
		}
		for (row = 0; row < tims[i].h; row++)
		{
			printf("TIMCLUT %d %d", i, row);
			for (x = 0; x < tims[i].w; x++)
				printf(" %04X", colors[row * tims[i].w + x]);
			printf("\n");
		}
		free(colors);
	}
	printf("TIMCLUT end\n");
}

// The whole VRAM, read from the GPU by the debug monitor (the viewer waits meanwhile)
static void	VramRead(char *file)
{
	uint8_t	*vram = malloc(VRAM_W * VRAM_H * 2);
	FILE	*f;

	Drain();
	if (!file || !*file)
		printf("TIMERR vram FILE\n");
	else if (!Service('V', vram, 0, (VRAM_W << 16) | VRAM_H))
		printf("TIMVRAM error USB\n");
	else if (!(f = fopen(file, "wb")) || (fwrite(vram, 1, VRAM_W * VRAM_H * 2, f) != VRAM_W * VRAM_H * 2))
	{
		printf("TIMVRAM error can't write %s\n", file);
		if (f)
			fclose(f);
	}
	else
	{
		fclose(f);
		printf("TIMVRAM ok %s\n", file);
	}
	free(vram);
}

// select I ROW: the viewer shows this TIM (and CLUT row) at its next frame
static void	Select(void)
{
	char	*arg = strtok(NULL, " \t");
	int		i = arg ? atoi(arg) : -1, row = (arg = strtok(NULL, " \t")) ? atoi(arg) : -1;
	int16_t	req[2];

	if ((i < 0) || (i >= n_tims))
	{
		printf("TIMERR no TIM file %d\n", i);
		return;
	}
	req[0] = i;
	req[1] = row;
	Drain();
	if (!Usb('U', (uint8_t *)req, TIMVIEW_SYNC_ADRS + 8, sizeof(req)))
		printf("TIMERR USB error\n");
}

// text 0/1: the viewer's info panel and controls hidden or shown (Start on the PS1)
static void	Text(void)
{
	char	*arg = strtok(NULL, " \t");
	int16_t	req = (arg && atoi(arg)) ? 1 : 0;

	Drain();
	if (!Usb('U', (uint8_t *)&req, TIMVIEW_SYNC_ADRS + 14, sizeof(req)))
		printf("TIMERR USB error\n");
}

// move I IX IY CX CY: the new positions in the TIM file's headers (a save on the PS1 keeps them), then the viewer moves it
static void	Move(void)
{
	char		*arg[5];
	int			i, n;
	uint16_t	xy[2];
	int16_t		req;
	LiveTim		*t;

	for (n = 0; (n < 5) && (arg[n] = strtok(NULL, " \t")); n++)
		;
	i = (n > 0) ? atoi(arg[0]) : -1;
	if ((n < 3) || (i < 0) || (i >= n_tims))
	{
		printf("TIMERR move I IX IY CX CY\n");
		return;
	}
	t = &tims[i];
	Drain();
	xy[0] = atoi(arg[1]);
	xy[1] = atoi(arg[2]);
	if (!Usb('U', (uint8_t *)xy, t->adrs + t->img_block + 4, sizeof(xy)))
		goto usb_error;
	if (t->w && (n == 5) && (atoi(arg[3]) >= 0))
	{
		xy[0] = atoi(arg[3]);
		xy[1] = atoi(arg[4]);
		if (!Usb('U', (uint8_t *)xy, t->adrs + 8 + 4, sizeof(xy)))
			goto usb_error;
		t->x = xy[0];		// (the next CLUT writes go there)
		t->y = xy[1];
	}
	req = i;
	if (Usb('U', (uint8_t *)&req, TIMVIEW_SYNC_ADRS + SYNC_REQ_MOVE, sizeof(req)))
		return;
usb_error:
	printf("TIMERR USB error\n");
}

// The viewer's screen buffers and font in VRAM (placed in areas the TIM files don't use): "TIMAREA X Y W H" lines
static void	Areas(void)
{
	uint8_t	sync[SYNC_SIZE];
	int		i, tries;

	for (tries = 0; tries < 20; tries++)	// (the viewer fills them before its main loop)
	{
		Drain();
		if (!Usb('D', sync, TIMVIEW_SYNC_ADRS, sizeof(sync)))
			return;
		if (Get32(sync) == TIMVIEW_SYNC_MAGIC)
			break;
		SleepMs(100);
	}
	if (Get32(sync) != TIMVIEW_SYNC_MAGIC)
		return;
	for (i = 0; i < 3; i++)
		printf("TIMAREA %d %d %d %d\n", Get16(sync + SYNC_AREAS + i * 8), Get16(sync + SYNC_AREAS + i * 8 + 2),
			Get16(sync + SYNC_AREAS + i * 8 + 4), Get16(sync + SYNC_AREAS + i * 8 + 6));
}

// state: the TIM and CLUT row shown on the PS1 (changed with its pad), and if its text is shown
static void	State(void)
{
	uint8_t	sync[16];

	Drain();
	if (!Usb('D', sync, TIMVIEW_SYNC_ADRS, sizeof(sync)))
		printf("TIMERR USB error\n");
	else if (Get32(sync) != TIMVIEW_SYNC_MAGIC)
		printf("TIMSEL none\n");
	else
		printf("TIMSEL %d %d %d\n", (int16_t)Get16(sync + 4), (int16_t)Get16(sync + 6), (int16_t)Get16(sync + 12));
}

// "-v auto": the TIM viewer tells where the screen on the TV is, and stops drawing after a complete frame while the
// screenshot is read (with one screen buffer, it draws on the screen shown)
bool	TimViewScreen(int *x, int *y)
{
	uint8_t	sync[SYNC_SIZE_V2], magic[4];
	int16_t	hold = 1;
	int		tries;

	if (!USB_Process('D', magic, TIMVIEW_DATA_ADRS, 4) || (Get32(magic) != TIMVIEW_MAGIC) ||
		!USB_Process('D', sync, TIMVIEW_SYNC_ADRS, sizeof(sync)) || (Get32(sync) != TIMVIEW_SYNC_MAGIC))
		return (false);
	if (!USB_Process('U', (uint8_t *)&hold, TIMVIEW_SYNC_ADRS + SYNC_HOLD, sizeof(hold)))
		return (false);
	for (tries = 0; tries < 50; tries++)	// (a frame, at most 1 second)
	{
		if (!USB_Process('D', sync, TIMVIEW_SYNC_ADRS, sizeof(sync)))
			break;
		if (Get16(sync + SYNC_HELD))
			break;
		SleepMs(20);
	}
	*x = (int16_t)Get16(sync + SYNC_SHOWN);
	*y = (int16_t)Get16(sync + SYNC_SHOWN + 2);
	if ((*x < 0) || (*x > 1024 - 320) || (*y < 0) || (*y > 512 - 240))
	{
		TimViewRelease();
		*x = *y = 0;
		return (false);
	}
	return (true);
}

void	TimViewRelease(void)
{
	int16_t	hold = 0;

	USB_Process('U', (uint8_t *)&hold, TIMVIEW_SYNC_ADRS + SYNC_HOLD, sizeof(hold));
}

// Shared with the sound tester's live link (sndlive.c)
void	LiveDrain(void)
{
	Drain();
}

bool	LiveNextLine(char **text, bool *eof)
{
	if (!NextLine(eof))
		return (false);
	*text = line;
	return (true);
}

void	LiveSleepMs(int ms)
{
	SleepMs(ms);
}

int	TimLive(void)
{
	char	*cmd;
	bool	eof = false;
	int		i;

	setvbuf(stdout, NULL, _IONBF, 0);
	ps1_program_running = true;		// Only the kernel hook (and the debug monitor) answer
	usb_big_packet_size = 0;
	usb_quiet = true;
	for (i = 0; i < 10; i++)	// (the viewer starts)
	{
		SleepMs(100);
		Drain();
	}
	if (!ReadTims())
	{
		printf("TIMERR the TIM viewer doesn't answer\n");
		return (1);
	}
	printf("TIMLIVE ready %d\n", n_tims);
	for (i = 0; i < n_tims; i++)
		printf("TIMINFO %d %d %d %d %d\n", i, tims[i].x, tims[i].y, tims[i].w, tims[i].h);
	Areas();

	while (!eof)
	{
		if (usb_failures >= USB_LOST)
		{
			printf("TIMLIVE disconnected\n");
			return (1);
		}
		Drain();
		if (!NextLine(&eof))
		{
			SleepMs(2);
			continue;
		}
		if (!(cmd = strtok(line, " \t")))
			continue;
		if (!strcmp(cmd, "clut"))
			ClutWrite();
		else if (!strcmp(cmd, "read"))
			ClutRead();
		else if (!strcmp(cmd, "select"))
			Select();
		else if (!strcmp(cmd, "state"))
			State();
		else if (!strcmp(cmd, "text"))
			Text();
		else if (!strcmp(cmd, "move"))
			Move();
		else if (!strcmp(cmd, "vram"))
			VramRead(strtok(NULL, "\t"));	// (the path may contain spaces)
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("TIMERR unknown command %s\n", cmd);
	}
	return (0);
}
