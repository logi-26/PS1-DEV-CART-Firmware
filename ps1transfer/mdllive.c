/*
* PS1 DEV CART Transfer tool: live link of the model viewer with the PS1 Dev Cart Manager ("-m model.tmd [-tod ...]
* -mdllive")
*
* The model viewer (ps1-tools/tmdviewer) shares its state in RAM (ModelSync in tmdviewer.c): the TOD animation played,
* its frame, playback, the view, written at each frame. The computer changes them with requests the viewer applies at
* its next frame. So the controls of the computer and the controller act on the same viewer, each one shows what the
* other does.
*
* Commands, one per line on the standard input:
*	tod I			TOD I played from its start (0 = first -tod)
*	frame F			frame F of the TOD shown (paused)
*	play 0/1		paused / playing
*	loop 0/1
*	speed P			playback speed in percent (1-800)
*	rot X Y Z		view rotation (4096 = 360 degrees)
*	dist D			camera distance
*	view			view reset (rotation, distance)
*	quit
* Answers (start with "MDL"): at the start "MDLLIVE ready", "MDLINFO TODS DIST_MIN DIST_MAX"; then
* "MDLSTATE TOD FRAME FIRST LAST PLAYING LOOP SPEED ROT_X ROT_Y ROT_Z DIST OBJECTS POLYGONS SKIPPED" each time it
* changes (TOD -1: no animation; SKIPPED: TOD packets the viewer doesn't use: light, camera, matrix...). "MDLLIVE
* disconnected" (then the end) when the cartridge stops answering. Errors: "MDLERR text".
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timlive.h"

#define	VIEWER_SYNC_ADRS	0x80100300	// ModelSync in tmdviewer.c (in the data block header)
#define	VIEWER_SYNC_MAGIC	0x534C444D

// ModelSync offsets
#define	SYNC_TOD_COUNT		4
#define	SYNC_STATE			6		// tod ... objects: 11 int16
#define	SYNC_DIST			28		// dist, dist_min, dist_max, polygons, skipped (int32)
#define	SYNC_REQ_TOD		48
#define	SYNC_REQ_FRAME		50
#define	SYNC_REQ_PLAY		52
#define	SYNC_REQ_LOOP		54
#define	SYNC_REQ_SPEED		56
#define	SYNC_REQ_VIEW		58
#define	SYNC_REQ_ROT		60		// flag, x, y, z
#define	SYNC_REQ_DIST		68
#define	SYNC_SIZE			72

#define	POLL_MS				50		// The state read (and printed when it changes) 20 times a second
#define	USB_LOST			3		// Failures in a row: "MDLLIVE disconnected", the link ends

static int		usb_failures;

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

static void	Write(int offset, void *data, int size)
{
	if (!Usb('U', (uint8_t *)data, VIEWER_SYNC_ADRS + offset, size))
		printf("MDLERR USB error\n");
}

// A request of the computer (applied by the viewer at its next frame)
static void	Request(int offset, int value)
{
	int16_t	v = value;

	Write(offset, &v, sizeof(v));
}

static int	Arg(int def)
{
	char	*arg = strtok(NULL, " \t");

	return (arg ? atoi(arg) : def);
}

// The viewer's state, printed when it changes
static void	State(bool force)
{
	static char	last[160];
	uint8_t		sync[SYNC_SIZE];
	char		text[160];
	int			i;

	if (!Usb('D', sync, VIEWER_SYNC_ADRS, sizeof(sync)) || (Get32(sync) != VIEWER_SYNC_MAGIC))
		return;
	text[0] = 0;
	for (i = 0; i < 10; i++)		// tod ... rot_z
		sprintf(text + strlen(text), " %d", Get16(sync + SYNC_STATE + i * 2));
	sprintf(text + strlen(text), " %d %d %d %d", (int)Get32(sync + SYNC_DIST), Get16(sync + SYNC_STATE + 20),
		(int)Get32(sync + SYNC_DIST + 12), (int)Get32(sync + SYNC_DIST + 16));
	if (force || strcmp(text, last))
	{
		strcpy(last, text);
		printf("MDLSTATE%s\n", text);
	}
}

int	MdlLive(void)
{
	uint8_t		sync[SYNC_SIZE];
	char		*cmd, *text;
	bool		eof = false;
	uint32_t	waited = 0;
	int			i;

	setvbuf(stdout, NULL, _IONBF, 0);
	ps1_program_running = true;		// Only the kernel hook (and the debug monitor) answer
	usb_big_packet_size = 0;
	usb_quiet = true;
	for (i = 0; i < 50; i++)		// (the viewer starts: textures, model, the animations prepared)
	{
		LiveSleepMs(100);
		if (Usb('D', sync, VIEWER_SYNC_ADRS, sizeof(sync)) && (Get32(sync) == VIEWER_SYNC_MAGIC))
			break;
	}
	if (Get32(sync) != VIEWER_SYNC_MAGIC)
	{
		printf("MDLERR the model viewer doesn't answer (an older tmdviewer.ps-exe?)\n");
		return (1);
	}
	printf("MDLLIVE ready\n");
	printf("MDLINFO %d %d %d\n", Get16(sync + SYNC_TOD_COUNT), (int)Get32(sync + SYNC_DIST + 4),
		(int)Get32(sync + SYNC_DIST + 8));
	State(true);

	while (!eof)
	{
		if (usb_failures >= USB_LOST)
		{
			printf("MDLLIVE disconnected\n");
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
		if (!strcmp(cmd, "tod"))
			Request(SYNC_REQ_TOD, Arg(-1));
		else if (!strcmp(cmd, "frame"))
			Request(SYNC_REQ_FRAME, Arg(0));
		else if (!strcmp(cmd, "play"))
			Request(SYNC_REQ_PLAY, Arg(1) ? 1 : 0);
		else if (!strcmp(cmd, "loop"))
			Request(SYNC_REQ_LOOP, Arg(1) ? 1 : 0);
		else if (!strcmp(cmd, "speed"))
			Request(SYNC_REQ_SPEED, Arg(100));
		else if (!strcmp(cmd, "view"))
			Request(SYNC_REQ_VIEW, 1);
		else if (!strcmp(cmd, "rot"))
		{
			int16_t	r[4];

			r[1] = Arg(0);
			r[2] = Arg(0);
			r[3] = Arg(0);
			r[0] = 1;
			Write(SYNC_REQ_ROT, r, sizeof(r));
		}
		else if (!strcmp(cmd, "dist"))
		{
			int32_t	d = Arg(-1);

			Write(SYNC_REQ_DIST, &d, sizeof(d));
		}
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("MDLERR unknown command %s\n", cmd);
	}
	return (0);
}
