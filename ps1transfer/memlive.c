/*
* PS1 DEV CART Transfer tool: memory of the running program read and written on request, for the PS1 Dev Cart
* Manager's watch list ("-memlive")
*
* Attaches to the program running (nothing is uploaded), and takes the console's place meanwhile: its printf text is
* shown and its file server (PCDRV) requests are served, so watching variables doesn't stop the console.
*
* Commands, one per line on the standard input (addresses in hexadecimal, sizes in bytes):
*	read ADDR SIZE		"MEMREAD ADDR HEX" (the bytes, up to 4096)
*	write ADDR HEX		the bytes written (little endian values as the PS1 stores them): "MEMWROTE ADDR SIZE"
*	dump ADDR SIZE FILE	RAM read in one go into FILE (up to the whole 2 MB, about 4 s while a game runs; "read" asks
*						for each block: much slower for a large area): "MEMDUMP ADDR SIZE FILE"
*	pad BITS			remote pad (program started with -pad): the buttons of the selected port pressed (PsyQ pad bits
*						in hexadecimal: PADLup 1000, PADRdown 0040...; 0: none), "pad off": the computer lets go
*	padport N			N = 0: port 1 (default), 1: port 2 is driven from here (the other port keeps a real
*						controller); needs a firmware with the debug monitor version 9 or later
*	padstate			"MEMPAD READY BUF1 BUF2 MONVER" (READY 1: started with -pad; BUF1/BUF2: the program's pad
*						buffer of each port, 0: it hasn't called InitPAD yet (BUF2 also 0 with an older firmware);
*						MONVER: the debug monitor version, "MEMPAD 0 0 0 0" if it can't be read)
*	padtype ID			the device the program sees (monitor version 11, its ID in hexadecimal, see monitor.h):
*						41 digital pad, 73 DualShock (analog), 53 analog joystick, 23 NeGcon, 12 mouse, 63 GunCon;
*						"pad BITS" then gives its buttons as the bits of its reply (~(byte 2 << 8 | byte 3))
*	padaxes HEX			its 4 other bytes (8 hexadecimal digits): analog RX RY LX LY, NeGcon twist I II L, GunCon X Y
*						(16-bit each, little endian)
*	padmove DX DY		mouse motion (decimal), given to the program over the next frames (at most 127 a frame)
*	screen FILE			the frame being drawn (the GPU's draw area) read from VRAM as 16-bit pixels into FILE:
*						"MEMSCREEN X Y W H FILE" (GunCon aiming)
*	quit
* At the start "MEMLIVE ready RUNNING" (1: a program runs, 0: the PS1 DEV CART menu); "MEMLIVE disconnected" (then
* the end) when the cartridge stops answering. Errors: "MEMERR text".
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "timlive.h"
#include "../monitor/monitor.h"

#define	MAX_READ	4096
#define	USB_LOST	3		// Failures in a row: "MEMLIVE disconnected", the link ends

static int	usb_failures;

static bool	Usb(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	bool	ok;

	LiveDrain();
	ok = USB_Process(cmd, data, adrs, size);
	usb_failures = ok ? 0 : usb_failures + 1;
	return (ok);
}

static bool	Address(char *text, uint32_t *adrs)
{
	char	*end;

	if (!text)
		return (false);
	*adrs = strtoul(text, &end, 16);
	return (!*end && (*adrs >= 0x80000000) && (*adrs < 0x80200000 || (*adrs >= 0x1F800000 && *adrs < 0x1F800400)));
}

static void	Read(void)
{
	static uint8_t	buf[MAX_READ];
	static char		hex[MAX_READ * 2 + 1];
	char			*arg = strtok(NULL, " \t");
	uint32_t		adrs, size, i;

	if (!Address(arg, &adrs) || !(arg = strtok(NULL, " \t")) || !(size = atoi(arg)) || (size > MAX_READ))
	{
		printf("MEMERR read ADDR SIZE (RAM address, 1-%d bytes)\n", MAX_READ);
		return;
	}
	if (!Usb('D', buf, adrs, size))
	{
		printf("MEMERR USB error\n");
		return;
	}
	for (i = 0; i < size; i++)
		sprintf(hex + i * 2, "%02X", buf[i]);
	printf("MEMREAD %08X %s\n", adrs, hex);
}

static void	Dump(void)
{
	char		*arg = strtok(NULL, " \t"), *file;
	uint32_t	adrs, size;
	uint8_t		*buf;
	FILE		*f;

	if (!Address(arg, &adrs) || !(arg = strtok(NULL, " \t")) || !(size = strtoul(arg, NULL, 0)) ||
		(size > ((adrs >= 0x80000000) ? 0x80200000 : 0x1F800400) - adrs) || !(file = strtok(NULL, "")))
	{
		printf("MEMERR dump ADDR SIZE FILE (RAM address and size)\n");
		return;
	}
	while ((*file == ' ') || (*file == '\t'))
		file++;
	if (!(buf = malloc(size)))
	{
		printf("MEMERR dump: out of memory\n");
		return;
	}
	if (!Usb('D', buf, adrs, size))
		printf("MEMERR dump: USB error\n");
	else if (!(f = fopen(file, "wb")) || (fwrite(buf, 1, size, f) != size) || fclose(f))
		printf("MEMERR dump: can't write %s\n", file);
	else
		printf("MEMDUMP %08X %u %s\n", adrs, size, file);
	free(buf);
}

static void	Write(void)
{
	static uint8_t	buf[MAX_READ];
	char			*arg = strtok(NULL, " \t"), *hex;
	uint32_t		adrs, size, i;

	if (!Address(arg, &adrs) || !(hex = strtok(NULL, " \t")) || (strlen(hex) & 1) || !(size = strlen(hex) / 2) ||
		(size > MAX_READ))
	{
		printf("MEMERR write ADDR HEX (RAM address, 1-%d bytes)\n", MAX_READ);
		return;
	}
	for (i = 0; i < size; i++)
	{
		char	byte[3] = {hex[i * 2], hex[i * 2 + 1], 0};

		buf[i] = strtoul(byte, NULL, 16);
	}
	if (Usb('U', buf, adrs, size))
		printf("MEMWROTE %08X %u\n", adrs, size);
	else
		printf("MEMERR USB error\n");
}

// Remote pad: the buttons of the selected port the monitor adds to its pad buffer (prof_target), on (prof_seq bit 0)
bool	RemotePad(char *arg)
{
	uint16_t	bits;
	uint8_t		on;

	if (!arg)
		return (false);
	on = strcmp(arg, "off") ? 1 : 0;
	bits = on ? strtoul(arg, NULL, 16) : 0;
	return (USB_Process('U', (uint8_t *)&bits, MONITOR_STATE_ADRS + offsetof(MonitorState, prof_target), 2) &&
		USB_Process('U', &on, MONITOR_STATE_ADRS + offsetof(MonitorState, prof_seq), 1));
}

// Port driven from the computer: 0 (port 1) or 1 (port 2, needs monitor version 9: pad_port reuses prof_frames,
// see monitor.h). Writing it on an older monitor is harmless (ignored, port 1 stays driven)
bool	RemotePadPort(char *arg)
{
	uint8_t	port;

	if (!arg || ((*arg != '0') && (*arg != '1')) || arg[1])
		return (false);
	port = (*arg == '1') ? 1 : 0;
	return (USB_Process('U', &port, MONITOR_STATE_ADRS + offsetof(MonitorState, prof_frames), 1));
}

// The device commands (padtype, padaxes, padmove): 1 done, 0 error (printed with the prefix), -1 not one of them
int	RemotePadDevice(const char *cmd, const char *err)
{
	static int16_t	mouse_x, mouse_y;	// Motion totals sent (PadDevice.mouse_x/y)
	uint8_t			data[6], ver = 0;
	char			*a = strtok(NULL, " \t"), *b;
	uint32_t		v, i;

	if (strcmp(cmd, "padtype") && strcmp(cmd, "padaxes") && strcmp(cmd, "padmove"))
		return (-1);
	if (!USB_Process('D', &ver, MONITOR_STATE_ADRS + offsetof(MonitorState, version), 1) || (ver < 11))
	{
		printf("%s the firmware in the cartridge is too old for other controllers (debug monitor version 11)\n", err);
		return (0);
	}
	if (!strcmp(cmd, "padtype"))
	{
		if (!a || !(v = strtoul(a, NULL, 16)) || (v > 0xFF))
		{
			printf("%s padtype ID (hexadecimal: 41, 73, 53, 23, 12, 63)\n", err);
			return (0);
		}
		// Type, then the motion totals and given motion back to 0 (the monitor reads them with the type)
		memset(data, 0, sizeof(data));
		mouse_x = mouse_y = 0;
		if (!USB_Process('U', data, MONITOR_STATE_ADRS + MON_STATE_PAD_DEVICE + offsetof(PadDevice, mouse_x), 6) ||
			!USB_Process('U', data, MONITOR_STATE_ADRS + MON_STATE_PAD_DEVICE + offsetof(PadDevice, given_x), 4))
			goto usb_error;
		data[0] = (v == PAD_DIGITAL) ? 0 : v;
		if (!USB_Process('U', data, MONITOR_STATE_ADRS + MON_STATE_PAD_DEVICE, 1))
			goto usb_error;
		return (1);
	}
	if (!strcmp(cmd, "padaxes"))
	{
		if (!a || (strlen(a) != 8))
		{
			printf("%s padaxes HEX (4 bytes: 8 hexadecimal digits)\n", err);
			return (0);
		}
		for (i = 0; i < 4; i++)
		{
			char	byte[3] = {a[i * 2], a[i * 2 + 1], 0};

			data[i] = strtoul(byte, NULL, 16);
		}
		if (!USB_Process('U', data, MONITOR_STATE_ADRS + MON_STATE_PAD_DEVICE + offsetof(PadDevice, axes), 4))
			goto usb_error;
		return (1);
	}
	if (!a || !(b = strtok(NULL, " \t")))
	{
		printf("%s padmove DX DY\n", err);
		return (0);
	}
	mouse_x += atoi(a);
	mouse_y += atoi(b);
	memcpy(data, &mouse_x, 2);
	memcpy(data + 2, &mouse_y, 2);
	if (!USB_Process('U', data, MONITOR_STATE_ADRS + MON_STATE_PAD_DEVICE + offsetof(PadDevice, mouse_x), 4))
		goto usb_error;
	return (1);

usb_error:
	printf("%s USB error\n", err);
	return (0);
}

// screen FILE: the draw area (GPU info, debug monitor version 8) read from VRAM, both through the 'M' service
static void	Screen(void)
{
	char		*file = strtok(NULL, "\t\n");
	uint32_t	info[4], x, y, w, h;
	uint8_t		*pixels;
	FILE		*f;

	while (file && (*file == ' '))
		file++;
	if (!file || !*file)
	{
		printf("MEMERR screen FILE\n");
		return;
	}
	LiveDrain();
	if (!HotService(MON_CMD_GPU_INFO, (uint8_t *)info, 0, sizeof(info)))
	{
		usb_failures++;
		printf("MEMERR the draw area couldn't be read\n");
		return;
	}
	x = info[0] & 0x3FF;
	y = (info[0] >> 10) & 0x3FF;
	w = (info[1] & 0x3FF) - x + 1;
	h = ((info[1] >> 10) & 0x3FF) - y + 1;
	if ((w < 16) || (h < 16) || (x + w > 1024) || (y + h > 512))
	{
		x = y = 0;
		w = 320;
		h = 240;
	}
	if (!(pixels = malloc(w * h * 2)))
		return;
	if (!HotService('V', pixels, (x << 16) | y, (w << 16) | h))
	{
		usb_failures++;
		printf("MEMERR VRAM not read\n");
	}
	else if (!(f = fopen(file, "wb")) || (fwrite(pixels, 2, w * h, f) != w * h))
	{
		printf("MEMERR can't write %s\n", file);
		if (f)
			fclose(f);
	}
	else
	{
		fclose(f);
		printf("MEMSCREEN %u %u %u %u %s\n", x, y, w, h, file);
	}
	free(pixels);
}

void	RemotePadState(void)
{
	MonitorState	ms;

	if (!USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)))
		printf("MEMERR USB error\n");
	else if ((ms.magic != MONITOR_MAGIC) || (ms.version < 8))
		printf("MEMPAD 0 0 0 0\n");
	else
		printf("MEMPAD %d %08X %08X %d\n", (ms.prof_on && (ms.prof_armed == PROF_ARMED_PAD)) ? 1 : 0, ms.pad_buf,
			(ms.version >= 9) ? ms.prof_draw : 0, ms.version);
}

int	MemLive(void)
{
	char	*cmd, *text;
	bool	eof = false;

	setvbuf(stdout, NULL, _IONBF, 0);
	ps1_program_running = true;		// (kept if it can't be read: a program with an older kernel hook)
	USB_DetectPacketSize();
	usb_quiet = true;
	printf("MEMLIVE ready %d\n", ps1_program_running ? 1 : 0);

	while (!eof)
	{
		if (usb_failures >= USB_LOST)
		{
			printf("MEMLIVE disconnected\n");
			return (1);
		}
		if (!LiveNextLine(&text, &eof))
		{
			LiveDrain();		// The program's text and file requests, while nothing is asked
			LiveSleepMs(2);
			continue;
		}
		if (!(cmd = strtok(text, " \t")))
			continue;
		if (!strcmp(cmd, "read"))
			Read();
		else if (!strcmp(cmd, "write"))
			Write();
		else if (!strcmp(cmd, "dump"))
			Dump();
		else if (!strcmp(cmd, "pad"))
		{
			LiveDrain();
			if (!RemotePad(strtok(NULL, " \t")))
				printf("MEMERR pad BITS (hexadecimal) or pad off\n");
		}
		else if (!strcmp(cmd, "padport"))
		{
			LiveDrain();
			if (!RemotePadPort(strtok(NULL, " \t")))
				printf("MEMERR padport 0 (port 1) or padport 1 (port 2)\n");
		}
		else if (!strcmp(cmd, "padstate"))
		{
			LiveDrain();
			RemotePadState();
		}
		else if (!strcmp(cmd, "screen"))
			Screen();
		else if (!strncmp(cmd, "pad", 3) && (LiveDrain(), RemotePadDevice(cmd, "MEMERR") >= 0))
			;
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("MEMERR unknown command %s\n", cmd);
	}
	return (0);
}
