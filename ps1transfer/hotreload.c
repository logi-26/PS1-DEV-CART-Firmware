/*
* PS1 DEV CART Transfer tool: hot reload (-hot, -watch) (from main.c)
*
* PS1USB Transfer tool v1.0
* by OrionSoft [2024]
* https://orionsoft.games/
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*
* Linux version require libftdi1-dev
*/

#include "ps1transfer.h"

/****************************************/
// Hot reload: files sent to the running program without restarting it. VRAM and SPU RAM are written by
// the debug monitor ('M' service byte, then a 'W'/'S' command, see monitor/monitor.c), RAM by the kernel hook ('U').
//   file.tim				VRAM at the TIM position (image and CLUT), file.tim@x,y: image at another position
//   file.vag@0x1010		SPU RAM (VAG header skipped), also .vb (VAB body) or any file, address < 0x80000
//   file.bin@0x80123456	RAM

static void	HotConsole(void);

// A command run by the debug monitor while the program runs ('M' service): VRAM read/write... (also timlive.c)
bool	HotService(char cmd, uint8_t *data, uint32_t param, uint32_t size)
{
	uint8_t	service = MON_CMD_SERVICE;

	if (ftdi_write_data(ftdi, &service, 1) != 1)
		return (false);
	// The program stops at its next interrupt (at most a frame): printf text sent before would be read as the answer
	// to the command, it is shown first
#ifdef _WIN32
	Sleep(50);
#else
	usleep(50000);
#endif
	HotConsole();
	return (USB_Process(cmd, data, param, size));
}

// Text printed by the program (and PCDRV requests) waiting in the USB FIFO, read before a command and in -watch
static void	HotConsole(void)
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

static bool	HotReloadFile(HotFile *hot)
{
	char		file[1024], *at, *ext;
	uint8_t		*data, *p;
	uint32_t	target = 0, flags, x, y, w, h, block;
	int			size, sent = 0;
	bool		has_target, ok = true, is_tim;

	strncpy(file, hot->name, sizeof(file) - 1);
	file[sizeof(file) - 1] = 0;
	if ((has_target = ((at = strrchr(file, '@')) != NULL)))
		*at++ = 0;
	ext = strrchr(file, '.');
	is_tim = ext && !strcasecmp(ext, ".tim");
	if (!(data = SystemGetFile(file, &size)))
		return (false);
	HotConsole();

	if (is_tim)
	{
		// TIM: ID, flags, [CLUT block], image block. Block: length, x, y, w, h (16-bit), data.
		if ((size < 8) || (*(uint32_t *)data != 0x10))
		{
			printf("'%s' is not a TIM file !\n", file);
			free(data);
			return (false);
		}
		flags = ((uint32_t *)data)[1];
		p = data + 8;
		while (ok && (p + 12 <= data + size))
		{
			block = *(uint32_t *)p;
			x = ((uint16_t *)p)[2];
			y = ((uint16_t *)p)[3];
			w = ((uint16_t *)p)[4];
			h = ((uint16_t *)p)[5];
			if ((block < 12) || (p + block > data + size) || !w || !h || (x + w > 1024) || (y + h > 512))
			{
				printf("'%s': bad TIM block\n", file);
				ok = false;
				break;
			}
			// Image block (the last one) at another position with file.tim@x,y; the CLUT block (the first one, flag 8)
			// too with file.tim@x,y,cx,cy (a texture replaced where the program has it: its CLUT where it has its own)
			if (has_target && (!(flags & 8) || (p != data + 8)))
				sscanf(at, "%u,%u", &x, &y);
			else if (has_target)
			{
				uint32_t	ix, iy, cx, cy;

				if (sscanf(at, "%u,%u,%u,%u", &ix, &iy, &cx, &cy) == 4)
				{
					x = cx;
					y = cy;
				}
			}
			ok = HotService(MON_CMD_VRAM_WRITE, p + 12, (x << 16) | y, (w << 16) | h);
			sent += w * h * 2;
			p += block;
		}
	}
	else if (!has_target)
	{
		printf("'%s': give the address, e.g. %s@0x1010 (SPU RAM) or %s@0x80100000 (RAM)\n", file, file, file);
		free(data);
		return (false);
	}
	else
	{
		target = strtoul(at, NULL, 0);
		p = data;
		if (ext && !strcasecmp(ext, ".vag") && (size > 48) && !memcmp(data, "VAGp", 4))	// VAG header
		{
			p += 48;
			size -= 48;
		}
		if (target >= 0x80000000)	// RAM, with the kernel hook (no monitor needed)
			ok = USB_Process('U', p, target, size);
		else if (target + size <= 0x80000)	// SPU RAM (512KB), even size
			ok = HotService(MON_CMD_SPU_WRITE, p, target, (size + 1) & ~1);
		else
		{
			printf("'%s': address 0x%X is neither SPU RAM (< 0x80000) nor RAM (0x80000000...)\n", file, target);
			ok = false;
		}
		sent = size;
	}
	free(data);
	if (ok)
		printf("Reloaded %s (%d bytes)\n", hot->name, sent);
	else
		printf("Reload of %s failed (USB error).\n", hot->name);
	return (ok);
}

// Sends the files, then with watch: sends a file again each time it changes, and shows the program printf
bool	HotReload(HotFile *files, int count, bool watch)
{
	bool		ok = true;		// All the files sent (exit code)
	struct stat	st;
	uint8_t		state[16];
	int			i;
	char		file[1024], *at;

	if (!ps1_program_running)
	{
		printf("-hot sends files to a running program: start it first (ps1transfer -e -f game.exe).\n");
		return (false);
	}
	// VRAM/SPU writes need the debug monitor with the 'M' service (MonitorState.version >= 1)
	if (!USB_Process('D', state, MONITOR_STATE_ADRS, sizeof(state)))
		return (false);
	if ((*(uint32_t *)state != MONITOR_MAGIC) || (state[14] < 1))
		printf("Warning: the firmware has no debug monitor with hot reload, only RAM (@0x80...) files can be sent.\n");

	for (i = 0; i < count; i++)
	{
		if (!HotReloadFile(&files[i]))
			ok = false;
		strncpy(file, files[i].name, sizeof(file) - 1);
		file[sizeof(file) - 1] = 0;
		if ((at = strrchr(file, '@')))
			*at = 0;
		if (!stat(file, &st))
		{
			files[i].mtime = st.st_mtime;
			files[i].size = st.st_size;
		}
	}
	if (watch)
		printf("Watching %d file(s), Ctrl+C to stop...\n", count);
	setvbuf(stdout, NULL, _IONBF, 0);
	while (watch)
	{
		HotConsole();
		for (i = 0; i < count; i++)
		{
			strncpy(file, files[i].name, sizeof(file) - 1);
			file[sizeof(file) - 1] = 0;
			if ((at = strrchr(file, '@')))
				*at = 0;
			if (stat(file, &st) || ((st.st_mtime == files[i].mtime) && (st.st_size == files[i].size)))
				continue;
			// Changed: wait until the program writing it has finished (same size twice)
			do
			{
				files[i].size = st.st_size;
#ifdef _WIN32
				Sleep(300);
#else
				usleep(300000);
#endif
			} while (!stat(file, &st) && (st.st_size != files[i].size));
			files[i].mtime = st.st_mtime;
			HotReloadFile(&files[i]);
		}
#ifdef _WIN32
		Sleep(100);
#else
		usleep(100000);
#endif
	}
	return (ok);
}

