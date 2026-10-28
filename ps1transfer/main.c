/*
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

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <sys/stat.h>
#include "lodepng.h"
#include "crc.h"
#include "gdbserver.h"
#include "synthbridge.h"
#include "usblink.h"
#include "profiler.h"
#include "timlive.h"
#include "../ps1-tools/cardtool/cardserver.h"
#include "../monitor/monitor.h"

#define	PS1TRANSFER_VERSION	"1.0"	// PS1 DEV CART release (the same as the firmware and the PS1 tools)

/****************************************/

#ifdef _WIN32
#include "ftd2xx.h"
FT_HANDLE	ftdi;

#define	ftdi_tcioflush	FT_ResetDevice
#define	ftdi_free		FT_Close

uint32_t	ftdi_read_data(FT_HANDLE handle, uint8_t *data, uint32_t size)
{
	DWORD	rbyte;

	if (FT_Read(handle, data, size, &rbyte) == FT_OK)
		return (rbyte);
	return (0);
}

uint32_t	ftdi_write_data(FT_HANDLE handle, uint8_t *data, uint32_t size)
{
	DWORD	rbyte;

	if (FT_Write(handle, data, size, &rbyte) == FT_OK)
		return (rbyte);
	return (0);
}

#else
#include <ftdi.h>
struct ftdi_context *ftdi = NULL;

#define	sleep_ms(ms)	usleep(ms * 1000)

#endif

/****************************************/

#define	PCDRV_MAX_FD	16
#define	PCDRV_MAX_TRANSFER	(16 << 20)	// Bytes of one file server request (the PS1 has 2 MB, 8 MB on dev units)

FILE	*PCdrvFD[PCDRV_MAX_FD];

// File server activity ("-pcdrvlog": pcdrv=... lines for the GUI and scripts) and the files allowed ("-pcdrvany":
// any path, else only the files of the served folder)
typedef struct
{
	char		name[128];
	uint32_t	read, written;
} PcdrvFile;

static PcdrvFile	pcdrv_files[PCDRV_MAX_FD];
static bool			pcdrv_log, pcdrv_any_path;
static int			console_last_char = '\n';	// Last character of the program's text shown

static void	PcdrvLog(const char *fmt, ...)
{
	va_list	args;

	if (console_last_char != '\n')		// (the program's text stopped in the middle of a line)
		putchar('\n');
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	console_last_char = '\n';
}

// Only names in the served folder (no absolute path, drive or "..") unless -pcdrvany: a program can't read or write
// the other files of the computer
static bool	PcdrvPathAllowed(const char *name)
{
	const char	*p = name;

	if (pcdrv_any_path)
		return (true);
	if (!name[0] || (name[0] == '/') || strchr(name, ':'))
		return (false);
	while (*p)
	{
		if ((p[0] == '.') && (p[1] == '.') && ((p[2] == '/') || !p[2]))
			return (false);
		p = strchr(p, '/');
		if (!p)
			break;
		p++;
	}
	return (true);
}

// "@input": a file of the lines given to ps1transfer's standard input (e.g. by the PS1 Dev Cart Manager), which the
// program reads to get commands while the file server runs (the PS1 DEV CART VIDEO TESTER): a read returns what has
// arrived, at once (0 bytes if nothing). A thread reads the input into a ring (one writer, one reader).
#define	INPUT_RING	8192
static uint8_t				input_ring[INPUT_RING];
static volatile uint32_t	input_head, input_tail;
static int					pcdrv_input_fd = -1;
static bool					input_started;

#ifdef _WIN32
static DWORD WINAPI	InputThread(LPVOID arg)
#else
static void	*InputThread(void *arg)
#endif
{
	int	c;

	(void)arg;
	while ((c = getchar()) != EOF)
	{
		while (input_head - input_tail >= INPUT_RING)	// (full: the program doesn't read it)
#ifdef _WIN32
			Sleep(10);
#else
			usleep(10000);
#endif
		input_ring[input_head % INPUT_RING] = (uint8_t)c;
		input_head++;
	}
	return (0);
}

static void	InputStart(void)
{
	if (input_started)
		return;
	input_started = true;
#ifdef _WIN32
	CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
#else
	{
		pthread_t	t;

		pthread_create(&t, NULL, InputThread, NULL);
		pthread_detach(t);
	}
#endif
}

static uint32_t	InputRead(uint8_t *data, uint32_t size)
{
	uint32_t	n = 0;

	while ((n < size) && (input_tail != input_head))
		data[n++] = input_ring[input_tail++ % INPUT_RING];
	return (n);
}

// Opens a file for the program: file number, or -1 (refused, not found, no free file number)
static int	PcdrvOpen(char *name, const char *fmode, const char *op, const char *mode)
{
	int	i;

	if (!strcmp(name, "@input"))
	{
		for (i = 2; (i < PCDRV_MAX_FD) && PCdrvFD[i]; i++)
			;
		if (i == PCDRV_MAX_FD)
			return (-1);
		PCdrvFD[i] = stdin;		// (a marker: read from the ring)
		pcdrv_input_fd = i;
		snprintf(pcdrv_files[i].name, sizeof(pcdrv_files[i].name), "%s", name);
		pcdrv_files[i].read = pcdrv_files[i].written = 0;
		InputStart();
		return (i);
	}
	if (!PcdrvPathAllowed(name))
	{
		PcdrvLog("PCDRV: \"%s\" refused, outside the served folder (-pcdrvany allows any path)\n", name);
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"outside the folder\"\n", op, mode, name);
		return (-1);
	}
	for (i = 2; (i < PCDRV_MAX_FD) && PCdrvFD[i]; i++)
		;
	if (i == PCDRV_MAX_FD)
	{
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"too many open files\"\n", op, mode, name);
		return (-1);
	}
	if (!(PCdrvFD[i] = fopen(name, fmode)))
	{
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"%s\"\n", op, mode, name,
				(fmode[0] == 'r') ? "not found" : "can't create");
		return (-1);
	}
	snprintf(pcdrv_files[i].name, sizeof(pcdrv_files[i].name), "%s", name);
	pcdrv_files[i].read = pcdrv_files[i].written = 0;
	if (pcdrv_log)
		PcdrvLog("pcdrv=%s fd=%d mode=%s name=\"%s\"\n", op, i, mode, name);
	return (i);
}

/****************************************/

enum
{
	CMD_UNKNOWN,
	CMD_UPLOAD,
	CMD_DOWNLOAD,
	CMD_EXECUTE,
	CMD_FLASH,
	CMD_VRAM,
	CMD_CRASH,
	CMD_PEEK,
	CMD_POKE,
	CMD_MEMLIVE,
	CMD_HOT,
	CMD_INFO,
	CMD_FLASH_ROM,
	CMD_SYNTH,
	CMD_CARD_LIVE,
	CMD_RELAY,
	CMD_NET,
	CMD_PROF,
	CMD_SPU,
	CMD_BOOT_DISC
};

/****************************************/

void	*SystemGetFile(char *filename, int *sizeptr)
{
	FILE	*f;
	int		size;
	void	*buffer = NULL;

	if (f = fopen(filename, "rb"))
	{
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sizeptr)
			*sizeptr = size;
		if (buffer = malloc(size))
			fread(buffer, 1, size, f);
		else
			printf("Memory Allocation Error for file '%s' of size %d\n", filename, size);
		fclose(f);
	}
	else
		printf("File '%s' not found !\n", filename);

	return (buffer);
}

uint32_t	atohex(char *str)
{
	uint32_t	value = 0;

	if ((str[0] == '0') && (str[1] == 'x'))
		str = &str[2];
	while (42)
	{
		if (((*str) >= '0') && ((*str) <= '9'))
		{
			value <<= 4;
			value |= (*str) - '0';
		}
		else if (((*str) >= 'A') && ((*str) <= 'F'))
		{
			value <<= 4;
			value |= ((*str) - 'A') + 0xA;
		}
		else if (((*str) >= 'a') && ((*str) <= 'f'))
		{
			value <<= 4;
			value |= ((*str) - 'a') + 0xA;
		}
		else
			break;
		str++;
	}

	return (value);
}

/****************************************/

#define	USB_READ_SIZE	2048	// Reads are done by whole packet (USB_ReadData waits for all the bytes)
#define	USB_WRITE_SIZE	65536	// A whole packet in one write (16KB: 585 KB/s upload, 2KB writes: 555), the USB driver handles the chip buffer flow control
#define	USB_LATENCY_MS	1		// FTDI default is 16ms, which delayed each packet CRC/ACK byte sent by the PS1
#define	USB_READ_TIMEOUT	5		// Seconds without data before a read fails (e.g. PS1 running a program, not answering)
#define	USB_READ_POLL_MS	100		// Windows: FT_Read returns after this time so the timeout can be checked
#define	MAX_PACKET_SIZE	2048	// 2k max for reliability
#define	RESET_QUIET_MS	2500	// -r after a stopped transfer: the PS1 quiet this long (its acknowledgment wait ended)
#define	RESET_DRAIN_S	12		// (at most: a program sending all the time, e.g. its printf)

// Execute command param: TTY mode set by the kernel hook when the program starts, see usb.h
#define	EXEC_TTY_MODE_USB	0	// printf/TTY redirected to USB (default)
#define	EXEC_TTY_MODE_BIOS	1	// BIOS TTY device kept untouched
#define	EXEC_TTY_MODE_SIO	2	// Serial port TTY driver installed (like the Net Yaroze boot disc)
#define	EXEC_TTY_MODE_LINK	3	// The same TTY driver on the USB link instead of the serial port (-relay)

/****************************************/
// USB device selection (by index or serial number, for multiple PS1 DEV CART devices)

#ifdef _WIN32
// The PS1 answers each 2KB packet with a single CRC byte, which the FTDI chip only sends to the computer
// when its latency timer expires, so a short latency is what makes transfers fast
void	USB_SetLatency(void)
{
	FT_SetLatencyTimer(ftdi, USB_LATENCY_MS);
	FT_SetTimeouts(ftdi, USB_READ_POLL_MS, USB_READ_TIMEOUT * 1000);	// Default is no timeout: a missing answer froze ps1transfer
}

bool	USB_OpenDevice(int deviceid, char *serial)
{
	if (serial)
		return (FT_OpenEx(serial, FT_OPEN_BY_SERIAL_NUMBER, &ftdi) == FT_OK);
	return (FT_Open(deviceid, &ftdi) == FT_OK);
}

void	USB_ListDevices(void)
{
	DWORD		i, n = 0, flags, type, id, loc;
	char		serial[16], desc[64];
	FT_HANDLE	handle;

	if ((FT_CreateDeviceInfoList(&n) != FT_OK) || !n)
	{
		printf("No USB device found.\n");
		return;
	}
	for (i = 0; i < n; i++)
	{
		if (FT_GetDeviceInfoDetail(i, &flags, &type, &id, &loc, serial, desc, &handle) == FT_OK)
			printf("-z %lu  -n %-12s %s%s\n", i, serial, desc, (flags & FT_FLAGS_OPENED) ? " (in use)" : "");
	}
}
#else
// The PS1 answers each 2KB packet with a single CRC byte, which the FTDI chip only sends to the computer
// when its latency timer expires, so a short latency is what makes transfers fast
void	USB_SetLatency(void)
{
	ftdi_set_latency_timer(ftdi, USB_LATENCY_MS);
}

bool	USB_OpenDevice(int deviceid, char *serial)
{
	int	ret;

	if (serial)
		ret = ftdi_usb_open_desc(ftdi, 0x0403, 0x6001, NULL, serial);
	else
		ret = ftdi_usb_open_desc_index(ftdi, 0x0403, 0x6001, NULL, NULL, deviceid);
	if (ret < 0)
		fprintf(stderr, "Unable to open USB device: %d (%s)\n", ret, ftdi_get_error_string(ftdi));
	return (ret >= 0);
}

void	USB_ListDevices(void)
{
	struct ftdi_device_list	*list, *dev;
	char					serial[64], desc[64];
	int						i;

	if (ftdi_usb_find_all(ftdi, &list, 0x0403, 0x6001) <= 0)
	{
		printf("No USB device found.\n");
		return;
	}
	for (dev = list, i = 0; dev; dev = dev->next, i++)
	{
		if (ftdi_usb_get_strings(ftdi, dev->dev, NULL, 0, desc, sizeof(desc), serial, sizeof(serial)) < 0)
		{
			strcpy(desc, "?");
			strcpy(serial, "?");
		}
		printf("-z %d  -n %-12s %s\n", i, serial, desc);
	}
	ftdi_list_free(&list);
}
#endif

uint8_t	USB_cmd[1+4+4+1];
uint32_t	usb_big_packet_size;	// 0 = firmware only supports MAX_PACKET_SIZE packets
bool		ps1_program_running;	// A program runs: only the kernel hook answers (2KB packets)
uint8_t		fw_features;			// FEATURE_* of the firmware, 0 with the original firmware
bool		vram_live;				// -live: -v reads the screen also at the menu
bool		vram_auto;				// -v auto: the screen on the TV, told by the program (TIM viewer, timlive.c)
const char	*spu_file;				// -spu: file written with the SPU RAM and registers
uint32_t	hook_packet_size = MAX_PACKET_SIZE;	// Packets of the kernel hook/debug monitor (file server, live access)
bool		hook_packet_detected;

// The firmware stores in the kernel hook params: is_enabled (a program runs), big packet size (log2) and features,
// the original firmware leaves 0 in the last 2 bytes
#define	KERNEL_HOOK_PARAMS_ADRS	0x8000DF70
#define	FEATURE_LIVE_ACCESS		(1 << 0)	// 'D'/'U'/'V' also while a program runs, see usb.h
#define	FEATURE_HOOK_BIG_PACKETS	(1 << 2)	// The kernel hook and the debug monitor also use big packets

bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);

// A second cartridge open at the same time (USB link relay): USB_Process uses the selected one
void	*USB_GetHandle(void)
{
	return ((void *)ftdi);
}

void	USB_SetHandle(void *handle)
{
	ftdi = handle;
}

void	*USB_OpenOther(char *serial)
{
	void	*current = (void *)ftdi, *handle = NULL;

#ifdef _WIN32
	if (USB_OpenDevice(0, serial))
	{
		FT_Purge(ftdi, FT_PURGE_RX | FT_PURGE_TX);
		USB_SetLatency();
		handle = (void *)ftdi;
	}
#else
	if ((ftdi = ftdi_new()))
	{
		if (USB_OpenDevice(0, serial))
		{
			USB_SetLatency();
			handle = (void *)ftdi;
		}
		else
			ftdi_free(ftdi);
	}
#endif
	ftdi = current;
	return (handle);
}

void	USB_CloseOther(void *handle)
{
	if (!handle)
		return;
#ifdef _WIN32
	FT_Close((FT_HANDLE)handle);
#else
	ftdi_usb_close((struct ftdi_context *)handle);
	ftdi_free((struct ftdi_context *)handle);
#endif
}

void	USB_DetectPacketSize(void)
{
	uint8_t	params[4];

	usb_big_packet_size = 0;
	if (USB_Process('D', params, KERNEL_HOOK_PARAMS_ADRS, sizeof(params)))
	{
		ps1_program_running = params[0];
		fw_features = params[3];
		if (!ps1_program_running && (params[2] >= 12) && (params[2] <= 16))
			usb_big_packet_size = 1 << params[2];
		// Packets of the kernel hook while a program runs (the firmware's params stay the same)
		hook_packet_size = ((fw_features & FEATURE_HOOK_BIG_PACKETS) && (params[2] >= 12) && (params[2] <= 16)) ?
			(1 << params[2]) : MAX_PACKET_SIZE;
		hook_packet_detected = true;
	}
}

// The menu shows the device ID (USB serial number, only readable from the computer), sent with the 'I'
// command when the menu runs (the firmware saves it in the cartridge flash when it changes)
#define	FEATURE_DEVICE_ID		(1 << 1)
#define	FEATURE_BOOT_DISC		(1 << 3)	// 'C' command at the menu: the CD drive unlocked and the disc booted
#define	DEVICE_ID_SIZE			16

// USB serial number of the opened cartridge (at least 64 bytes)
bool	USB_GetSerial(char *serial)
{
	memset(serial, 0, 64);
#ifdef _WIN32
	{
		FT_DEVICE	type;
		DWORD		id;
		char		desc[64];

		if (FT_GetDeviceInfo(ftdi, &type, &id, serial, desc, NULL) != FT_OK)
			return (false);
	}
#else
	if (ftdi_usb_get_strings2(ftdi, libusb_get_device(ftdi->usb_dev), NULL, 0, NULL, 0, serial, 64) < 0)
		return (false);
#endif
	return (serial[0] != 0);
}

void	USB_SendDeviceId(void)
{
	char	serial[64];

	if (USB_GetSerial(serial))
		USB_Process('I', (uint8_t *)serial, 0, DEVICE_ID_SIZE);
}

// Read exactly "size" bytes (reads may return less data than requested), fails after USB_READ_TIMEOUT seconds without data
bool	USB_ReadData(uint8_t *data, uint32_t size)
{
	time_t	last = time(NULL);
	int		n;

	while (size)
	{
		n = ftdi_read_data(ftdi, data, size);
		if (n < 0)
			return (false);
		if (n)
		{
			data += n;
			size -= n;
			last = time(NULL);
		}
		else if (time(NULL) - last > USB_READ_TIMEOUT)
			return (false);
	}
	return (true);
}

// USB link relay: "exchange" command 'X' of the kernel hook, on a given cartridge (thread safe, the
// relay drives each cartridge from its own thread): writes wsize bytes at adrs, then reads the rsize bytes that
// follow, in one transfer (both sizes up to MAX_PACKET_SIZE)
static bool	USB_ReadFrom(void *handle, uint8_t *data, uint32_t size)
{
	time_t	last = time(NULL);
	int		n;

	while (size)
	{
#ifdef _WIN32
		n = ftdi_read_data((FT_HANDLE)handle, data, size);
#else
		n = ftdi_read_data((struct ftdi_context *)handle, data, size);
#endif
		if (n < 0)
			return (false);
		if (n)
		{
			data += n;
			size -= n;
			last = time(NULL);
		}
		else if (time(NULL) - last > USB_READ_TIMEOUT)
			return (false);
	}
	return (true);
}

static bool	USB_WriteTo(void *handle, uint8_t *data, uint32_t size)
{
#ifdef _WIN32
	return (ftdi_write_data((FT_HANDLE)handle, data, size) == size);
#else
	return (ftdi_write_data((struct ftdi_context *)handle, data, size) == (int)size);
#endif
}

void	PC_FileServer(void);

volatile uint32_t	link_crc_errors;	// Exchange packets received wrong (USB link relay, see ExchangeCmd)

// letter: 'X' (the kernel hook's), or MON_CMD_EXCHANGE for the relay of a debug session (also answered by the debug
// monitor while the program is stopped). served: what arrives before the answer is handled, not taken for it: a stop
// notification or a PCDRV request (Data Link Escape: PC_FileServer, the selected cartridge must be this one), the
// program's text.
// The PS1 answers it in an interrupt (kernel hook), with the program stopped meanwhile: the command, the written packet
// and both acknowledgements ('N') go in one USB write, so that it never waits there for the computer (three USB round
// trips, a few milliseconds at each exchange: the program's frames disturbed). The CRCs are checked afterwards: a
// written packet received wrong is written again whole by the next exchange, a read one received wrong is not used
// (rdata unchanged).
// Debug session (served): the program can stop (breakpoint, pause) before the command is answered; the monitor's stop
// notification then takes the command's first byte for the computer's answer, and the exchange is not answered. So the
// command goes alone, and the rest once it is answered ('G': the kernel hook or the monitor then goes to the end); a
// stop notification before: the exchange is skipped (true, nothing read: the next one writes everything again).
static bool	ExchangeCmd(void *handle, char letter, bool served, uint32_t adrs, uint8_t *wdata, uint32_t wsize,
	uint8_t *rdata, uint32_t rsize)
{
	uint8_t			out[1+4+4+1 + MAX_PACKET_SIZE + 2], in[MAX_PACKET_SIZE];	// (one relay thread per cartridge)
	uint8_t			*cmd = out, ack, crc;
	uint32_t		size = (wsize << 16) | rsize;
	int				n;
	bool			stopped;

	if ((wsize > MAX_PACKET_SIZE) || (rsize > MAX_PACKET_SIZE))
		return (false);
	cmd[0] = letter;
	cmd[1] = adrs >> 24;
	cmd[2] = adrs >> 16;
	cmd[3] = adrs >> 8;
	cmd[4] = adrs;
	cmd[5] = size >> 24;
	cmd[6] = size >> 16;
	cmd[7] = size >> 8;
	cmd[8] = size;
	cmd[9] = CRC8_Compute(cmd, 1+4+4);
	memcpy(out + 10, wdata, wsize);
	out[10 + wsize] = 'N';		// Written packet taken (its CRC is checked below)
	out[11 + wsize] = 'N';		// Read packet taken
	if (!USB_WriteTo(handle, out, served ? 10 : 12 + wsize))
		return (false);
	// The answer: 'G', or escaped "DLE g" (the firmware answers the command after a 'Q' that way, also after the 'Q' alone
	// asking whether it has escaped answers: USB_EscapedAnswers); "DLE q": that 'Q''s own answer, late
	for (n = 0; ; n++)
	{
		if (!USB_ReadFrom(handle, &ack, 1) || (ack == 'B') || (n > 65536))
			return (false);
		if (ack == 'G')
			break;
		if (ack == 16)
		{
			if (!USB_ReadFrom(handle, &ack, 1) || (ack == 'b'))
				return (false);
			if (ack == 'g')
				break;
			if (ack == 'q')
				continue;
			if (!served)
				return (false);
			stopped = mon_stop_pending;
			PC_FileServerCmd(ack);		// PCDRV request or stop notification (its command byte read)
			if (mon_stop_pending && !stopped)
				return (true);			// The program stopped: the exchange isn't answered (skipped)
			continue;
		}
		if (!served)
			return (false);
		putchar(ack);
	}
	if (served && !USB_WriteTo(handle, out + 10, 2 + wsize))	// (answered: the rest at once)
		return (false);
	if (!USB_ReadFrom(handle, &crc, 1) || !USB_ReadFrom(handle, in, rsize) || !USB_ReadFrom(handle, &ack, 1))
		return (false);
	if (crc != CRC8_Compute(wdata, wsize))
		link_crc_errors++;
	if (ack != CRC8_Compute(in, rsize))
		link_crc_errors++;
	else
		memcpy(rdata, in, rsize);
	return (true);
}

bool	USB_Exchange(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize)
{
	return (ExchangeCmd(handle, 'X', false, adrs, wdata, wsize, rdata, rsize));
}

// The relay of a debug session (gdbserver.c): the debugged cartridge's exchange, the stop notifications kept
bool	USB_ExchangeServed(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize)
{
	return (ExchangeCmd(handle, MON_CMD_EXCHANGE, true, adrs, wdata, wsize, rdata, rsize));
}

// Escaped answers (see usb.h): 'Q' before each command, the firmware answers "DLE g" / "DLE b" instead of 'G' / 'B', so
// text a program prints on the USB tty just before the answer can't be taken for it. Known per cartridge (the relay
// opens two): asked once with a 'Q' alone, answered "DLE q" by a firmware that does it (the original and older PS1 DEV
// CART firmwares ignore it).
#define	ESC_HANDLES		4
#define	ESC_WAIT_MS		200		// The kernel hook answers at its next interrupt (a frame at most), the menu at once

static struct
{
	void	*handle;
	int		state;		// 1 escaped answers, -1 not supported
}	esc_tab[ESC_HANDLES];

static int	*EscState(void)
{
	int	i;

	for (i = 0; i < ESC_HANDLES; i++)
		if (esc_tab[i].handle == (void *)ftdi)
			return (&esc_tab[i].state);
	for (i = 0; i < ESC_HANDLES; i++)
		if (!esc_tab[i].handle)
		{
			esc_tab[i].handle = (void *)ftdi;
			esc_tab[i].state = 0;
			return (&esc_tab[i].state);
		}
	esc_tab[0].handle = (void *)ftdi;	// (more cartridges than that: asked again)
	esc_tab[0].state = 0;
	return (&esc_tab[0].state);
}

bool	USB_ReadByteNoWait(uint8_t *c);

static uint32_t	NowMs(void)
{
#ifdef _WIN32
	return (GetTickCount());
#else
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

// After an upload (its progress line ended): its transfer rate and time
static void	PrintUploadTime(uint32_t bytes, uint32_t start_ms)
{
	uint32_t	ms = NowMs() - start_ms;

	printf((ms < 10000) ? "\nTransfer rate: %u KB/s\nTime taken: %.2f seconds\n" :
		"\nTransfer rate: %u KB/s\nTime taken: %.1f seconds\n",
		ms ? (uint32_t)((uint64_t)bytes * 1000 / 1024 / ms) : 0, ms / 1000.0);
}

static int	USB_EscapedAnswers(void)
{
	int			*state = EscState();
	uint8_t		q = 'Q', c;
	bool		dle = false;
	uint32_t	start;

	if (*state)
		return (*state);
	*state = -1;
	if (ftdi_write_data(ftdi, &q, 1) != 1)
		return (*state);
	// (the bytes before are the program's text: not shown, as before a command)
	for (start = NowMs(); NowMs() - start < ESC_WAIT_MS; )
	{
		if (!USB_ReadByteNoWait(&c))
		{
#ifdef _WIN32
			Sleep(1);
#else
			usleep(1000);
#endif
			continue;
		}
		if (dle && (c == 'q'))
			return (*state = 1);
		dle = (c == 16);
	}
	return (*state);
}

void	USB_DetectEscaped(void)
{
	USB_EscapedAnswers();
}

// "-progress" prints machine readable lines (progress=done/total) for the GUI instead of the text
bool	machine_progress;
bool	usb_quiet;		// No progress text at all (-synth: its answers are read line by line)

void	ShowProgress(uint32_t remaining, uint32_t total)
{
	if (machine_progress)
		printf("progress=%u/%u\n", total - remaining, total);
	else
		printf("%u bytes remaining...\r", remaining);
	fflush(stdout);
}

bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	uint8_t		retry, ack, crc8, *odata;
	uint32_t	lsize, usize, packet_size = MAX_PACKET_SIZE, header_size = size, total;
	bool		ack_ok;
	int			skipped;
	bool		escaped;
	// Small transfers (e.g. crash report reads) and hot reloads without progress text
	bool		show_progress = (size > MAX_PACKET_SIZE) && (cmd != MON_CMD_VRAM_WRITE) && (cmd != MON_CMD_SPU_WRITE) &&
				!usb_quiet;

	// VRAM read: adrs = x << 16 | y, size = w << 16 | h, one packet per line (same protocol as a download)
	// Hot reload VRAM write: adrs = x << 16 | y, size = w << 16 | h, packets of whole lines (see monitor/monitor.h)
	if (cmd == MON_CMD_VRAM_WRITE)
	{
		packet_size = MON_VRAM_LINES(size >> 16) * (size >> 16) * 2;
		size = (size >> 16) * (size & 0xFFFF) * 2;
	}
	if ((cmd == MON_CMD_SPU_WRITE) || (cmd == MON_CMD_SPU_READ))
		packet_size = MON_SPU_PACKET;

	if (cmd == 'V')
	{
		packet_size = (size >> 16) * 2;
		size = packet_size * (size & 0xFFFF);
	}

	total = size;

	// Big transfers use big packets when the firmware supports them (same commands in lowercase)
	if (usb_big_packet_size && ((cmd == 'U') || (cmd == 'D')) && (size > MAX_PACKET_SIZE))
	{
		packet_size = usb_big_packet_size;
		cmd += 'a' - 'A';
	}
	else if ((cmd == 'U') || (cmd == 'D'))	// Kernel hook (a program runs): big packets with FEATURE_HOOK_BIG_PACKETS
		packet_size = hook_packet_size;

	USB_cmd[0] = cmd;
	USB_cmd[1] = (adrs >> 24) & 0xFF;
	USB_cmd[2] = (adrs >> 16) & 0xFF;
	USB_cmd[3] = (adrs >> 8) & 0xFF;
	USB_cmd[4] = adrs & 0xFF;
	USB_cmd[5] = (header_size >> 24) & 0xFF;
	USB_cmd[6] = (header_size >> 16) & 0xFF;
	USB_cmd[7] = (header_size >> 8) & 0xFF;
	USB_cmd[8] = header_size & 0xFF;
	USB_cmd[9] = CRC8_Compute(USB_cmd, 1+4+4);

	// Send Command Packet (after 'Q' for an escaped answer)
	escaped = (USB_EscapedAnswers() > 0);
	if (escaped)
	{
		uint8_t	qcmd[1+1+4+4+1] = {'Q'};

		memcpy(qcmd + 1, USB_cmd, sizeof(USB_cmd));
		if (ftdi_write_data(ftdi, qcmd, sizeof(qcmd)) != sizeof(qcmd))
			return (false);
	}
	else if (ftdi_write_data(ftdi, USB_cmd, 1+4+4+1) != 1+4+4+1)
		return (false);
	if (cmd == 'R')		// Asked again after the reset (e.g. a RAM test firmware replaced by the one in flash)
		*EscState() = 0;

	retry = 3;

	// Get Ack Packet. Before it: text the program printed on its USB tty (e.g. a link game run with the default tty,
	// writing every frame), skipped; while profiling, the profiler packets (DLE + type, see profiler.c); "DLE q"
	// (answer to the 'Q'). Escaped answer: "DLE g" / "DLE b", the only one accepted when the firmware has them (text
	// can't be taken for it). Otherwise 'G' / 'B' (a 'G' or 'B' in text just before is taken for it: older firmwares).
	ack_ok = USB_ReadData(&ack, 1);
	for (skipped = 0; ack_ok && (skipped < 4096); skipped++)
	{
		if (ack == 16)
		{
			if (!(ack_ok = USB_ReadData(&ack, 1)))
				break;
			if ((ack == 'g') || (ack == 'b'))
			{
				ack -= 'a' - 'A';
				break;
			}
			if (prof_active && (ack != 'q') && !(ack_ok = ProfReadPacketType(ack)))
				break;
		}
		else if (!escaped && ((ack == 'G') || (ack == 'B')))
			break;
		ack_ok = USB_ReadData(&ack, 1);
	}
	if (ack_ok)
	{
		if (ack == 'G')
		{
			if ((cmd == 'D') || (cmd == 'd') || (cmd == 'V') || (cmd == MON_CMD_GPU_INFO) || (cmd == MON_CMD_SPU_READ))
			{
				while (size)	// get all packets
				{
					if (show_progress)
						ShowProgress(size, total);
					lsize = (size > packet_size) ? packet_size : size;
					odata = data;
					// Get packet by blocks of USB_READ_SIZE
					while (lsize)
					{
						usize = (lsize > USB_READ_SIZE) ? USB_READ_SIZE : lsize;
						if (!USB_ReadData(data, usize))
							return (false);
						data += usize;
						lsize -= usize;
					}
					// Check CRC
					lsize = (size > packet_size) ? packet_size : size;
					if (USB_ReadData(&crc8, 1))
					{
						if (crc8 == CRC8_Compute(odata, lsize))
						{
							ack = 'N';	// next
							size -= lsize;
						}
						else
						{
							printf("\nretry...\n");
							ack = 'A';	// again
							//ftdi_tcioflush(ftdi);
							data = odata;
							retry--;
							if (!retry)
								return (false);
						}
					}
					else
						return (false);
					if (ftdi_write_data(ftdi, &ack, 1) != 1)
						return (false);
				}
			}
			else if (cmd != 'F')
			{
				while (size)	// get all packets
				{
					if (show_progress)
						ShowProgress(size, total);
					lsize = (size > packet_size) ? packet_size : size;
					odata = data;
					// Send packet by blocks of USB_WRITE_SIZE
					while (lsize)
					{
						usize = (lsize > USB_WRITE_SIZE) ? USB_WRITE_SIZE : lsize;
						if (ftdi_write_data(ftdi, data, usize) != usize)
							return (false);
						data += usize;
						lsize -= usize;
					}
					// Check CRC
					lsize = (size > packet_size) ? packet_size : size;
					if (USB_ReadData(&crc8, 1))
					{
						if (crc8 == CRC8_Compute(odata, lsize))
						{
							ack = 'N';	// next
							size -= lsize;
						}
						else
						{
							printf("\nretry...\n");
							ack = 'A';	// again
							//ftdi_tcioflush(ftdi);
							data = odata;
							retry--;
							if (!retry)
								return (false);
						}
					}
					else
						return (false);
					if (ftdi_write_data(ftdi, &ack, 1) != 1)
						return (false);
				}
			}
			if (show_progress && machine_progress)
				ShowProgress(0, total);	// Done (the progress is shown at the start of each packet)
			return (true);
		}
	}
	return (false);
}

// GDB server (gdbserver.c): bytes from the PS1 without waiting, raw bytes to the PS1
bool	USB_ReadByteNoWait(uint8_t *c)
{
#ifdef _WIN32
	DWORD	queued = 0;

	if ((FT_GetQueueStatus(ftdi, &queued) != FT_OK) || !queued)
		return (false);
#endif
	return (ftdi_read_data(ftdi, c, 1) == 1);
}

bool	USB_WriteBytes(uint8_t *data, uint32_t size)
{
	return (ftdi_write_data(ftdi, data, size) == size);
}

/****************************************/
// PC File Server

bool		mon_stop_pending;	// Debug monitor stop notification ('B' request), see gdbserver.c
uint32_t	mon_stop_regs, mon_stop_info;

// first: the command byte when it was already read (profiler), -1 otherwise
void	PC_FileServerCmd(int first)
{
	uint8_t		*data, *ptr = NULL, answer, param[1+4+4+1];
	uint32_t	data_size, odata_size;
	uint32_t	fd;
	int			i;

	data = NULL;
	if (first < 0)
	{
		if (!USB_ReadData(param, 1))
			return;
		if ((param[0] == 'q') || (param[0] == 'g') || (param[0] == 'b'))	// A late escaped answer (see USB_Process)
			return;
		first = param[0];
	}
	param[0] = first;
	if (USB_ReadData(param + 1, sizeof(param) - 1))
	{
		if (param[1+4+4] == CRC8_Compute(param, 1+4+4))
			answer = 'G';	// Good ACK
		else
			answer = 'B';	// Bad command

		fd = (param[1] << 24) | (param[2] << 16) | (param[3] << 8) | param[4];
		odata_size = data_size = (param[5] << 24) | (param[6] << 16) | (param[7] << 8) | param[8];

		// Send ACK
		if (ftdi_write_data(ftdi, &answer, 1) != 1)
			goto pcdrvout;

		if (answer == 'G')
		{
			uint8_t		*odata, crc8;
			uint32_t	lsize, usize;
			int			ret_value = -1;

			// (zeroed: a short read sends zeros after the file's bytes, not what was in the computer's memory; one more
			// byte: the end of a file name is always there)
			if ((param[0] == 'O') || (param[0] == 'C') || (param[0] == 'R') || (param[0] == 'W'))
			{
				if ((data_size > PCDRV_MAX_TRANSFER) || !(ptr = data = calloc(data_size + 1, 1)))
				{
					printf("PCDRV: transfer of %u bytes refused\n", (unsigned int)data_size);
					goto pcdrvout;
				}
			}

			// Read data
			if ((param[0] == 'O') || (param[0] == 'C') || (param[0] == 'W'))
			{
				while (data_size)
				{
					lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
					odata = ptr;
					while (lsize)
					{
						usize = (lsize > USB_READ_SIZE) ? USB_READ_SIZE : lsize;
						if (!USB_ReadData(ptr, usize))
							goto pcdrvout;
						ptr += usize;
						lsize -= usize;
					}
					// Check CRC
					lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
					if (USB_ReadData(&crc8, 1))
					{
						if (crc8 == CRC8_Compute(odata, lsize))
						{
							answer = 'N';	// next
							data_size -= lsize;
						}
						else
						{
							answer = 'A';	// again
							ptr = odata;
						}
					}
					else
						goto pcdrvout;
					if (ftdi_write_data(ftdi, &answer, 1) != 1)
						goto pcdrvout;
				}
			}

			// Convert path
			if ((param[0] == 'O') || (param[0] == 'C'))
				for (i = 0; data[i]; i++)
					if (data[i] == '\\')
						data[i] = '/';

			switch (param[0])	// Process command
			{
				case 'I':	// Init
					ret_value = 0;
					if (pcdrv_log)
						PcdrvLog("pcdrv=init\n");
				break;

				case 'O':	// Open file
					ret_value = PcdrvOpen((char *)data, (fd == 0) ? "rb" : ((fd == 1) ? "wb" : "rb+"), "open",
						(fd == 0) ? "read" : ((fd == 1) ? "write" : "readwrite"));
				break;

				case 'C':	// Create file
					ret_value = PcdrvOpen((char *)data, "wb+", "create", "readwrite");
				break;

				case 'S':	// Seek file
					answer = (fd >> 24) & 0xFF;
					fd &= 0xFFFFFF;

					if ((fd < PCDRV_MAX_FD) && ((int)fd != pcdrv_input_fd))
					{
						if (PCdrvFD[fd])
						{
							fseek(PCdrvFD[fd], (int)data_size, (answer == 0) ? SEEK_SET : ((answer == 1) ? SEEK_CUR : SEEK_END));
							ret_value = ftell(PCdrvFD[fd]);
						}
					}
				break;

				case 'R':	// Read file
					if ((fd < PCDRV_MAX_FD) && PCdrvFD[fd] && ((int)fd == pcdrv_input_fd))
						ret_value = InputRead(data, data_size);
					else if ((fd < PCDRV_MAX_FD) && PCdrvFD[fd])
					{
						ret_value = fread(data, 1, data_size, PCdrvFD[fd]);
						pcdrv_files[fd].read += ret_value;
					}
					// Send Data (also for a file not open: the PS1 waits for the bytes, then gets -1)
					{
						{
							while (data_size)
							{
								lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
								odata = ptr;
								while (lsize)
								{
									usize = (lsize > USB_WRITE_SIZE) ? USB_WRITE_SIZE : lsize;
									if (ftdi_write_data(ftdi, ptr, usize) != usize)
										goto pcdrvout;
									ptr += usize;
									lsize -= usize;
								}
								// Check CRC
								lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
								if (USB_ReadData(&crc8, 1))
								{
									if (crc8 == CRC8_Compute(odata, lsize))
									{
										answer = 'N';	// next
										data_size -= lsize;
									}
									else
									{
										answer = 'A';	// again
										ptr = odata;
									}
								}
								else
									goto pcdrvout;
								if (ftdi_write_data(ftdi, &answer, 1) != 1)
									goto pcdrvout;
							}
						}
					}
				break;

				case 'W':	// Write file
					if (fd < PCDRV_MAX_FD)
					{
						if (PCdrvFD[fd])
						{
							ret_value = fwrite(data, 1, odata_size, PCdrvFD[fd]);
							pcdrv_files[fd].written += ret_value;
						}
					}
				break;

				case 'c':	// Close file
					if (fd < PCDRV_MAX_FD)
					{
						if (PCdrvFD[fd])
						{
							ret_value = ((int)fd == pcdrv_input_fd) ? 0 : fclose(PCdrvFD[fd]);
							if ((int)fd == pcdrv_input_fd)
								pcdrv_input_fd = -1;
							PCdrvFD[fd] = NULL;
							if (pcdrv_log)
								PcdrvLog("pcdrv=close fd=%d name=\"%s\" read=%u written=%u\n", (int)fd,
									pcdrv_files[fd].name, pcdrv_files[fd].read, pcdrv_files[fd].written);
						}
					}
				break;

				case 'B':	// Debug monitor: the program has stopped (registers address, reason << 8 | ExcCode)
					mon_stop_regs = fd;
					mon_stop_info = odata_size;
					mon_stop_pending = true;
					ret_value = 0;
				break;
			}

			// Send return value
			param[0] = (ret_value >> 24) & 0xFF;
			param[1] = (ret_value >> 16) & 0xFF;
			param[2] = (ret_value >> 8) & 0xFF;
			param[3] = ret_value & 0xFF;
			ftdi_write_data(ftdi, param, 4);
		}
	}
pcdrvout:
	if (data)
		free(data);
}

void	PC_FileServer(void)
{
	PC_FileServerCmd(-1);
}

/****************************************/
// Net Yaroze runtime library upload (normally loaded by the Net Yaroze boot disc)

#define	LIBPS_DEFAULT_ADRS	0x80010000

// Accepts the original "libps.exe" from the Net Yaroze CD-ROM (PS-X EXE) or the stripped raw "libps.bin"
// The Net Yaroze boot disc runs the start code of the library (libps.exe + 0x30: callbacks, pads with
// InitPAD on the buffers of GetPadBuf, ...) before the program, which doesn't do it itself (without it, the pads read
// "no controller"). ps1transfer executes a stub that calls it, then jumps to the program entry point.
#define	LIBPS_INIT_OFFSET	0x30
#define	LIBPS_INIT_CODE		0x27BDFFE8		// addiu sp, sp, -24: first instruction of the start code (checked)
#define	LIBPS_STUB_SIZE		24

uint32_t	libps_init;		// Address of the library start code, 0: unknown library
uint32_t	libps_adrs, libps_size;	// Where the library was uploaded

// The stub, just below the program (unused by Net Yaroze programs): returns its address, 0 on USB error
uint32_t	UploadNetYarozeStart(uint32_t exe_adrs, uint32_t entry)
{
	uint32_t	stub[LIBPS_STUB_SIZE / 4], adrs = exe_adrs - 32;

	stub[0] = 0x0C000000 | ((libps_init >> 2) & 0x03FFFFFF);	// jal libps_init
	stub[1] = 0;												// nop
	stub[2] = 0x3C080000 | (entry >> 16);						// lui t0, entry >> 16
	stub[3] = 0x35080000 | (entry & 0xFFFF);					// ori t0, t0, entry & 0xFFFF
	stub[4] = 0x01000008;										// jr t0
	stub[5] = 0;												// nop
	printf("Net Yaroze library start code: 0x%X, then the program at 0x%X\n", libps_init, entry);
	return (USB_Process('U', (uint8_t *)stub, adrs, sizeof(stub)) ? adrs : 0);
}

bool	UploadNetYarozeLibrary(char *filename, uint32_t exe_adrs, uint32_t exe_size)
{
	uint32_t	start_ms;
	uint8_t		*data, *ptr;
	uint32_t	adrs = LIBPS_DEFAULT_ADRS;
	int			size;
	bool		ret;

	if (!(data = SystemGetFile(filename, &size)))
		return (false);

	ptr = data;
	if ((size > 2048) && !memcmp(data, "PS-X EXE", 8))
	{
		adrs = *((uint32_t*)&data[8+4+4+(2*4)]);	// Text address from the exe header
		ptr = &data[2048];
		size -= 2048;
	}

	if ((exe_adrs < adrs + size) && (exe_adrs + exe_size > adrs))
	{
		printf("The executable (0x%X-0x%X) overlaps the Net Yaroze library (0x%X-0x%X) !\n",
			exe_adrs, exe_adrs + exe_size, adrs, adrs + size);
		free(data);
		return (false);
	}

	libps_init = 0;
	if ((size > LIBPS_INIT_OFFSET + 4) && (*(uint32_t *)&ptr[LIBPS_INIT_OFFSET] == LIBPS_INIT_CODE))
		libps_init = adrs + LIBPS_INIT_OFFSET;
	else
		printf("Unknown Net Yaroze library version: its start code isn't run (pads may not work).\n");

	libps_adrs = adrs;
	libps_size = size;
	printf("Uploading Net Yaroze library %u bytes to 0x%X...\n", size, adrs);
	start_ms = NowMs();
	ret = USB_Process('U', ptr, adrs, size);
	if (ret)
		PrintUploadTime(size, start_ms);
	else
		printf("\n");
	free(data);

	return (ret);
}

/****************************************/
// Net Yaroze programs as the Net Yaroze tools make them: the ECOFF executable of the compiler ("main") and the
// "auto" script of siocons, which loads it and the data files ("local load main", "local dload file address",
// "go"). A program converted to a PS-X EXE (eco2exe, or yarexe, which also includes the library) runs as usual.

#define	ECOFF_MAGIC			0x0162		// MIPS little endian
#define	ECOFF_STYP_NOLOAD	(0x80 | 0x400)	// .bss, .sbss: cleared by the program's start code
#define	AUTO_MAX_DATA		256
#define	AUTO_MAX_DEPTH		4
#define	RAM_USER_START		0x80010000
#define	RAM_FIRMWARE		0x801EF000	// PS1 DEV CART firmware (receives the files): data must stay below

typedef struct
{
	char		path[1024];
	uint32_t	adrs, size;
} AutoData;

static AutoData	auto_data[AUTO_MAX_DATA];
static int		auto_count;
static char		auto_program[1024];

// ECOFF executable to a PS-X EXE in memory (sections with data at their addresses, entry point and gp); NULL: not
// an ECOFF file. The program clears its .bss itself (as with the Net Yaroze boot disc and yarexe).
uint8_t	*EcoffToPsxExe(const uint8_t *data, int size, int *exe_size)
{
	uint16_t	nscns, opthdr;
	uint32_t	lo = 0xFFFFFFFF, hi = 0, image, i;
	const uint8_t	*sec;
	uint8_t		*exe;

	if ((size < 20 + 56) || (*(uint16_t *)data != ECOFF_MAGIC))
		return (NULL);
	nscns = *(uint16_t *)&data[2];
	opthdr = *(uint16_t *)&data[16];
	if ((opthdr < 56) || (20 + opthdr + nscns * 40 > (uint32_t)size))
		return (NULL);
	for (i = 0, sec = data + 20 + opthdr; i < nscns; i++, sec += 40)
	{
		uint32_t	vaddr = *(uint32_t *)&sec[12], ssize = *(uint32_t *)&sec[16], ptr = *(uint32_t *)&sec[20];

		if (!ssize || !ptr || (*(uint32_t *)&sec[36] & ECOFF_STYP_NOLOAD))
			continue;
		if (ptr + ssize > (uint32_t)size)
		{
			printf("ECOFF section %.8s is outside of the file\n", (const char *)sec);
			return (NULL);
		}
		if (vaddr < lo)
			lo = vaddr;
		if (vaddr + ssize > hi)
			hi = vaddr + ssize;
	}
	if (lo >= hi)
		return (NULL);
	image = (hi - lo + 2047) & ~2047;
	if (!(exe = calloc(1, 2048 + image)))
		return (NULL);
	memcpy(exe, "PS-X EXE", 8);
	*(uint32_t *)&exe[0x10] = *(uint32_t *)&data[20 + 16];	// a.out entry
	*(uint32_t *)&exe[0x14] = *(uint32_t *)&data[20 + 52];	// gp value
	*(uint32_t *)&exe[0x18] = lo;
	*(uint32_t *)&exe[0x1C] = image;
	strcpy((char *)&exe[0x4C], "Net Yaroze ECOFF executable (ps1transfer)");
	for (i = 0, sec = data + 20 + opthdr; i < nscns; i++, sec += 40)
	{
		uint32_t	vaddr = *(uint32_t *)&sec[12], ssize = *(uint32_t *)&sec[16], ptr = *(uint32_t *)&sec[20];

		if (ssize && ptr && !(*(uint32_t *)&sec[36] & ECOFF_STYP_NOLOAD))
			memcpy(exe + 2048 + vaddr - lo, data + ptr, ssize);
	}
	printf("Net Yaroze ECOFF executable: 0x%X-0x%X, entry point 0x%X\n", lo, hi, *(uint32_t *)&exe[0x10]);
	*exe_size = 2048 + image;
	return (exe);
}

// A file named in an auto script: relative to the folder of the script ('\' or '/')
static void	AutoPath(char *out, size_t n, const char *script, const char *name)
{
	const char	*slash = strrchr(script, '/'), *bslash = strrchr(script, '\\');
	size_t		dir;

	if (bslash > slash)
		slash = bslash;
	dir = slash ? (size_t)(slash - script + 1) : 0;
	if ((name[0] == '/') || (name[0] == '\\') || (name[0] && (name[1] == ':')) || (dir >= n))
		dir = 0;
	snprintf(out, n, "%.*s%s", (int)dir, script, name);
}

// Reads an auto script (and the scripts it calls with "auto"): the program and the data files with their addresses
bool	ReadAutoScript(const char *script, int depth)
{
	FILE	*f;
	char	line[1024], *w[4], *p;
	int		n, number = 0;

	if (depth >= AUTO_MAX_DEPTH)
	{
		printf("%s: too many nested auto scripts\n", script);
		return (false);
	}
	if (!(f = fopen(script, "r")))
	{
		printf("Auto script '%s' not found !\n", script);
		return (false);
	}
	while (fgets(line, sizeof(line), f))
	{
		number++;
		for (n = 0, p = strtok(line, " \t\r\n"); p && (n < 4); p = strtok(NULL, " \t\r\n"))
			w[n++] = p;
		if (n && !strcmp(w[0], "local"))	// The "local" commands of siocons (and the same without it)
		{
			memmove(w, w + 1, sizeof(char *) * 3);
			n--;
		}
		if (!n || (w[0][0] == '#') || (w[0][0] == ';'))
			continue;
		if (!strcmp(w[0], "load") && (n >= 2))
		{
			if (auto_program[0])
				printf("%s line %d: a second program, only the last one is run\n", script, number);
			AutoPath(auto_program, sizeof(auto_program), script, w[1]);
		}
		else if (!strcmp(w[0], "dload") && (n >= 3))
		{
			AutoData	*d = &auto_data[auto_count];

			if (auto_count >= AUTO_MAX_DATA)
			{
				printf("%s: more than %d data files\n", script, AUTO_MAX_DATA);
				fclose(f);
				return (false);
			}
			AutoPath(d->path, sizeof(d->path), script, w[1]);
			d->adrs = strtoul(w[2], NULL, 16);
			if (d->adrs < 0x80000000)
				d->adrs |= 0x80000000;
			auto_count++;
		}
		else if (!strcmp(w[0], "auto") && (n >= 2))
		{
			char	sub[1024];

			AutoPath(sub, sizeof(sub), script, w[1]);
			if (!ReadAutoScript(sub, depth + 1))
			{
				fclose(f);
				return (false);
			}
		}
		else if (strcmp(w[0], "go"))
			printf("%s line %d: '%s' skipped (only load, dload, auto and go are used)\n", script, number, w[0]);
	}
	fclose(f);
	return (true);
}

// Data files of the auto script, after the library: each one must fit below the firmware, and not overlap the
// library, the program or another file
bool	UploadAutoData(uint32_t exe_adrs, uint32_t exe_size, uint32_t lib_adrs, uint32_t lib_size)
{
	int		i, j;
	uint8_t	*data;
	int		size;

	for (i = 0; i < auto_count; i++)
	{
		FILE	*f = fopen(auto_data[i].path, "rb");

		if (!f)
		{
			printf("Data file '%s' not found !\n", auto_data[i].path);
			return (false);
		}
		fseek(f, 0, SEEK_END);
		auto_data[i].size = ftell(f);
		fclose(f);
	}
	for (i = 0; i < auto_count; i++)
	{
		AutoData	*d = &auto_data[i];
		const char	*clash = NULL;

		if ((d->adrs < RAM_USER_START) || (d->adrs + d->size > RAM_FIRMWARE))
		{
			printf("%s at 0x%X-0x%X: outside of 0x%X-0x%X (the cartridge firmware is above)\n", d->path, d->adrs,
				d->adrs + d->size, RAM_USER_START, RAM_FIRMWARE);
			return (false);
		}
		if (lib_size && (d->adrs < lib_adrs + lib_size) && (d->adrs + d->size > lib_adrs))
			clash = "the Net Yaroze library";
		else if ((d->adrs < exe_adrs + exe_size) && (d->adrs + d->size > exe_adrs))
			clash = "the program";
		for (j = 0; !clash && (j < i); j++)
			if ((d->adrs < auto_data[j].adrs + auto_data[j].size) && (d->adrs + d->size > auto_data[j].adrs))
				clash = auto_data[j].path;
		if (clash)
		{
			printf("%s at 0x%X-0x%X overlaps %s\n", d->path, d->adrs, d->adrs + d->size, clash);
			return (false);
		}
	}
	for (i = 0; i < auto_count; i++)
	{
		if (!(data = SystemGetFile(auto_data[i].path, &size)))
			return (false);
		printf("%s: %d bytes to 0x%X\n", auto_data[i].path, size, auto_data[i].adrs);
		usb_quiet = true;
		if (!USB_Process('U', data, auto_data[i].adrs, size))
		{
			usb_quiet = false;
			free(data);
			return (false);
		}
		usb_quiet = false;
		free(data);
	}
	return (true);
}

/****************************************/
// TMD model viewer: tmdviewer.ps-exe (built from the tmdviewer folder) is executed with a data block holding
// the model and its textures, see ps1-tools/tmdviewer/tmdviewer.c

#define	VIEWER_FILENAME		"tmdviewer.ps-exe"
#define	VIEWER_DATA_ADRS	0x80100000	// Above the viewer program and its buffers
#define	VIEWER_DATA_END		0x801E0000	// Below the PS1 DEV CART firmware (0x801EF000) which receives the data
#define	VIEWER_MAGIC		0x56444D54	// "TMDV"
#define	VIEWER_MAX_TIM		8
#define	VIEWER_MAX_TOD		16			// TOD animations (-tod)
#define	VIEWER_TOD_TABLE	0x40		// Number of TODs, then address, size and name (24 bytes) of each
#define	VIEWER_HEADER_SIZE	0x400		// (the shared state of -mdllive at 0x300, see mdllive.c)

// The PS1 tools (model viewer, sound tester) are looked for next to the ps1transfer executable
#define	TOOL_NAME_MAX	64

char	*GetToolPath(char *argv0, char *name)
{
	static char	path[4096];
	char		*sep, *sep2;
	int			len;

#ifdef _WIN32
	len = GetModuleFileNameA(NULL, path, sizeof(path) - TOOL_NAME_MAX);
#else
	len = readlink("/proc/self/exe", path, sizeof(path) - TOOL_NAME_MAX - 1);
#endif
	if (len <= 0)
	{
		strncpy(path, argv0, sizeof(path) - TOOL_NAME_MAX - 1);
		len = strlen(path);
	}
	path[len] = 0;

	sep = strrchr(path, '/');
	sep2 = strrchr(path, '\\');
	if (sep2 > sep)
		sep = sep2;
	strncpy(sep ? sep + 1 : path, name, TOOL_NAME_MAX - 1);
	return (path);
}

/****************************************/
// Sound tester: soundtest.ps-exe (built from the soundtest folder) is executed with a data block holding
// the sound files, see ps1-tools/soundtest/soundtest.c

#define	TESTER_FILENAME		"soundtest.ps-exe"
#define	TESTER_DATA_ADRS	0x80100000
#define	TESTER_DATA_END		0x801E0000	// Below the PS1 DEV CART firmware (0x801EF000) which receives the data
#define	TESTER_MAGIC		0x54444E53	// "SNDT"
#define	TESTER_MAX_FILES	8
#define	TESTER_HEADER_SIZE	0x100

enum
{
	SOUND_VH = 1,
	SOUND_VB,
	SOUND_VAB,	// VH followed by VB
	SOUND_SEQ,
	SOUND_VAG
};

// Type from the file header ("pBAV" VH/VAB, "pQES" SEQ, "VAGp" VAG), VB files have no header: ".vb" extension
int	SoundFileType(char *name, uint8_t *data, int size)
{
	char	*ext = strrchr(name, '.');

	if ((size >= 32) && !memcmp(data, "pBAV", 4))
	{
		uint32_t	ps = data[18] | (data[19] << 8);	// Number of programs
		uint32_t	vh_size = 32 + 128 * 16 + ps * 16 * 32 + 256 * 2;

		return ((size > vh_size) ? SOUND_VAB : SOUND_VH);
	}
	if ((size >= 4) && !memcmp(data, "pQES", 4))
		return (SOUND_SEQ);
	if ((size >= 48) && !memcmp(data, "VAGp", 4))
		return (SOUND_VAG);
	if (ext && ((!strcmp(ext, ".vb")) || (!strcmp(ext, ".VB")) || (!strcmp(ext, ".Vb"))))
		return (SOUND_VB);
	return (0);
}

/****************************************/
// TIM and VRAM viewer: timviewer.ps-exe (built from the timviewer folder) is executed with a data block holding
// the TIM files and their names, see ps1-tools/timviewer/timviewer.c

#define	TIMVIEW_FILENAME	"timviewer.ps-exe"
#define	TIMVIEW_DATA_ADRS	0x80100000
#define	TIMVIEW_DATA_END	0x801E0000	// Below the PS1 DEV CART firmware (0x801EF000) which receives the data
#define	TIMVIEW_MAGIC		0x564D4954	// "TIMV"
#define	TIMVIEW_MAX_FILES	32
#define	TIMVIEW_NAME_LEN	24
#define	TIMVIEW_HEADER_SIZE	0x800

bool	UploadTimData(char **files, int count)
{
	uint8_t		*block, *file, *entry;
	uint32_t	*header, offset = TIMVIEW_HEADER_SIZE;
	int			i, size;
	char		*name, *sep;
	bool		ret = false;

	if (!(block = calloc(1, TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = TIMVIEW_MAGIC;
	header[1] = count;

	for (i = 0; i < count; i++)
	{
		if (!(file = SystemGetFile(files[i], &size)))
			goto out;
		if ((size < 8) || (*((uint32_t *)file) != 0x10))
		{
			printf("'%s' is not a TIM file !\n", files[i]);
			free(file);
			goto out;
		}
		if (offset + size > TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS)
		{
			printf("TIM files are too big (max %d bytes) !\n", TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS - TIMVIEW_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		// Entry: address, size, file name without path (displayed by the viewer)
		entry = block + 8 + i * (8 + TIMVIEW_NAME_LEN);
		((uint32_t *)entry)[0] = TIMVIEW_DATA_ADRS + offset;
		((uint32_t *)entry)[1] = size;
		name = files[i];
		if ((sep = strrchr(name, '/')))
			name = sep + 1;
		if ((sep = strrchr(name, '\\')))
			name = sep + 1;
		strncpy((char *)entry + 8, name, TIMVIEW_NAME_LEN - 1);
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading %d TIM file(s), %u bytes to 0x%X...\n", count, offset, TIMVIEW_DATA_ADRS);
	ret = USB_Process('U', block, TIMVIEW_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

/****************************************/
// Memory card backup/restore: cardtool.ps-exe (built from the cardtool folder) is executed with a command block,
// the backup is written by the PS1 program with the PCDRV file server, see ps1-tools/cardtool/cardtool.c

#define	CARD_FILENAME		"cardtool.ps-exe"
#define	LINKTEST_FILENAME	"linktest.ps-exe"	// Link cable tester (linktest folder), "-link" option
#define	PADTEST_FILENAME	"padtest.ps-exe"		// Control tester (padtest folder), "-padtest" option
#define	CARD_DATA_ADRS		0x80100000
#define	CARD_MAGIC			0x44524143	// "CARD"
#define	CARD_IMAGE_OFFSET	0x100
#define	CARD_SIZE			(128 * 1024)
#define	CARD_GME_HEADER		3904		// DexDrive .gme files: header before the raw image
#define	CARD_EOT			4			// Printed by the PS1 program at the end: console mode is stopped

enum	{CARD_BACKUP = 1, CARD_RESTORE};

bool	exit_on_eot;

// Saves listed from the directory frames (sectors 1-15): state 0x51 = first block of a save, size, file name
void	PrintCardDirectory(uint8_t *img)
{
	int	i, n = 0, used = 0;

	if (memcmp(img, "MC", 2))
	{
		printf("Not a formatted memory card image (no \"MC\" header).\n");
		return;
	}
	printf("Memory card saves:\n");
	for (i = 1; i < 16; i++)
	{
		uint8_t		*e = img + i * 128;
		uint32_t	size = e[4] | (e[5] << 8) | (e[6] << 16) | (e[7] << 24);

		if ((e[0] & 0xF0) == 0x50)
			used++;
		if (e[0] == 0x51)
		{
			printf("  block %2d: %-20.20s %u block(s)\n", i, (char *)e + 10, size / 8192);
			n++;
		}
	}
	printf("%d save(s), %d/15 blocks used\n", n, used);
}

/****************************************/
// Memory card image tools (on the computer, no PS1 needed): list with titles/icons, export/import .mcs saves,
// delete, copy between images, format. Card image: 16 blocks of 8KB, block 0 = header + directory frames (1-15),
// one directory frame per block: state, size (first block), next block, file name, XOR checksum.

#define	MC_BLOCK		8192
#define	MC_FRAME		128
#define	MC_STATE_FREE	0xA0
#define	MC_STATE_FIRST	0x51
#define	MC_STATE_MIDDLE	0x52
#define	MC_STATE_LAST	0x53

static uint8_t	*McFrame(uint8_t *img, int block)	{return (img + block * MC_FRAME);}	// Directory frame of block 1-15

static void	McChecksum(uint8_t *frame)
{
	uint8_t	x = 0;
	int		i;

	for (i = 0; i < MC_FRAME - 1; i++)
		x ^= frame[i];
	frame[MC_FRAME - 1] = x;
}

static void	McSetEntry(uint8_t *img, int block, uint32_t state, uint32_t size, uint16_t next, const char *name)
{
	uint8_t	*f = McFrame(img, block);

	memset(f, 0, MC_FRAME);
	f[0] = state;
	f[4] = size;
	f[5] = size >> 8;
	f[6] = size >> 16;
	f[7] = size >> 24;
	f[8] = next;
	f[9] = next >> 8;
	if (name)
		memcpy(f + 10, name, strnlen(name, 20));	// 20 characters max, the frame is zero filled
	McChecksum(f);
}

// Blank formatted card: "MC" header, 15 free blocks, empty broken sector list
static void	McFormat(uint8_t *img)
{
	int	i;

	memset(img, 0, CARD_SIZE);
	img[0] = 'M';
	img[1] = 'C';
	McChecksum(img);
	for (i = 1; i < 16; i++)
		McSetEntry(img, i, MC_STATE_FREE, 0, 0xFFFF, NULL);
	for (i = 16; i < 36; i++)	// Broken sector list
	{
		memset(McFrame(img, i), 0, MC_FRAME);
		memset(McFrame(img, i), 0xFF, 4);
		McFrame(img, i)[8] = 0xFF;
		McFrame(img, i)[9] = 0xFF;
		McChecksum(McFrame(img, i));
	}
}

// Shift-JIS title (save title frame) to ASCII: full width letters, digits and common signs, others '?'
static void	McTitle(uint8_t *sjis, char *out, int max)
{
	static const struct {uint16_t code; char c;} signs[] = {{0x8140, ' '}, {0x8143, ','}, {0x8144, '.'}, {0x8146, ':'},
		{0x8147, ';'}, {0x8148, '?'}, {0x8149, '!'}, {0x815E, '/'}, {0x8169, '('}, {0x816A, ')'}, {0x817B, '+'},
		{0x817C, '-'}, {0x8181, '='}, {0x815B, '-'}, {0x8160, '~'}, {0x8175, '['}, {0x8176, ']'}, {0x8193, '%'}};
	int	i = 0, n = 0, k;

	while ((i < 64) && sjis[i] && (n < max - 1))
	{
		if (sjis[i] < 0x80)
			out[n++] = sjis[i++];
		else
		{
			uint16_t	c = (sjis[i] << 8) | sjis[i + 1];
			char		a = '?';

			if ((c >= 0x824F) && (c <= 0x8258))	a = '0' + c - 0x824F;
			else if ((c >= 0x8260) && (c <= 0x8279))	a = 'A' + c - 0x8260;
			else if ((c >= 0x8281) && (c <= 0x829A))	a = 'a' + c - 0x8281;
			else
				for (k = 0; k < sizeof(signs) / sizeof(signs[0]); k++)
					if (signs[k].code == c)
						a = signs[k].c;
			out[n++] = a;
			i += 2;
		}
	}
	while (n && (out[n - 1] == ' '))
		n--;
	out[n] = 0;
}

// Save: block number (1-15) or file name, returns the first block or 0
static int	McFind(uint8_t *img, char *save)
{
	int	i, n = atoi(save);

	if ((n >= 1) && (n <= 15) && (McFrame(img, n)[0] == MC_STATE_FIRST))
		return (n);
	for (i = 1; i < 16; i++)
		if ((McFrame(img, i)[0] == MC_STATE_FIRST) && !strncmp((char *)McFrame(img, i) + 10, save, 20))
			return (i);
	printf("Save '%s' not found (give its file name or block number, see -mclist)\n", save);
	return (0);
}

// Blocks of a save, following the "next" links from its first block
static int	McChain(uint8_t *img, int first, int *blocks)
{
	int	n = 0, b = first;

	while ((b >= 1) && (b <= 15) && (n < 15))
	{
		uint8_t		*f = McFrame(img, b);
		uint16_t	next = f[8] | (f[9] << 8);

		blocks[n++] = b;
		if (next == 0xFFFF)
			break;
		b = next + 1;
	}
	return (n);
}

static void	McList(uint8_t *img, char *icon_dir)
{
	int		i, j, n, blocks[15], used = 0, saves = 0;
	char	title[96], name[24], png[4096];

	if (memcmp(img, "MC", 2))
	{
		printf("Not a formatted memory card image (no \"MC\" header).\n");
		return;
	}
	for (i = 1; i < 16; i++)
	{
		uint8_t	*f = McFrame(img, i), *save = img + i * MC_BLOCK;

		used += ((f[0] & 0xF0) == 0x50);
		if (f[0] != MC_STATE_FIRST)
			continue;
		saves++;
		n = McChain(img, i, blocks);
		memcpy(name, f + 10, 20);
		name[20] = 0;
		McTitle(save + 4, title, sizeof(title));
		printf("  block %2d: %-20s %d block(s)  \"%s\"%s\n", i, name, n, title, memcmp(save, "SC", 2) ? "  (no SC title frame)" : "");
		printf("            blocks");
		for (j = 0; j < n; j++)
			printf(" %d", blocks[j]);
		printf(", icon %d frame(s)\n", save[2] & 3);

		// Icon: first frame, 16x16 4-bit, CLUT of 16 colors (15-bit, 0 = transparent) in the title frame
		if (icon_dir && !memcmp(save, "SC", 2))
		{
			uint8_t	rgba[16 * 16 * 4];

			for (j = 0; j < 256; j++)
			{
				uint8_t		p = (save[MC_FRAME + j / 2] >> ((j & 1) * 4)) & 15;
				uint16_t	c = save[0x60 + p * 2] | (save[0x61 + p * 2] << 8);

				rgba[j * 4] = (c & 31) << 3;
				rgba[j * 4 + 1] = ((c >> 5) & 31) << 3;
				rgba[j * 4 + 2] = ((c >> 10) & 31) << 3;
				rgba[j * 4 + 3] = c ? 255 : 0;
			}
			snprintf(png, sizeof(png), "%s/block%02d_%s.png", icon_dir, i, name);
			if (!lodepng_encode32_file(png, rgba, 16, 16))
				printf("            icon: %s\n", png);
		}
	}
	printf("%d save(s), %d/15 blocks used\n", saves, used);
}

// .mcs single save: directory frame of the first block (128 bytes) followed by the save blocks
static uint8_t	*McExport(uint8_t *img, int first, int *size)
{
	int		blocks[15], n = McChain(img, first, blocks), i;
	uint8_t	*mcs = malloc(MC_FRAME + n * MC_BLOCK);

	memcpy(mcs, McFrame(img, first), MC_FRAME);
	for (i = 0; i < n; i++)
		memcpy(mcs + MC_FRAME + i * MC_BLOCK, img + blocks[i] * MC_BLOCK, MC_BLOCK);
	*size = MC_FRAME + n * MC_BLOCK;
	return (mcs);
}

static bool	McImport(uint8_t *img, uint8_t *mcs, int size)
{
	int		n = (size - MC_FRAME) / MC_BLOCK, blocks[15], i, free_count = 0;
	char	name[24];

	if ((size < MC_FRAME + MC_BLOCK) || ((size - MC_FRAME) % MC_BLOCK) || (mcs[0] != MC_STATE_FIRST))
	{
		printf("Not a .mcs save file (128 bytes directory frame + 8KB blocks)\n");
		return (false);
	}
	memcpy(name, mcs + 10, 20);
	name[20] = 0;
	for (i = 1; i < 16; i++)
		if ((McFrame(img, i)[0] == MC_STATE_FIRST) && !strncmp((char *)McFrame(img, i) + 10, name, 20))
		{
			printf("The card already has a save '%s' (block %d), delete it first with -mcdelete\n", name, i);
			return (false);
		}
	// Free blocks, then deleted ones (a deleted save can still be undeleted as long as its blocks are not reused)
	for (i = 1; (i < 16) && (free_count < n); i++)
		if (McFrame(img, i)[0] == MC_STATE_FREE)
			blocks[free_count++] = i;
	for (i = 1; (i < 16) && (free_count < n); i++)
		if (((McFrame(img, i)[0] & 0xF0) != 0x50) && (McFrame(img, i)[0] != MC_STATE_FREE))
			blocks[free_count++] = i;
	if (free_count < n)
	{
		printf("Not enough free blocks: %d needed\n", n);
		return (false);
	}
	for (i = 0; i < n; i++)
	{
		memcpy(img + blocks[i] * MC_BLOCK, mcs + MC_FRAME + i * MC_BLOCK, MC_BLOCK);
		McSetEntry(img, blocks[i], (i == 0) ? MC_STATE_FIRST : ((i == n - 1) ? MC_STATE_LAST : MC_STATE_MIDDLE),
			(i == 0) ? n * MC_BLOCK : 0, (i == n - 1) ? 0xFFFF : blocks[i + 1] - 1, name);
	}
	printf("Save '%s' imported in block(s)", name);
	for (i = 0; i < n; i++)
		printf(" %d", blocks[i]);
	printf("\n");
	return (true);
}

// Deleted like the PS1 does: states 0xA1 (first), 0xA2 (middle), 0xA3 (last), data and links kept
static void	McDelete(uint8_t *img, int first)
{
	int	blocks[15], n = McChain(img, first, blocks), i;

	for (i = 0; i < n; i++)
	{
		McFrame(img, blocks[i])[0] = 0xA0 | (McFrame(img, blocks[i])[0] & 0x0F);
		McChecksum(McFrame(img, blocks[i]));
	}
	printf("Save in block %d deleted (%d block(s) freed)\n", first, n);
}

static uint8_t	*McLoadImage(char *filename)
{
	uint8_t	*file, *img;
	int		size;

	if (!(file = SystemGetFile(filename, &size)))
		return (NULL);
	if ((size != CARD_SIZE) && (size != CARD_SIZE + CARD_GME_HEADER))
	{
		printf("'%s' is not a memory card image (%d bytes, 131072 expected)\n", filename, size);
		free(file);
		return (NULL);
	}
	img = malloc(CARD_SIZE);
	memcpy(img, file + size - CARD_SIZE, CARD_SIZE);	// DexDrive .gme: header skipped
	free(file);
	return (img);
}

static bool	McSaveFile(char *filename, uint8_t *data, int size, bool backup)
{
	char	bak[4200];
	FILE	*f;

	if (backup && !access(filename, 0))
	{
		snprintf(bak, sizeof(bak), "%s.bak", filename);
		remove(bak);
		rename(filename, bak);
		printf("Previous image kept as %s\n", bak);
	}
	if (!(f = fopen(filename, "wb")))
	{
		printf("Can't write '%s'\n", filename);
		return (false);
	}
	fwrite(data, 1, size, f);
	fclose(f);
	return (true);
}

// -mclist, -mcexport, -mcimport, -mcdelete, -mccopy, -mcformat: returns the exit code
int	CardImageCommand(int argc, char **argv, int i)
{
	char	*cmd = argv[i], *icons = NULL;
	uint8_t	*img = NULL, *img2 = NULL, *mcs = NULL;
	int		first, size, ret = EXIT_FAILURE, need = (!strcmp(cmd, "-mclist") || !strcmp(cmd, "-mcformat")) ? 1 :
				((!strcmp(cmd, "-mccopy") || !strcmp(cmd, "-mcexport")) ? 3 : 2);

	if (i + need >= argc)
	{
		printf("%s: missing parameters, see the usage\n", cmd);
		return (ret);
	}
	if (!strcmp(cmd, "-mcformat"))
	{
		img = malloc(CARD_SIZE);
		McFormat(img);
		if (McSaveFile(argv[i + 1], img, CARD_SIZE, true))
		{
			printf("Blank formatted memory card image written: %s\n", argv[i + 1]);
			ret = EXIT_SUCCESS;
		}
		goto out;
	}
	if (!(img = McLoadImage(argv[i + 1])))
		goto out;

	if (!strcmp(cmd, "-mclist"))
	{
		if ((i + 3 < argc) && !strcmp(argv[i + 2], "-icons"))
			icons = argv[i + 3];
		McList(img, icons);
		ret = EXIT_SUCCESS;
	}
	else if (!strcmp(cmd, "-mcexport"))
	{
		if ((first = McFind(img, argv[i + 2])) && (mcs = McExport(img, first, &size)) && McSaveFile(argv[i + 3], mcs, size, false))
		{
			printf("Save exported: %s (%d bytes)\n", argv[i + 3], size);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcimport"))
	{
		if ((mcs = SystemGetFile(argv[i + 2], &size)) && McImport(img, mcs, size) && McSaveFile(argv[i + 1], img, CARD_SIZE, true))
		{
			McList(img, NULL);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcdelete"))
	{
		if ((first = McFind(img, argv[i + 2])))
		{
			McDelete(img, first);
			if (McSaveFile(argv[i + 1], img, CARD_SIZE, true))
				ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mccopy"))	// src save dst
	{
		if ((first = McFind(img, argv[i + 2])) && (mcs = McExport(img, first, &size)) && (img2 = McLoadImage(argv[i + 3])) &&
			McImport(img2, mcs, size) && McSaveFile(argv[i + 3], img2, CARD_SIZE, true))
		{
			McList(img2, NULL);
			ret = EXIT_SUCCESS;
		}
	}

out:
	free(img);
	free(img2);
	free(mcs);
	return (ret);
}

bool	UploadCardData(int mode, int slot, char *filename)
{
	uint8_t		*block, *file = NULL;
	uint32_t	*header, size = CARD_IMAGE_OFFSET;
	char		*name, *sep;
	int			fsize;
	bool		ret = false;

	if (!(block = calloc(1, CARD_IMAGE_OFFSET + CARD_SIZE)))
		return (false);
	header = (uint32_t *)block;
	header[0] = CARD_MAGIC;
	header[1] = mode;
	header[2] = slot;
	// Backup file name without path (the file server runs in its directory)
	name = filename;
	if ((sep = strrchr(name, '/')))
		name = sep + 1;
	if ((sep = strrchr(name, '\\')))
		name = sep + 1;
	strncpy((char *)&header[3], name, 63);

	if (mode == CARD_RESTORE)
	{
		if (!(file = SystemGetFile(filename, &fsize)))
			goto out;
		if (fsize == CARD_SIZE + CARD_GME_HEADER)	// DexDrive .gme
			memcpy(block + CARD_IMAGE_OFFSET, file + CARD_GME_HEADER, CARD_SIZE);
		else if (fsize == CARD_SIZE)
			memcpy(block + CARD_IMAGE_OFFSET, file, CARD_SIZE);
		else
		{
			printf("'%s' is not a memory card image (%d bytes, 131072 expected) !\n", filename, fsize);
			goto out;
		}
		PrintCardDirectory(block + CARD_IMAGE_OFFSET);
		size += CARD_SIZE;
		printf("Confirm on the PS1: Cross to write the card in slot %d, Circle to cancel.\n", slot);
	}

	ret = USB_Process('U', block, CARD_DATA_ADRS, size);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(file);
	free(block);
	return (ret);
}

bool	UploadSoundData(char **files, int count)
{
	static const char	*type_names[] = {"?", "VH", "VB", "VAB", "SEQ", "VAG"};
	uint8_t				*block, *file;
	uint32_t			*header, offset = TESTER_HEADER_SIZE;
	int					i, size, type;
	bool				ret = false;

	if (!(block = calloc(1, TESTER_DATA_END - TESTER_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = TESTER_MAGIC;
	header[1] = count;

	for (i = 0; i < count; i++)
	{
		if (!(file = SystemGetFile(files[i], &size)))
			goto out;
		if (!(type = SoundFileType(files[i], file, size)))
		{
			printf("'%s' is not a VH, VB, VAB, SEQ or VAG file !\n", files[i]);
			free(file);
			goto out;
		}
		if (offset + size > TESTER_DATA_END - TESTER_DATA_ADRS)
		{
			printf("Sound files are too big (max %d bytes) !\n", TESTER_DATA_END - TESTER_DATA_ADRS - TESTER_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		header[2 + i * 3] = type;
		header[2 + i * 3 + 1] = TESTER_DATA_ADRS + offset;
		header[2 + i * 3 + 2] = size;
		printf("%s: %s (%d bytes)\n", type_names[type], files[i], size);
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading %d sound file(s), %u bytes to 0x%X...\n", count, offset, TESTER_DATA_ADRS);
	ret = USB_Process('U', block, TESTER_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

// Data block: header (magic, TMD address, number of TIM, TIM addresses; at VIEWER_TOD_TABLE the TOD table) then the
// TMD, TIM and TOD files
bool	UploadModelData(char *model, char **tims, int tim_count, char **tods, int tod_count)
{
	uint8_t		*block, *file;
	uint32_t	*header, offset = VIEWER_HEADER_SIZE, *tod_table;
	int			i, size;
	bool		ret = false;

	if (!(block = calloc(1, VIEWER_DATA_END - VIEWER_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = VIEWER_MAGIC;
	header[2] = tim_count;
	tod_table = (uint32_t *)(block + VIEWER_TOD_TABLE);
	tod_table[0] = tod_count;

	for (i = -1; i < tim_count + tod_count; i++)
	{
		char		*name = (i < 0) ? model : ((i < tim_count) ? tims[i] : tods[i - tim_count]);
		uint32_t	id = (i < 0) ? 0x41 : ((i < tim_count) ? 0x10 : 0x50);

		if (!(file = SystemGetFile(name, &size)))
			goto out;
		if ((size < 8) || ((*((uint32_t *)file) & ((id == 0x50) ? 0xFF : 0xFFFFFFFF)) != id))
		{
			printf("'%s' is not a %s file !\n", name, (i < 0) ? "TMD" : ((i < tim_count) ? "TIM" : "TOD"));
			free(file);
			goto out;
		}
		if (offset + size > VIEWER_DATA_END - VIEWER_DATA_ADRS)
		{
			printf("Model and textures are too big (max %d bytes) !\n", VIEWER_DATA_END - VIEWER_DATA_ADRS - VIEWER_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		if (i < 0)
			header[1] = VIEWER_DATA_ADRS + offset;
		else if (i < tim_count)
			header[3 + i] = VIEWER_DATA_ADRS + offset;
		else
		{
			// TOD: address, size, name (file name without its folder, shown by the viewer)
			uint32_t	*entry = tod_table + 1 + (i - tim_count) * 8;
			char		*base = name + strlen(name);

			while ((base > name) && (base[-1] != '/') && (base[-1] != '\\'))
				base--;
			entry[0] = VIEWER_DATA_ADRS + offset;
			entry[1] = size;
			strncpy((char *)(entry + 2), base, 23);
			printf("TOD: %s (%d bytes, %d frames)\n", name, size, (int)((uint32_t *)file)[1]);
		}
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading model, %d texture(s) and %d animation(s), %u bytes to 0x%X...\n", tim_count, tod_count, offset,
		VIEWER_DATA_ADRS);
	ret = USB_Process('U', block, VIEWER_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

/****************************************/
// Crash report: registers of the crashed program (BIOS TCB) and stack, read while the PS1 DEV CART crash screen is displayed
// (the crash screen keeps processing USB commands with the PS1 DEV CART firmware)

#define	BIOS_TCBH_PTR		0x80000108	// Task control block header pointer, its first word points to the current TCB
#define	CRASH_STACK_SIZE	1024
#define	CRASH_MAX_CALLERS	16
#define	RAM_START			0x80000000
#define	RAM_END				0x80200000
#define	SYMBOL_MAX_OFFSET	0x10000		// Further from a symbol, the address is shown without name

typedef struct
{
	uint32_t	adrs;
	char		*name;
} Symbol;

Symbol	*symbols;
int		symbol_count;

int	SymbolCompare(const void *a, const void *b)
{
	uint32_t	x = ((const Symbol *)a)->adrs, y = ((const Symbol *)b)->adrs;

	return ((x > y) - (x < y));
}

int		symbol_max;

void	AddSymbol(uint32_t adrs, const char *name)
{
	if ((adrs < RAM_START) || (adrs >= RAM_END) || !name[0] || (name[0] == '.') || (name[0] == '$') || strchr(name, '='))
		return;
	if (symbol_count == symbol_max)
	{
		symbol_max = symbol_max ? symbol_max * 2 : 1024;
		symbols = realloc(symbols, symbol_max * sizeof(Symbol));
	}
	symbols[symbol_count].adrs = adrs;
	symbols[symbol_count].name = strdup(name);
	symbol_count++;
}

// ELF symbol table (SHT_SYMTAB section and its string table): defined functions, objects and labels
static uint32_t	Le32(uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}
static uint16_t	Le16(uint8_t *p)	{return (p[0] | (p[1] << 8));}

bool	LoadSymbolsElf(uint8_t *elf, int size)
{
	uint32_t	shoff = Le32(elf + 0x20), i, j;
	uint16_t	shentsize = Le16(elf + 0x2E), shnum = Le16(elf + 0x30);

	if ((elf[4] != 1) || (elf[5] != 1) || (shoff + shnum * shentsize > size))	// 32-bit little endian
		return (false);
	for (i = 0; i < shnum; i++)
	{
		uint8_t		*sh = elf + shoff + i * shentsize, *strsh;
		uint32_t	symoff = Le32(sh + 16), symsize = Le32(sh + 20), strtab;

		if ((Le32(sh + 4) != 2) || (Le32(sh + 24) >= shnum) || (symoff + symsize > size))	// SHT_SYMTAB
			continue;
		strsh = elf + shoff + Le32(sh + 24) * shentsize;
		strtab = Le32(strsh + 16);
		for (j = 0; j + 16 <= symsize; j += 16)
		{
			uint8_t		*sym = elf + symoff + j;
			uint32_t	name = Le32(sym), type = sym[12] & 15;

			if (Le16(sym + 14) && (type <= 2) && (strtab + name < size))	// Defined NOTYPE/OBJECT/FUNC
				AddSymbol(Le32(sym + 4), (char *)elf + strtab + name);
		}
	}
	return (true);
}

// ECOFF symbol table (Net Yaroze executable, not stripped): the symbolic header (HDRR) gives the external symbols
// (functions, variables, and the Net Yaroze library's functions as absolute symbols: VSync, DrawSync...) and the
// local ones of each file descriptor (static functions and variables)
#define	ECOFF_HDRR_MAGIC	0x7009
#define	ECOFF_ST_GLOBAL		1
#define	ECOFF_ST_STATIC		2
#define	ECOFF_ST_PROC		6
#define	ECOFF_ST_STATICPROC	14

static bool	EcoffSymbolClass(uint32_t bits, bool external)
{
	uint32_t	st = bits & 0x3F, sc = (bits >> 6) & 0x1F;

	// Storage class: text, data, bss, absolute, sdata, sbss, rdata
	if ((sc != 1) && (sc != 2) && (sc != 3) && (sc != 5) && (sc != 13) && (sc != 14) && (sc != 15))
		return (false);
	return (external ? ((st == ECOFF_ST_GLOBAL) || (st == ECOFF_ST_PROC)) :
		((st == ECOFF_ST_STATIC) || (st == ECOFF_ST_STATICPROC)));	// (global procedures are also external)
}

bool	LoadSymbolsEcoff(uint8_t *ecoff, int size)
{
	uint32_t	symptr = Le32(ecoff + 8), i, j;
	uint8_t		*h = ecoff + symptr;
	uint32_t	isym, sym_off, ss_off, ss_ext_off, ifd, fd_off, iext, ext_off;

	if ((Le16(ecoff) != ECOFF_MAGIC) || !symptr || (symptr + 96 > (uint32_t)size) || (Le16(h) != ECOFF_HDRR_MAGIC))
		return (false);
	isym = Le32(h + 32);
	sym_off = Le32(h + 36);
	ss_off = Le32(h + 60);
	ss_ext_off = Le32(h + 68);
	ifd = Le32(h + 72);
	fd_off = Le32(h + 76);
	iext = Le32(h + 88);
	ext_off = Le32(h + 92);
	if ((ext_off + iext * 16 > (uint32_t)size) || (sym_off + isym * 12 > (uint32_t)size) ||
		(fd_off + ifd * 72 > (uint32_t)size))
		return (false);
	for (i = 0; i < iext; i++)		// EXTR: flags (2), file (2), SYMR: name (4), value (4), st/sc/index (4)
	{
		uint8_t		*e = ecoff + ext_off + i * 16;
		uint32_t	name = ss_ext_off + Le32(e + 4);

		if (EcoffSymbolClass(Le32(e + 12), true) && (name < (uint32_t)size) && memchr(ecoff + name, 0, size - name))
			AddSymbol(Le32(e + 8), (char *)ecoff + name);
	}
	for (i = 0; i < ifd; i++)		// FDR: names (issBase at 8), symbols (isymBase at 16, csym at 20)
	{
		uint8_t		*fd = ecoff + fd_off + i * 72;
		uint32_t	iss_base = Le32(fd + 8), isym_base = Le32(fd + 16), csym = Le32(fd + 20);

		for (j = 0; (j < csym) && (isym_base + j < isym); j++)
		{
			uint8_t		*s = ecoff + sym_off + (isym_base + j) * 12;
			uint32_t	name = ss_off + iss_base + Le32(s);

			if (EcoffSymbolClass(Le32(s + 8), false) && (name < (uint32_t)size) && memchr(ecoff + name, 0, size - name))
				AddSymbol(Le32(s + 4), (char *)ecoff + name);
		}
	}
	return (true);
}

// Symbol file: ELF executable, Net Yaroze ECOFF executable, GNU ld map file ("   0x80010000   main") or nm output
// ("80010000 T main")
void	LoadSymbols(char *filename)
{
	FILE		*f;
	char		line[1024], *tok[4], *name;
	uint8_t		magic[4] = {0};
	int			n;

	if (!(f = fopen(filename, "rb")))
	{
		printf("Symbol file '%s' not found !\n", filename);
		return;
	}
	fread(magic, 1, 4, f);
	fclose(f);
	if (!memcmp(magic, "\x7F" "ELF", 4))
	{
		uint8_t	*elf = SystemGetFile(filename, &n);

		if (elf && !LoadSymbolsElf(elf, n))
			printf("'%s' is not a 32-bit little endian ELF file !\n", filename);
		free(elf);
	}
	else if ((magic[0] | (magic[1] << 8)) == ECOFF_MAGIC)
	{
		uint8_t	*ecoff = SystemGetFile(filename, &n);

		if (ecoff && !LoadSymbolsEcoff(ecoff, n))
			printf("'%s': no symbol table in this ECOFF executable (stripped?)\n", filename);
		free(ecoff);
	}
	else if ((f = fopen(filename, "r")))
	{
		while (fgets(line, sizeof(line), f))
		{
			char	whole[1024];

			strcpy(whole, line);
			for (n = 0, tok[0] = strtok(line, " \t\r\n"); tok[n] && (n < 3); tok[++n] = strtok(NULL, " \t\r\n"))
				;
			if ((n == 2) && !strncmp(tok[0], "0x", 2) && strncmp(tok[1], "0x", 2))	// map: "0x80010000  main"
				name = tok[1];
			else if ((n == 3) && (strlen(tok[1]) == 1) && isalpha((unsigned char)tok[1][0]))	// nm: "80010000 T main"
			{
				// The rest of the line: C++ names from "nm -C" have spaces ("Game::draw(int, int)")
				name = whole + (tok[2] - line);
				name[strcspn(name, "\r\n")] = 0;
			}
			else
				continue;
			AddSymbol((uint32_t)strtoull(tok[0], NULL, 16), name);
		}
		fclose(f);
	}
	qsort(symbols, symbol_count, sizeof(Symbol), SymbolCompare);
	printf("%d symbols loaded from '%s'\n", symbol_count, filename);
}

// The last executable run with -e is remembered, its symbol file (same name: .map, .sym or .elf) is loaded automatically
char	*LastExePath(void)
{
	static char	path[4096];
	char		*tmp = getenv("TEMP");

#ifndef _WIN32
	if (!tmp)
		tmp = "/tmp";
#endif
	snprintf(path, sizeof(path), "%s/ps1transfer_last_exe.txt", tmp ? tmp : ".");
	return (path);
}

void	SaveLastExe(char *exe)
{
	char	full[4096];
	FILE	*f;

#ifdef _WIN32
	if (!_fullpath(full, exe, sizeof(full)))
#else
	if (!realpath(exe, full))
#endif
		return;
	if ((f = fopen(LastExePath(), "w")))
	{
		fprintf(f, "%s\n", full);
		fclose(f);
	}
}

void	AutoLoadSymbols(char *symfile)
{
	static const char	*exts[] = {".elf", ".map", ".sym", ".ELF", ".MAP", ".SYM"};	// (ELF: static functions too)
	char				exe[4096], full[4096], cand[4200], *dot, *sep;
	uint8_t				magic[2] = {0};
	FILE				*f;
	int					i;

	if (symbol_count)
		return;
	if (symfile)
	{
		LoadSymbols(symfile);
		return;
	}
	if (!(f = fopen(LastExePath(), "r")))
		return;
	exe[0] = 0;
	fgets(exe, sizeof(exe), f);
	fclose(f);
	exe[strcspn(exe, "\r\n")] = 0;
	// A Net Yaroze ECOFF executable (e.g. "main" of an auto script) has its own symbols, unless a file is given
	// next to it
	if ((f = fopen(exe, "rb")))
	{
		fread(magic, 1, 2, f);
		fclose(f);
	}
	strcpy(full, exe);
	dot = strrchr(exe, '.');
	sep = strrchr(exe, '\\') > strrchr(exe, '/') ? strrchr(exe, '\\') : strrchr(exe, '/');
	if (dot && (!sep || (dot > sep)))
		*dot = 0;
	for (i = 0; i < 6; i++)
	{
		snprintf(cand, sizeof(cand), "%s%s", exe, exts[i]);
		if (!access(cand, 0))
		{
			printf("Symbols of the last executable: ");
			LoadSymbols(cand);
			return;
		}
	}
	if ((magic[0] | (magic[1] << 8)) == ECOFF_MAGIC)
	{
		printf("Symbols of the last executable (ECOFF): ");
		LoadSymbols(full);
	}
}

// Address: hexadecimal, or a symbol "@name" / "@name+0x10" (symbol file loaded automatically or with -sym)
bool	ParseAddress(char *str, uint32_t *adrs, char *symfile)
{
	char	name[256], *plus;
	int		i;

	if (str[0] != '@')
	{
		*adrs = atohex(str);
		return (true);
	}
	AutoLoadSymbols(symfile);
	strncpy(name, str + 1, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if ((plus = strchr(name, '+')))
		*plus++ = 0;
	for (i = 0; i < symbol_count; i++)
		if (!strcmp(symbols[i].name, name))
		{
			*adrs = symbols[i].adrs + (plus ? strtoul(plus, NULL, 0) : 0);
			printf("%s = 0x%08X\n", str, *adrs);
			return (true);
		}
	printf("Symbol '%s' not found%s\n", name, symbol_count ? "" : " (no symbol file: use -sym file, or run the program with -e first)");
	return (false);
}

// Nearest symbol at or before the address
Symbol	*FindSymbol(uint32_t adrs)
{
	int	low = 0, high = symbol_count - 1, mid;

	if (!symbol_count || (adrs < symbols[0].adrs))
		return (NULL);
	while (low < high)
	{
		mid = (low + high + 1) / 2;
		if (symbols[mid].adrs <= adrs)
			low = mid;
		else
			high = mid - 1;
	}
	return ((adrs - symbols[low].adrs < SYMBOL_MAX_OFFSET) ? &symbols[low] : NULL);
}

// Name and start of the symbol at or before an address (profiler), NULL if none
const char	*SymbolAt(uint32_t adrs, uint32_t *start)
{
	Symbol	*s = FindSymbol(adrs);

	if (!s)
		return (NULL);
	*start = s->adrs;
	return (s->name);
}

// A symbol of this name is loaded (profiler: the waiting functions of the program are known)
bool	SymbolExists(const char *name)
{
	int	i;

	for (i = 0; i < symbol_count; i++)
		if (!strcmp(symbols[i].name, name))
			return (true);
	return (false);
}

void	PrintAddress(uint32_t adrs)
{
	Symbol	*s = FindSymbol(adrs);

	printf("%08X", adrs);
	if (s)
		printf("  %s + 0x%X", s->name, adrs - s->adrs);
	printf("\n");
}

// RAM address seen through KSEG0 (the BIOS stores some pointers as physical addresses 0x0000xxxx, or KSEG1 0xA0xxxxxx)
uint32_t	RamAddress(uint32_t adrs)
{
	return (((adrs & 0x1FFFFFFF) < (RAM_END - RAM_START)) ? (RAM_START | (adrs & 0x1FFFFFFF)) : adrs);
}

bool	ReadWord(uint32_t adrs, uint32_t *value)
{
	adrs = RamAddress(adrs);
	return (((adrs & 3) == 0) && (adrs >= RAM_START) && (adrs < RAM_END) && USB_Process('D', (uint8_t *)value, adrs, 4));
}

// Function start: symbol, or the nearest "addiu sp,sp,-N" before pc (searched in the 1KB before it)
uint32_t	FunctionStart(uint32_t pc)
{
	Symbol		*s = FindSymbol(pc);
	uint32_t	code[256], start = (pc - RAM_START > 1024) ? pc - 1024 : RAM_START, n, i;

	if (s)
		return (s->adrs);
	n = (pc - start) / 4;
	if (!n || !USB_Process('D', (uint8_t *)code, start, n * 4))
		return (0);
	for (i = n; i-- > 0;)
		if ((code[i] & 0xFFFF8000) == 0x27BD8000)	// addiu sp,sp,-N
			return (start + i * 4);
	return (0);
}

// Walk the frames: each function prologue gives its frame size ("addiu sp,sp,-N") and where ra is saved
// ("sw ra,off(sp)"). Only the instructions before pc have been executed (frame not set up yet at the start).
int		UnwindStack(uint32_t *regs)
{
	uint32_t	pc = RamAddress(regs[32]), sp = RamAddress(regs[29]), ra = regs[31], start, code[256], n, i, frame, saved, at;
	int			depth, ra_off;

	printf("\nCall stack (from the function prologues, most recent first, callers shown at their call):\n");
	for (depth = 0; depth < CRASH_MAX_CALLERS; depth++)
	{
		// Callers: the call instruction (return address - 8), the return address can be after the end of the
		// function when it ends with a call that never returns
		at = depth ? pc - 8 : pc;
		printf("  #%-2d ", depth);
		PrintAddress(at);
		if (!(start = FunctionStart(at)) || (pc - start > sizeof(code)))
			break;
		n = (pc - start) / 4;
		if (n && !USB_Process('D', (uint8_t *)code, start, n * 4))
			break;
		frame = 0;
		ra_off = -1;
		for (i = 0; i < n; i++)
		{
			if (!frame && ((code[i] & 0xFFFF8000) == 0x27BD8000))		// addiu sp,sp,-N
				frame = -(int16_t)(code[i] & 0xFFFF);
			else if ((ra_off < 0) && ((code[i] & 0xFFFF0000) == 0xAFBF0000))	// sw ra,off(sp)
				ra_off = (int16_t)(code[i] & 0xFFFF);
		}
		if (ra_off >= 0)		// ra saved in this frame
		{
			if (!ReadWord(sp + ra_off, &saved))
				break;
			ra = saved;
		}
		else if (depth > 0)	// A caller always saves ra: unknown prologue
			break;
		sp += frame;
		// Next frame: the return address must point after a call in RAM
		if ((ra & 3) || (RamAddress(ra) < RAM_START + 0x10000) || (RamAddress(ra) >= RAM_END) || (RamAddress(ra) == pc))
			break;
		pc = RamAddress(ra);
	}
	return (depth + 1);
}

// 16-bit VRAM pixels (w * h) to a 24-bit PNG file
static bool	VramToPng(const char *filename, const uint16_t *vram, int w, int h)
{
	uint8_t	*rgb = malloc(w * h * 3), *p = rgb;
	int		i;
	bool	ok;

	if (!rgb)
		return (false);
	for (i = 0; i < w * h; i++)
	{
		uint16_t	color = vram[i];
		uint8_t		r = color & 31;
		uint8_t		g = (color >> 5) & 31;
		uint8_t		b = (color >> 10) & 31;

		*p++ = (r << 3) | (r >> 2);
		*p++ = (g << 3) | (g >> 2);
		*p++ = (b << 3) | (b >> 2);
	}
	ok = !lodepng_encode24_file(filename, rgb, w, h);
	free(rgb);
	return (ok);
}

// Last frame displayed by the crashed program (monitor version 5): the crash screen is drawn in the program's draw
// area, the buffer it was drawing, so the frame it displayed is still in VRAM, above or below it (double buffering)
static void	CrashFrame(uint32_t info, const char *png)
{
	static const int	widths[4] = {256, 320, 512, 640};
	uint32_t	mode = (info >> 20) & 0x7F;	// GPUSTAT bits 16-22
	int			x = info & 0x3FF, y = (info >> 10) & 0x3FF, w, h, other, i, fy = -1, cand[4];
	uint16_t	*vram;

	w = (mode & 0x01) ? 368 : widths[(mode >> 1) & 3];
	h = (mode & 0x10) ? 256 : 240;
	other = (h == 256) ? 240 : 256;	// (PAL programs may use 240 lines, NTSC ones 256)
	if ((mode & 0x08) && (mode & 0x40))	// 480 lines, interlaced: one buffer
		h *= 2;
	cand[0] = y - h;
	cand[1] = y + h;
	cand[2] = (h == 480) ? -1 : y - other;
	cand[3] = (h == 480) ? -1 : y + other;
	for (i = 0; (i < 4) && (fy < 0); i++)
		if ((cand[i] >= 0) && (cand[i] + h <= 512))
			fy = cand[i];
	if (x + w > 1024)
		w = 1024 - x;
	if (fy < 0)
	{
		printf("\nLast frame: not kept (one display buffer: the crash screen is drawn over it)\n");
		return;
	}
	printf("\nLast frame displayed (double buffered programs): VRAM %d,%d %dx%d%s\n", x, fy, w, h,
		(mode & 0x20) ? " (24-bit video mode)" : "");
	printf("  (a program drawing where it displays has no second buffer: this area holds older VRAM contents)\n");
	if (!png)
	{
		printf("  Save it with -f: ps1transfer -crash [game.map] -f frame.png\n");
		return;
	}
	vram = malloc(w * h * sizeof(uint16_t));
	usb_quiet = true;
	if (vram && USB_Process('V', (uint8_t *)vram, (x << 16) | fy, (w << 16) | h) && VramToPng(png, vram, w, h))
		printf("  Saved to %s\n", png);
	else
		printf("  Can't save it to %s\n", png);
	usb_quiet = false;
	free(vram);
}

bool	CrashReport(char *symfile, const char *frame_png)
{
	MonitorState	ms;
	static const char	*cause_str[] = {"Interrupt", "TLB Modify", "TLB miss on load", "TLB miss on store",
							"Address error on load", "Address error on store", "Bus error on instruction fetch",
							"Bus error on data access", "System call", "Breakpoint instruction",
							"Reserved (illegal) instruction", "Coprocessor unusable", "Integer overflow", "Reserved"};
	static const char	*reg_names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
							"t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9",
							"k0", "k1", "gp", "sp", "fp", "ra"};
	uint32_t			tcbh, tcb, regs[40], stack[CRASH_STACK_SIZE / 4], sp, size, code, i, word, callers = 0;
	Symbol				*func;

	AutoLoadSymbols(symfile);

	// Registers saved by the BIOS exception handler in the current task control block
	if (!ReadWord(BIOS_TCBH_PTR, &tcbh) || !ReadWord(tcbh, &tcb) || !USB_Process('D', (uint8_t *)regs, RamAddress(tcb) + 8, sizeof(regs)))
	{
		printf("\nCan't read the registers: is the PS1 DEV CART crash screen displayed ? (needs the PS1 DEV CART firmware)\n");
		return (false);
	}
	if (!ps1_program_running)
		printf("\nNote: no program was executed since the last boot, these are the registers of the last exception.\n");
	if (USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)) && (ms.magic == MONITOR_MAGIC) &&
		(ms.version >= 5) && (ms.reason == MON_REASON_CRASH) && (ms.prof_draw & MON_CRASH_FRAME))
		CrashFrame(ms.prof_draw, frame_png);

	code = (regs[36] >> 2) & 0x1F;
	printf("\nCrash: %s (CAUSE %08X)\n", cause_str[(code < 13) ? code : 13], regs[36]);
	printf("PC  ");
	PrintAddress(regs[32]);
	if (regs[36] & 0x80000000)
		printf("    (in the branch delay slot: the faulting instruction is at PC + 4)\n");
	printf("RA  ");
	PrintAddress(regs[31]);

	printf("\nRegisters:\n");
	regs[0] = 0;	// r0 is always 0 (the BIOS uses this TCB slot for something else)
	for (i = 0; i < 32; i++)
		printf("  %-4s %08X%s", reg_names[i], regs[i], ((i & 3) == 3) ? "\n" : "");
	printf("  hi   %08X  lo   %08X  sr   %08X\n", regs[33], regs[34], regs[35]);

	// Return addresses on the stack: values just after a jal/jalr instruction (functions save ra on the stack)
	sp = RamAddress(regs[29]);
	if ((sp < RAM_START) || (sp >= RAM_END) || (sp & 3))
	{
		printf("\nInvalid stack pointer, no call chain.\n");
		return (true);
	}
	size = RAM_END - sp;
	if (size > CRASH_STACK_SIZE)
		size = CRASH_STACK_SIZE;
	if (!USB_Process('D', (uint8_t *)stack, sp, size))
		return (false);

	// Call stack from the function prologues (frame size and saved ra), the stack scan below is used if it stops early
	if (UnwindStack(regs) > 1)
		goto report_end;

	// With symbols, a return address must follow a "jal" to the function reached so far (start of the chain: PC),
	// so return addresses of calls already finished, still in the stack, are skipped.
	// A "jalr" (call through a function pointer) can't be checked: shown with "?".
	func = FindSymbol(RamAddress(regs[32]));
	printf("\n%s call chain, stack scan (most recent first):\n", symbol_count ? "Checked" : "Probable");
	for (i = 0; (i < size / 4 + 1) && (callers < CRASH_MAX_CALLERS); i++)
	{
		uint32_t	value = (i == 0) ? regs[31] : stack[i - 1];
		bool		is_jal, is_jalr;

		if ((value & 3) || (RamAddress(value) < RAM_START + 8) || (RamAddress(value) >= RAM_END) ||
			((i > 0) && (value == regs[31])) || (symbol_count && !FindSymbol(value)) || !ReadWord(value - 8, &word))
			continue;
		is_jal = ((word >> 26) == 3);
		is_jalr = (((word >> 26) == 0) && ((word & 0x3F) == 9));
		if (!is_jal && !is_jalr)
			continue;
		if (symbol_count && is_jal && func && ((((value - 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)) != func->adrs))
			continue;	// Calls another function: stale return address
		if (i == 0)
			printf("  ra        %c ", is_jalr ? '?' : ' ');
		else
			printf("  sp+0x%03X  %c ", (i - 1) * 4, is_jalr ? '?' : ' ');
		PrintAddress(value);
		func = FindSymbol(value);
		callers++;
	}

report_end:
	if (!symbol_count)
		printf("\nGive a symbol file to see function names: ps1transfer -crash game.map (.map, .sym or .elf),\n"
			"or put it next to the executable run with -e (same name) to load it automatically.\n");
	printf("\nThe crash screen is still displayed: read more memory with -d, a screenshot with -v, or reset with -r.\n");

	return (true);
}

/****************************************/

/****************************************/
// "-flash rom.rom" flashes a cartridge ROM. The ROM is uploaded to RAM with a header (magic, size, sum),
// the flasher (ar_flash.ps-exe, next to ps1transfer) flashes it and sends its status lines, then the PS1 is reset and
// the flash is read back and compared (except the last sector: the device ID, written again by the new firmware).

#define	FLASH_ROM_SIZE		(128 * 1024)
#define	FLASH_IMAGE_ADRS	0x80100000		// See ps1-tools/ar_flash/ar_flash.c
#define	FLASH_ID_SECTOR		0x1F000			// Device ID sector, not compared
#define	FLASHER_FILENAME	"ar_flash.ps-exe"
#define	FLASH_TIMEOUT		60				// Seconds for the flasher to report

static void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

// The ROM file, checked before anything is done with the PS1 (NULL if it isn't a cartridge ROM)
static uint8_t	*LoadRom(char *rom_file)
{
	uint8_t	*rom;
	int		size;

	if (!(rom = SystemGetFile(rom_file, &size)))
		return (NULL);
	if ((size != FLASH_ROM_SIZE) || memcmp(rom + 0x84, "Licensed by Sony Computer Entertainment Inc.", 44))
	{
		printf("'%s' is not a cartridge ROM (128KB with the cartridge boot header), nothing flashed.\n",
			rom_file);
		free(rom);
		return (NULL);
	}
	return (rom);
}

static bool	FlashRom(char *rom_file, uint8_t *rom, char *argv0)
{
	uint8_t		*image = NULL, *exe = NULL, *readback = NULL, c;
	int			size = FLASH_ROM_SIZE, exe_size, i, len = 0;
	uint32_t	sum = 0, exe_adrs;
	char		line[256];
	time_t		start;
	bool		ok = false, done = false;

	if (!(exe = SystemGetFile(GetToolPath(argv0, FLASHER_FILENAME), &exe_size)) || !(image = calloc(1, 32 + size)))
		goto out;

	// ROM image in RAM: "PS1 DEV CART ROM", size, sum, padding, ROM
	memcpy(image, "PS1 DEV CART ROM", 16);
	for (i = 0; i < size; i++)
		sum += rom[i];
	((uint32_t *)image)[4] = size;
	((uint32_t *)image)[5] = sum;
	memcpy(image + 32, rom, size);
	printf("Uploading the ROM %s...\n", rom_file);
	if (!USB_Process('U', image, FLASH_IMAGE_ADRS, 32 + size))
		goto usb_error;

	// Flasher, with printf to USB: its status lines end with an EOT character
	exe_adrs = *(uint32_t *)&exe[0x18];
	printf("Starting the flasher...\n");
	if (!USB_Process('U', exe + 2048, exe_adrs, exe_size - 2048) ||
		!USB_Process('E', exe + 16, EXEC_TTY_MODE_USB, 15 * 4))
		goto usb_error;
	for (start = time(NULL); !done && (time(NULL) - start < FLASH_TIMEOUT); )
	{
		if (!USB_ReadByteNoWait(&c))
		{
			SleepMs(1);
			continue;
		}
		if ((c == 4) || (c == '\n') || (len >= sizeof(line) - 1))
		{
			line[len] = 0;
			if (len && (line[len - 1] == '\r'))
				line[--len] = 0;
			if (!strncmp(line, "FLASH", 5))	// Status lines (not the text of the libraries)
				printf("%s\n", line);
			if (!strncmp(line, "FLASH OK", 8))
				ok = true;
			len = 0;
			done = (c == 4);
		}
		else if (c != '\r')
			line[len++] = c;
	}
	if (!done)
		printf("The flasher didn't answer (look at the TV): the flash may not be written.\n");
	if (!ok)
		goto out;

	// New firmware: reset, then compare the flash with the ROM
	printf("Resetting the PS1...\n");
	USB_Process('R', NULL, 0, 0);
	SleepMs(5000);
#ifdef _WIN32
	FT_Purge(ftdi, FT_PURGE_RX | FT_PURGE_TX);
#endif
	ps1_program_running = true;
	USB_DetectPacketSize();
	if (fw_features & FEATURE_DEVICE_ID)
		USB_SendDeviceId();		// The device ID sector was erased
	if (!(readback = malloc(size)))
		goto out;
	printf("Reading the flash back...\n");
	if (!USB_Process('D', readback, 0x1F000000, size))
		goto usb_error;
	for (i = 0; (i < FLASH_ID_SECTOR) && (readback[i] == rom[i]); i++)
		;
	if (i < FLASH_ID_SECTOR)
	{
		printf("\nReadback differs from the ROM at offset 0x%05X: flash again.\n", i);
		ok = false;
	}
	else
		printf("\nFlash written and checked: identical to %s.\n", rom_file);
	goto out;

usb_error:
	printf("\nUSB error.\n");
	ok = false;
out:
	free(image);
	free(exe);
	free(readback);
	return (ok);
}

/****************************************/
// Memory card commands on the card itself. "slot1" or "slot2" in place of the image file of -mclist,
// -mcexport, -mcimport, -mcdelete and -mccopy. The card tool runs in server mode (ps1-tools/cardtool/cardserver.h) and only
// the sectors needed are read or written: the directory, the title/icon frames of the saves, the blocks of a save.
// The image functions above work on a partial image of the card in memory; the sectors they change are written.

#define	CARD_MODE_SERVER	3
#define	CARD_SECTOR			128
#define	CARD_BLOCK_SECTORS	64
#define	CS_FIELD(_f_)		(CARDSRV_MAILBOX_ADRS + offsetof(CardServerMailbox, _f_))

typedef struct
{
	int		slot;
	uint8_t	img[CARD_SIZE];			// Sectors read (or changed by the image functions)
	uint8_t	orig[CARD_SIZE];		// Sectors as read from the card
	bool	have[CARDSRV_SECTORS];
} LiveCard;

static int	CardSlotArg(char *arg)
{
	return (!strcmp(arg, "slot1") ? 1 : (!strcmp(arg, "slot2") ? 2 : 0));
}

static bool	CardServerRunning(void)
{
	uint32_t	magic = 0;

	return (USB_Process('D', (uint8_t *)&magic, CS_FIELD(magic), 4) && (magic == CARDSRV_MAGIC));
}

// Uploads and executes the card tool in server mode (the PS1 is at the menu)
static bool	CardServerStart(char *argv0)
{
	uint8_t		*exe, header[CARD_IMAGE_OFFSET];
	uint32_t	zero = 0;
	int			size, i;
	bool		ok;

	if (!(exe = SystemGetFile(GetToolPath(argv0, CARD_FILENAME), &size)))
		return (false);
	memset(header, 0, sizeof(header));
	((uint32_t *)header)[0] = CARD_MAGIC;
	((uint32_t *)header)[1] = CARD_MODE_SERVER;
	printf("Starting the memory card tool...\n");
	// The magic of a previous run may still be in RAM: cleared, the card tool sets it once ready
	ok = USB_Process('U', (uint8_t *)&zero, CS_FIELD(magic), 4) && USB_Process('U', header, CARD_DATA_ADRS, sizeof(header)) &&
		USB_Process('U', exe + 2048, *(uint32_t *)&exe[0x18], size - 2048) &&
		USB_Process('E', exe + 16, EXEC_TTY_MODE_BIOS, 15 * 4);
	free(exe);
	if (!ok)
		return (false);
	ps1_program_running = true;
	usb_big_packet_size = 0;
	for (i = 0; i < 50; i++)
	{
		SleepMs(100);
		if (CardServerRunning())
			return (true);
	}
	printf("The memory card tool didn't start.\n");
	return (false);
}

// Runs a read or write of the sectors in the list (already in the image buffer for a write)
static bool	CardServerCommand(int cmd, int slot, uint16_t *sectors, int count, const char *text)
{
	struct
	{
		uint32_t	cmd, slot, count;
		char		text[CARDSRV_TEXT];
	}			fields;
	uint32_t	seq = 0, done = 0, error_sector = 0;
	int32_t		result = 0;
	time_t		start;

	memset(&fields, 0, sizeof(fields));
	fields.cmd = cmd;
	fields.slot = slot;
	fields.count = count;
	if (text)
		strncpy(fields.text, text, CARDSRV_TEXT - 1);
	if (!USB_Process('D', (uint8_t *)&seq, CS_FIELD(done_seq), 4) ||
		!USB_Process('U', (uint8_t *)&fields, CS_FIELD(cmd), sizeof(fields)) ||
		!USB_Process('U', (uint8_t *)sectors, CS_FIELD(sectors), (count * 2 + 3) & ~3))
		goto usb_error;
	seq++;
	if (!USB_Process('U', (uint8_t *)&seq, CS_FIELD(cmd_seq), 4))
		goto usb_error;
	if (cmd == CARDSRV_WRITE)
		printf("Confirm on the PS1: Cross to write %d sector(s) of the card in slot %d, Circle to cancel.\n", count, slot);
	for (start = time(NULL); done != seq; SleepMs(20))
	{
		if (!USB_Process('D', (uint8_t *)&done, CS_FIELD(done_seq), 4))
			goto usb_error;
		if (time(NULL) - start > ((cmd == CARDSRV_WRITE) ? 600 : 60))
		{
			printf("The memory card tool doesn't answer.\n");
			return (false);
		}
	}
	if (!USB_Process('D', (uint8_t *)&result, CS_FIELD(result), 4) ||
		!USB_Process('D', (uint8_t *)&error_sector, CS_FIELD(error_sector), 4))
		goto usb_error;
	switch (result)
	{
		case CARDSRV_OK:			return (true);
		case CARDSRV_NO_CARD:		printf("No memory card in slot %d.\n", slot); break;
		case CARDSRV_IO_ERROR:		printf("Memory card error %s sector %u.\n", (cmd == CARDSRV_WRITE) ? "writing" : "reading",
										error_sector); break;
		case CARDSRV_CANCELLED:		printf("Cancelled on the PS1, the card was not modified.\n"); break;
		default:					printf("The memory card tool refused the command (%d).\n", result);
	}
	return (false);

usb_error:
	printf("USB error.\n");
	return (false);
}

// Reads the sectors first..first+count-1 not read yet
static bool	LiveRead(LiveCard *c, int first, int count)
{
	uint16_t	list[CARDSRV_SECTORS];
	int			n = 0, i, run;

	for (i = first; i < first + count; i++)
		if (!c->have[i])
			list[n++] = i;
	if (!n)
		return (true);
	if (!CardServerCommand(CARDSRV_READ, c->slot, list, n, NULL))
		return (false);
	for (i = 0; i < n; i += run)	// Runs of consecutive sectors from the image buffer
	{
		for (run = 1; (i + run < n) && (list[i + run] == list[i] + run); run++)
			;
		if (!USB_Process('D', c->img + list[i] * CARD_SECTOR, CARDSRV_IMAGE_ADRS + list[i] * CARD_SECTOR, run * CARD_SECTOR))
		{
			printf("USB error.\n");
			return (false);
		}
	}
	for (i = 0; i < n; i++)
	{
		memcpy(c->orig + list[i] * CARD_SECTOR, c->img + list[i] * CARD_SECTOR, CARD_SECTOR);
		c->have[list[i]] = true;
	}
	return (true);
}

// Header and directory frames (sectors 0-15)
static bool	LiveDirectory(LiveCard *c)
{
	if (!LiveRead(c, 0, 16))
		return (false);
	if (memcmp(c->img, "MC", 2))
	{
		printf("The card in slot %d is not formatted (no \"MC\" header).\n", c->slot);
		return (false);
	}
	return (true);
}

// Writes what the image functions changed: the directory frames, and the whole blocks of the saves written
static bool	LiveWriteChanges(LiveCard *c, const char *text)
{
	uint16_t	list[CARDSRV_SECTORS];
	int			n = 0, s, b, i, run;

	for (s = 0; s < 16; s++)
	{
		if (!memcmp(c->img + s * CARD_SECTOR, c->orig + s * CARD_SECTOR, CARD_SECTOR))
			continue;
		list[n++] = s;
		b = s;		// Directory frame s = block s
		if ((b >= 1) && (c->img[b * CARD_SECTOR] >= MC_STATE_FIRST) && (c->img[b * CARD_SECTOR] <= MC_STATE_LAST))
			for (i = 0; i < CARD_BLOCK_SECTORS; i++)
				list[n++] = b * CARD_BLOCK_SECTORS + i;
	}
	if (!n)
	{
		printf("Nothing to write.\n");
		return (true);
	}
	for (i = 0; i < n; i += run)
	{
		for (run = 1; (i + run < n) && (list[i + run] == list[i] + run); run++)
			;
		if (!USB_Process('U', c->img + list[i] * CARD_SECTOR, CARDSRV_IMAGE_ADRS + list[i] * CARD_SECTOR, run * CARD_SECTOR))
		{
			printf("USB error.\n");
			return (false);
		}
	}
	return (CardServerCommand(CARDSRV_WRITE, c->slot, list, n, text));
}

// Whole blocks of a save
static bool	LiveSaveBlocks(LiveCard *c, int first)
{
	int	blocks[15], n = McChain(c->img, first, blocks), i;

	for (i = 0; i < n; i++)
		if (!LiveRead(c, blocks[i] * CARD_BLOCK_SECTORS, CARD_BLOCK_SECTORS))
			return (false);
	return (true);
}

// A save as .mcs data from a card slot or an image file
static uint8_t	*SaveFrom(char *source, char *save, int *size)
{
	LiveCard	*c;
	uint8_t		*img, *mcs = NULL;
	int			first;

	if (CardSlotArg(source))
	{
		c = calloc(1, sizeof(LiveCard));
		c->slot = CardSlotArg(source);
		if (LiveDirectory(c) && (first = McFind(c->img, save)) && LiveSaveBlocks(c, first))
			mcs = McExport(c->img, first, size);
		free(c);
	}
	else if ((img = McLoadImage(source)))
	{
		if ((first = McFind(img, save)))
			mcs = McExport(img, first, size);
		free(img);
	}
	return (mcs);
}

static bool	SaveName(uint8_t *mcs, char *name)
{
	memcpy(name, mcs + 10, 20);
	name[20] = 0;
	return (true);
}

// -mclist/-mcexport/-mcimport/-mcdelete/-mccopy with slot1/slot2: returns the exit code
static int	CardLiveCommand(int argc, char **argv, int i, char *argv0, bool start)
{
	char		*cmd = argv[i], *icons = NULL, text[CARDSRV_TEXT], name[24];
	LiveCard	*c = calloc(1, sizeof(LiveCard));
	uint8_t		*mcs = NULL, *img = NULL;
	int			first, size, b, ret = EXIT_FAILURE, need = !strcmp(cmd, "-mclist") ? 1 :
					((!strcmp(cmd, "-mccopy") || !strcmp(cmd, "-mcexport")) ? 3 : 2);

	setvbuf(stdout, NULL, _IONBF, 0);
	if (i + need >= argc)
	{
		printf("%s: missing parameters, see the usage\n", cmd);
		goto out;
	}
	if (!strcmp(cmd, "-mcformat"))
	{
		printf("-mcformat only makes images (format a card with the BIOS memory card manager).\n");
		goto out;
	}
	if (start && !CardServerStart(argv0))
		goto out;
	ps1_program_running = true;
	usb_big_packet_size = 0;
	c->slot = CardSlotArg(argv[i + 1]);

	if (!strcmp(cmd, "-mclist"))
	{
		if ((i + 3 < argc) && !strcmp(argv[i + 2], "-icons"))
			icons = argv[i + 3];
		if (!LiveDirectory(c))
			goto out;
		for (b = 1; b < 16; b++)	// Title frame and icon frames of each save
			if ((McFrame(c->img, b)[0] == MC_STATE_FIRST) && !LiveRead(c, b * CARD_BLOCK_SECTORS, 4))
				goto out;
		printf("Memory card in slot %d:\n", c->slot);
		McList(c->img, icons);
		ret = EXIT_SUCCESS;
	}
	else if (!strcmp(cmd, "-mcexport"))
	{
		if ((mcs = SaveFrom(argv[i + 1], argv[i + 2], &size)) && McSaveFile(argv[i + 3], mcs, size, false))
		{
			printf("Save exported: %s (%d bytes)\n", argv[i + 3], size);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcimport"))
	{
		if ((mcs = SystemGetFile(argv[i + 2], &size)) && LiveDirectory(c) && McImport(c->img, mcs, size) &&
			SaveName(mcs, name))
		{
			snprintf(text, sizeof(text), "IMPORT %s", name);
			if (LiveWriteChanges(c, text))
			{
				printf("Save written to the card in slot %d.\n", c->slot);
				ret = EXIT_SUCCESS;
			}
		}
	}
	else if (!strcmp(cmd, "-mcdelete"))
	{
		if (LiveDirectory(c) && (first = McFind(c->img, argv[i + 2])))
		{
			memcpy(name, McFrame(c->img, first) + 10, 20);
			name[20] = 0;
			McDelete(c->img, first);
			snprintf(text, sizeof(text), "DELETE %s", name);
			if (LiveWriteChanges(c, text))
				ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mccopy"))	// src save dst: each one a card slot or an image file
	{
		if (!(mcs = SaveFrom(argv[i + 1], argv[i + 2], &size)))
			goto out;
		if (CardSlotArg(argv[i + 3]))
		{
			c->slot = CardSlotArg(argv[i + 3]);
			memset(c->have, 0, sizeof(c->have));
			SaveName(mcs, name);
			snprintf(text, sizeof(text), "IMPORT %s", name);
			if (LiveDirectory(c) && McImport(c->img, mcs, size) && LiveWriteChanges(c, text))
				ret = EXIT_SUCCESS;
		}
		else if ((img = McLoadImage(argv[i + 3])) && McImport(img, mcs, size) && McSaveFile(argv[i + 3], img, CARD_SIZE, true))
		{
			McList(img, NULL);
			ret = EXIT_SUCCESS;
		}
	}
	else
		printf("%s doesn't work on a card slot.\n", cmd);

out:
	free(c);
	free(mcs);
	free(img);
	return (ret);
}

/****************************************/
// "-info" prints the cartridge state as key=value lines (for the GUI and scripts)

static void	PrintInfo(void)
{
	uint8_t		params[4], state[16];
	char		serial[64];
	bool		have_state;
	const char	*status;

	printf("device=%s\n", USB_GetSerial(serial) ? serial : "");
	if (!USB_Process('D', params, KERNEL_HOOK_PARAMS_ADRS, sizeof(params)))
	{
		// Original firmware while a program runs, or nothing answers
		printf("state=unknown\n");
		return;
	}
	have_state = params[3] && USB_Process('D', state, MONITOR_STATE_ADRS, sizeof(state)) &&
		(*(uint32_t *)state == MONITOR_MAGIC);
	if (!params[0])
		status = "menu";
	else if (have_state && (state[12] == MON_REASON_CRASH))
		status = "crashed";
	else if (have_state && (state[12] >= MON_REASON_BREAK) && (state[12] <= MON_REASON_PAUSE))
		status = "stopped";
	else
		status = "running";
	printf("state=%s\n", status);
	printf("firmware=%s\n", params[3] ? "devcart" : "original");
	if (have_state && state[15])
		printf("firmware_version=%d.%d\n", state[15] >> 4, state[15] & 15);
	printf("features=0x%02X\n", params[3]);
	printf("tty_mode=%d\n", params[1] & ~EXEC_DEBUG);
	printf("debug_session=%d\n", (params[1] & EXEC_DEBUG) ? 1 : 0);
	printf("monitor=%d\n", (have_state && *(uint32_t *)(state + 4)) ? 1 : 0);
	printf("monitor_version=%d\n", have_state ? state[14] : 0);
	printf("escaped_answers=%d\n", (USB_EscapedAnswers() > 0) ? 1 : 0);
}

/****************************************/
// Hot reload: files sent to the running program without restarting it. VRAM and SPU RAM are written by
// the debug monitor ('M' service byte, then a 'W'/'S' command, see monitor/monitor.c), RAM by the kernel hook ('U').
//   file.tim				VRAM at the TIM position (image and CLUT), file.tim@x,y: image at another position
//   file.vag@0x1010		SPU RAM (VAG header skipped), also .vb (VAB body) or any file, address < 0x80000
//   file.bin@0x80123456	RAM

#define	HOT_MAX_FILES	16

typedef struct
{
	char	*name;			// Argument: file[@where]
	time_t	mtime;			// -watch: last sent version
	long	size;
} HotFile;

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
			// Image block (the last one) at another position with file.tim@x,y
			if (has_target && (!(flags & 8) || (p != data + 8)))
				sscanf(at, "%u,%u", &x, &y);
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
static bool	HotReload(HotFile *files, int count, bool watch)
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

int main(int argc, char *argv[])
{
	int			deviceid = 0;
	int			x, y, w, h, i, ret, cmd = CMD_UNKNOWN;
	uint32_t	start_ms;						// Upload start (its rate and time shown)
	uint32_t	size = 0, adrs = 0x80010000, *ptr32, poke_value = 0;
	char		*adrs_str = NULL;
	uint8_t		*data = NULL, *ptr;
	char		*filename = NULL, *path = NULL, *serial = NULL, *libps = NULL, *auto_script = NULL;
	char		*model = NULL, *tims[VIEWER_MAX_TIM], *symfile = NULL, *sound_files[TESTER_MAX_FILES];
	char		*timview_files[TIMVIEW_MAX_FILES];
	int			sound_count = 0, timview_count = 0, card_mode = 0, card_slot = 1;
	bool		link_test = false;
	bool		pad_test = false;	// -padtest: the control tester, with the remote pad
	char		*card_file = NULL;
	int			tim_count = 0;
	bool		ttymode = false, reset = false, list = false;
	int			tty_mode = EXEC_TTY_MODE_USB;	// Default = redirect enabled (old behaviour)
	bool		tty_given = false;				// -c given
	char		*net_host = NULL;				// -net host:port
	ProfOptions	prof = {1000, 0, false, NULL, NULL, 0, NULL};	// -prof [rate[,counter]], -json, -csv, -idle
	bool		prof_on = false, prof_fill = true;	// -nofill: no RAM fill (RAM use not measured)
	bool		tim_live = false;	// -timlive: the TIM viewer linked with the GUI (timlive.c)
	bool		snd_live = false;	// -sndlive: the sound tester linked with the GUI (sndlive.c)
	bool		mdl_live = false;	// -mdllive: the model viewer linked with the GUI (mdllive.c)
	bool		remote_pad = false;	// -pad: a controller port driven from the computer (monitor/monitor.h, EXEC_PAD)
	char		*tod_files[VIEWER_MAX_TOD];
	int			tod_count = 0;
	int			gdb_port = 0;					// -gdb: debug session with the GDB server on this port
	bool		frame_live = false;				// -frame: debug session for the frame capture (gdbserver.c)
	uint32_t	gdb_entry = 0, gdb_entry_insn = 0;
	HotFile		hot_files[HOT_MAX_FILES];			// -hot: files sent to the running program
	int			hot_count = 0;
	bool		hot_watch = false;
	char		*flash_rom = NULL;				// -flash: cartridge ROM to flash
	uint8_t		*flash_data = NULL;
	bool		synth_start = true;			// -synth: the synth must be started
	int			card_live_arg = 0;				// Memory card command on a card slot (argv index)
	bool		card_start = true;				// The card tool must be started (server mode)
	char		*relay_serial = NULL;			// -relay: the other cartridge of the USB link
	int			exit_code = 0;					// EXIT_FAILURE after an error (used by the GUI and scripts)

	printf("PS1 DEV CART Transfer tool v%s\n\n", PS1TRANSFER_VERSION);

	// Handle arguments list
	for (i = 1; i < argc; i++)
	{
		if (argv[i][0] == '-')
		{
			switch (argv[i][1])
			{
				case 'u':	// Upload file to specified address
					cmd = CMD_UPLOAD;
				break;

				case 'b':	// The menu boots the disc in the drive (unlocked first: burned and import discs)
					if (!strcmp(argv[i], "-bootcd"))
						cmd = CMD_BOOT_DISC;
				break;

				case 'd':	// Download file from specified address and size
					cmd = CMD_DOWNLOAD;
				break;

				case 'e':	// Execute psexe file
					cmd = CMD_EXECUTE;
				break;

				case 'r':	// Reset the PS1
					if (!strcmp(argv[i], "-relay"))	// USB link relay with another cartridge (usblink.c)
					{
						if (i + 1 >= argc)
						{
							printf("Serial number of the other cartridge needed following -relay\n");
							goto usage;
						}
						relay_serial = argv[++i];
						if (!gdb_port)		// (with -gdb: the relay in the debug session)
							cmd = CMD_RELAY;
						break;
					}
					reset = true;
				break;

				case 'i':	// Cartridge state as key=value lines (GUI, scripts)
					if (!strcmp(argv[i], "-idle"))	// -prof: more waiting functions (names or addresses, commas)
					{
						if (i + 1 >= argc)
						{
							printf("Function names needed following -idle\n");
							goto usage;
						}
						prof.idle = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-info"))
						cmd = CMD_INFO;
				break;

				case 'h':	// Hot reload: -hot file[@where] (several), -watch
					if (!strcmp(argv[i], "-hot"))
					{
						if ((i + 1 >= argc) || (hot_count >= HOT_MAX_FILES))
						{
							printf("File needed following -hot (max %d)\n", HOT_MAX_FILES);
							goto usage;
						}
						hot_files[hot_count++].name = argv[++i];
						cmd = CMD_HOT;
					}
				break;

				case 'w':	// Flash Action Replay
					if (!strcmp(argv[i], "-watch"))	// Hot reload: send the files again when they change
					{
						hot_watch = true;
						break;
					}
					cmd = CMD_FLASH;
				break;

				case 'j':
					if (!strcmp(argv[i], "-json"))		// -prof: "PROF {...}" lines (GUI)
						prof.json = true;
				break;

				case 'c':	// TTY Console mode
				if (!strcmp(argv[i], "-csv"))	// -prof: one line per second in a CSV file
				{
					if (i + 1 >= argc)
					{
						printf("File needed following -csv\n");
						goto usage;
					}
					prof.csv = argv[++i];
					break;
				}
				if (!strcmp(argv[i], "-crash"))	// Crash report, optional symbol file (.map or nm output)
				{
					cmd = CMD_CRASH;
					if ((i + 1 < argc) && (argv[i + 1][0] != '-'))
						symfile = argv[++i];
					break;
				}
				{
					// Mode can be attached (-coff, -csio, -clink) or the next argument (-c off, -c sio, -c link)
					char	*opt = argv[i][2] ? &argv[i][2] : ((i + 1 < argc) ? argv[i + 1] : "");
					bool	next = !argv[i][2];

					tty_mode = EXEC_TTY_MODE_USB;
					if (!strcmp(opt, "off") || !strcmp(opt, "0"))	// BIOS TTY device is kept
						tty_mode = EXEC_TTY_MODE_BIOS;
					else if (!strcmp(opt, "sio"))					// Serial port TTY driver, like the Net Yaroze
						tty_mode = EXEC_TTY_MODE_SIO;
					else if (!strcmp(opt, "link"))					// The same driver on the USB link (-relay)
						tty_mode = EXEC_TTY_MODE_LINK;
					else
						next = false;

					if (next)
						i++;

					ttymode = (tty_mode == EXEC_TTY_MODE_USB);		// Can't have console if nothing is sent
					tty_given = true;
				}
				break;

				case 'n':	// open device by serial number (stable when using multiple PS1 DEV CART devices)
					if (!strcmp(argv[i], "-nofill"))	// -prof -e: free RAM not filled (RAM use not measured)
					{
						prof_fill = false;
						break;
					}
					if (!strcmp(argv[i], "-net"))	// The program's USB link connected to a TCP server (usblink.c)
					{
						if (i + 1 >= argc)
						{
							printf("host:port needed following -net\n");
							goto usage;
						}
						net_host = argv[++i];
						if (cmd == CMD_UNKNOWN)		// (with -e: after the program is executed)
							cmd = CMD_NET;
						break;
					}
					if (i + 1 >= argc)
					{
						printf("Serial number needed following -n\n");
						goto usage;
					}
					serial = argv[i + 1];
					i++;
				break;

				case 'l':	// list connected devices
					if (!strcmp(argv[i], "-live"))	// -v: the screen as it is, also at the menu (not the VRAM saved at boot)
					{
						vram_live = true;
						break;
					}
					// Link cable tester: "-link" uses the serial port like a PsyQ game (libcomb, BIOS tty kept),
					// "-link yaroze" like a Net Yaroze game (serial port tty driver, as -c sio). The tester reads
					// the tty mode to know which one.
					if (!strcmp(argv[i], "-link"))
					{
						link_test = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						tty_mode = EXEC_TTY_MODE_BIOS;
						if ((i + 1 < argc) && !strcmp(argv[i + 1], "yaroze"))
						{
							tty_mode = EXEC_TTY_MODE_SIO;
							i++;
						}
						else if ((i + 1 < argc) && !strcmp(argv[i + 1], "usb"))	// USB link through this computer
						{
							tty_mode = EXEC_TTY_MODE_USB;
							i++;
						}
						else if ((i + 1 < argc) && !strcmp(argv[i + 1], "yarozeusb"))	// Net Yaroze tty: on the USB link
						{
							tty_mode = EXEC_TTY_MODE_LINK;
							i++;
						}
						ttymode = false;
						break;
					}
					list = true;
				break;

				case 'm':	// TMD model to display with the model viewer
					if (!strcmp(argv[i], "-memlive"))	// Memory of the running program on request (memlive.c)
					{
						cmd = CMD_MEMLIVE;
						break;
					}
					if (!strcmp(argv[i], "-mdllive"))	// Model viewer: commands of the GUI on the input (mdllive.c)
					{
						mdl_live = true;
						break;
					}
					if (!strcmp(argv[i], "-mclist") || !strcmp(argv[i], "-mcexport") || !strcmp(argv[i], "-mcimport") ||
						!strcmp(argv[i], "-mcdelete") || !strcmp(argv[i], "-mccopy") || !strcmp(argv[i], "-mcformat"))
					{
						// slot1/slot2 instead of an image file (or as -mccopy destination): on the card itself
						if (((i + 1 < argc) && CardSlotArg(argv[i + 1])) ||
							(!strcmp(argv[i], "-mccopy") && (i + 3 < argc) && CardSlotArg(argv[i + 3])))
						{
							card_live_arg = i;
							cmd = CMD_CARD_LIVE;
							i = argc - 1;	// Its parameters follow (give -n before it)
							break;
						}
						return (CardImageCommand(argc, argv, i));	// Memory card image tools: no PS1 needed
					}
					if (!strcmp(argv[i], "-mcbackup") || !strcmp(argv[i], "-mcrestore"))	// Memory card tool
					{
						if (i + 1 >= argc)
						{
							printf("Memory card image file needed following %s\n", argv[i]);
							goto usage;
						}
						card_mode = strcmp(argv[i], "-mcbackup") ? CARD_RESTORE : CARD_BACKUP;
						card_file = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;		// The PS1 only accepts a reset command while a program runs
						ttymode = true;		// Progress text, and PCDRV file server for the backup file
						exit_on_eot = true;	// Until the PS1 program has finished
						break;
					}
					if (i + 1 >= argc)
					{
						printf("TMD file needed following -m\n");
						goto usage;
					}
					model = argv[i + 1];
					cmd = CMD_EXECUTE;
					reset = true;	// The PS1 only accepts a reset command while a program (e.g. a previous model) runs
					i++;
				break;

				case 't':	// TIM texture for the model viewer (can be used several times)
					if (!strcmp(argv[i], "-timlive"))	// TIM viewer: commands of the GUI on the input (timlive.c)
					{
						tim_live = true;
						break;
					}
					if (!strcmp(argv[i], "-tod"))	// TOD animation for the model viewer (with -m)
					{
						if ((i + 1 >= argc) || (tod_count >= VIEWER_MAX_TOD))
						{
							printf("TOD file needed following -tod (max %d)\n", VIEWER_MAX_TOD);
							goto usage;
						}
						tod_files[tod_count++] = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-tim"))	// TIM file for the TIM and VRAM viewer
					{
						if ((i + 1 >= argc) || (timview_count >= TIMVIEW_MAX_FILES))
						{
							printf("TIM file needed following -tim (max %d)\n", TIMVIEW_MAX_FILES);
							goto usage;
						}
						timview_files[timview_count++] = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if ((i + 1 >= argc) || (tim_count >= VIEWER_MAX_TIM))
					{
						printf("TIM file needed following -t (max %d)\n", VIEWER_MAX_TIM);
						goto usage;
					}
					tims[tim_count++] = argv[i + 1];
					i++;
				break;

				case 'y':	// Net Yaroze library (libps.exe) to upload before executing
					if (i + 1 >= argc)
					{
						printf("libps.exe file needed following -y\n");
						goto usage;
					}
					libps = argv[i + 1];
					i++;
				break;

				case 'f':	// file
					if (!strcmp(argv[i], "-frame"))	// Frame capture (debug session, commands on the input, gdbserver.c)
					{
						frame_live = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if (!strcmp(argv[i], "-flash"))	// Flash a cartridge ROM (with ar_flash.ps-exe, readback check)
					{
						if (i + 1 >= argc)
						{
							printf("ROM file needed following -flash\n");
							goto usage;
						}
						flash_rom = argv[++i];
						cmd = CMD_FLASH_ROM;
						break;
					}
					filename = argv[i + 1];
					i++;
				break;

				case 'g':	// GDB server: -gdb [port], the program is debugged with GDB (or VS Code), see gdbserver.c
					if (!strcmp(argv[i], "-gdb"))
					{
						gdb_port = 3333;
						if ((i + 1 < argc) && isdigit((unsigned char)argv[i + 1][0]))
							gdb_port = atoi(argv[++i]);
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
					}
				break;

				case 'p':	// PCDRV path
					if (!strcmp(argv[i], "-pad"))	// Remote pad: a port driven from the computer (-memlive, -frame)
					{
						remote_pad = true;
						break;
					}
					if (!strcmp(argv[i], "-padtest"))	// Control tester (padtest.ps-exe next to ps1transfer), remote pad on
					{
						pad_test = remote_pad = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if (!strcmp(argv[i], "-prof"))	// Profiler of the running program (profiler.c)
					{
						prof_on = true;
						if ((i + 1 < argc) && isdigit((unsigned char)argv[i + 1][0]))
						{
							prof.rate = atoi(argv[++i]);
							if (strchr(argv[i], ','))
								prof.counter = atoi(strchr(argv[i], ',') + 1);
						}
						if (cmd == CMD_UNKNOWN)		// (with -e: after the program is executed)
							cmd = CMD_PROF;
						break;
					}
					if (!strcmp(argv[i], "-pcdrvlog"))	// File server activity lines (GUI)
					{
						pcdrv_log = true;
						break;
					}
					if (!strcmp(argv[i], "-pcdrvany"))	// File server: any path of the computer
					{
						pcdrv_any_path = true;
						break;
					}
					if (!strcmp(argv[i], "-progress"))	// Machine readable progress lines (GUI)
					{
						machine_progress = true;
						break;
					}
					if (!strcmp(argv[i], "-peek") || !strcmp(argv[i], "-poke"))	// Memory dump / write (address or @symbol)
					{
						cmd = strcmp(argv[i], "-peek") ? CMD_POKE : CMD_PEEK;
						if ((i + 1 >= argc) || ((cmd == CMD_POKE) && (i + 2 >= argc)))
						{
							printf("%s needs an address%s\n", argv[i], (cmd == CMD_POKE) ? " and a value" : "");
							goto usage;
						}
						adrs_str = argv[++i];
						if (cmd == CMD_POKE)
							poke_value = strtoul(argv[++i], NULL, 0);
						break;
					}
					path = argv[i + 1];
					i++;
				break;

				case 'a':	// 0xAddress (hexadecimal address for download/upload) or @symbol[+offset]
					if (!strcmp(argv[i], "-auto"))	// Net Yaroze auto script: the program and its data files
					{
						if (i + 1 >= argc)
						{
							printf("Auto script needed following -auto\n");
							goto usage;
						}
						auto_script = argv[++i];
						cmd = CMD_EXECUTE;
						break;
					}
					adrs_str = argv[i + 1];
					i++;
				break;

				case 'v':	// VRAM x y w h
					if ((i + 1 < argc) && !strcmp(argv[i + 1], "auto"))	// The screen on the TV (PS1 DEV CART tools)
					{
						vram_auto = true;
						x = y = 0;
						w = 320;
						h = 240;
						i++;
						cmd = CMD_VRAM;
						break;
					}
					if (i + 4 < argc)
					{
						x = atoi(argv[i + 1]);
						y = atoi(argv[i + 2]);
						w = atoi(argv[i + 3]);
						h = atoi(argv[i + 4]);
						i += 4;
						cmd = CMD_VRAM;

						if ((x < 0) || ((x+w) > 1024) || (y < 0) || ((y+h) > 512))
						{
							printf("VRAM coordinates must be within 1024x512 !\n");
							goto usage;
						}
					}
					else
					{
						printf("VRAM command needs x y w h parameters following -v\n");
						goto usage;
					}
				break;

				case 's':	// size (size of download in decimal)
					if (!strcmp(argv[i], "-spu"))	// SPU RAM and registers of the running program to a file
					{
						if (i + 1 >= argc)
						{
							printf("-spu needs the file to write\n");
							goto usage;
						}
						spu_file = argv[++i];
						cmd = CMD_SPU;
						break;
					}
					if (!strcmp(argv[i], "-synth"))	// SPU synth bridge, commands on the standard input (synthbridge.c)
					{
						cmd = CMD_SYNTH;
						break;
					}
					if (!strcmp(argv[i], "-sym"))	// Symbol file (.map, .sym/nm output, .elf) for -crash, -peek, @names
					{
						if (i + 1 >= argc)
						{
							printf("Symbol file needed following -sym\n");
							goto usage;
						}
						symfile = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-slot"))	// Memory card slot (1 or 2) for -mcbackup/-mcrestore
					{
						if ((i + 1 >= argc) || ((card_slot = atoi(argv[i + 1])) < 1) || (card_slot > 2))
						{
							printf("Slot 1 or 2 needed following -slot\n");
							goto usage;
						}
						i++;
						break;
					}
					if (!strcmp(argv[i], "-sndlive"))	// Sound tester: commands of the GUI on the input (sndlive.c)
					{
						snd_live = true;
						break;
					}
					if (!strcmp(argv[i], "-snd"))	// Sound file for the sound tester (VH, VB, VAB, SEQ, VAG)
					{
						if ((i + 1 >= argc) || (sound_count >= TESTER_MAX_FILES))
						{
							printf("Sound file needed following -snd (max %d)\n", TESTER_MAX_FILES);
							goto usage;
						}
						sound_files[sound_count++] = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					size = atoi(argv[i + 1]);
					if (size <= 0)
					{
						printf("Wrong size '%s'\n", argv[i + 1]);
						goto usage;
					}
					i++;
				break;
				
				case 'z':	// set device id (only relevant when using multiple PS1 DEV CART devices)
					if (i + 1 >= argc)
					{
						printf("Device id needs a number following -z\n");
						goto usage;
					}
					deviceid = atoi(argv[i + 1]);
					i++;
				break;
			}
		}
		else
			goto usage;
	}
	
#ifdef  _WIN32
	if (list)
	{
		USB_ListDevices();
		return (0);
	}

	if (!USB_OpenDevice(deviceid, serial))
	{
		fprintf(stderr, "Unable to open USB device.\n");
		return EXIT_FAILURE;
	}
#else
	if ((ftdi = ftdi_new()) == 0)
	{
		fprintf(stderr, "ftdi_new failed\n");
		return EXIT_FAILURE;
	}

	if (list)
	{
		USB_ListDevices();
		ftdi_free(ftdi);
		return (0);
	}

	if (!USB_OpenDevice(deviceid, serial))
	{
		ftdi_free(ftdi);
		return EXIT_FAILURE;
	}
#endif

	ftdi_tcioflush(ftdi);
#ifdef _WIN32
	// Old data (e.g. printf text of a program after a "ps1transfer -c" was stopped) would be read as the answer to a command
	FT_Purge(ftdi, FT_PURGE_RX | FT_PURGE_TX);
#endif
	USB_SetLatency();	// After the reset/flush, which may restore the default value
	
	// The model viewer is executed with -m, unless another viewer is given with -f
	if (model && !filename)
		filename = GetToolPath(argv[0], VIEWER_FILENAME);
	// The sound tester is executed with -snd, unless another tester is given with -f
	if (sound_count && !filename)
		filename = GetToolPath(argv[0], TESTER_FILENAME);
	// The TIM viewer is executed with -tim, unless another viewer is given with -f
	if (timview_count && !filename)
		filename = GetToolPath(argv[0], TIMVIEW_FILENAME);
	// With -c, the palette editor saves/reloads the TIM files with the file server: directory of the first one (or -p)
	if (timview_count && ttymode && !path)
	{
		char	*sep, *sep2;

		path = strdup(timview_files[0]);
		sep = strrchr(path, '/');
		sep2 = strrchr(path, '\\');
		if (sep2 > sep)
			sep = sep2;
		if (sep)
			*sep = 0;
		else
			path = NULL;
	}
	// The memory card tool is executed with -mcbackup/-mcrestore, the backup file is written by the file server
	// in the directory of the given file
	// The link cable tester is executed with -link (on both consoles), unless another program is given with -f
	if (link_test && !filename)
		filename = GetToolPath(argv[0], LINKTEST_FILENAME);
	// The control tester is executed with -padtest (the remote pad on: the Remote Controller page drives it)
	if (pad_test && !filename)
		filename = GetToolPath(argv[0], PADTEST_FILENAME);
	if (card_mode && !filename)
	{
		char	*sep, *sep2;

		filename = GetToolPath(argv[0], CARD_FILENAME);
		path = strdup(card_file);
		sep = strrchr(path, '/');
		sep2 = strrchr(path, '\\');
		if (sep2 > sep)
			sep = sep2;
		if (sep)
			*sep = 0;
		else
			path = NULL;
	}

	// Address given as @symbol: resolved after all arguments (-sym can be anywhere)
	if (adrs_str && !ParseAddress(adrs_str, &adrs, symfile))
		return EXIT_FAILURE;

	// -e with -net: printf kept on the BIOS device unless -c says otherwise (text from the PS1 in the USB FIFO would
	// be read by the bridge as the answers of the kernel hook)
	if (net_host && !tty_given)
		tty_mode = EXEC_TTY_MODE_BIOS;

	// Net Yaroze auto script: its "load" program is run, its "dload" files are uploaded first (UploadAutoData)
	if (auto_script)
	{
		if (!ReadAutoScript(auto_script, 0))
			return EXIT_FAILURE;
		if (!auto_program[0])
		{
			printf("%s: no program to run (\"local load main\" line)\n", auto_script);
			return EXIT_FAILURE;
		}
		if (!libps)
		{
			printf("A Net Yaroze program needs the Net Yaroze library: add -y libps.exe (from the Net Yaroze CD)\n");
			return EXIT_FAILURE;
		}
		filename = auto_program;
		printf("Auto script %s: %s and %d data file(s)\n", auto_script, auto_program, auto_count);
	}

	if (!filename && !reset && (cmd != CMD_CRASH) && (cmd != CMD_PEEK) && (cmd != CMD_POKE) && (cmd != CMD_HOT) &&
		(cmd != CMD_MEMLIVE) &&
		(cmd != CMD_INFO) && (cmd != CMD_FLASH_ROM) && (cmd != CMD_SYNTH) &&
		(cmd != CMD_CARD_LIVE) && (cmd != CMD_RELAY) && (cmd != CMD_NET) && (cmd != CMD_PROF) && (cmd != CMD_SPU) &&
		(cmd != CMD_BOOT_DISC))
	{
		printf("No file specified.\n");
		goto usage;
	}

	if ((cmd == CMD_UPLOAD) || (cmd == CMD_EXECUTE) || (cmd == CMD_FLASH))
	{
		data = SystemGetFile(filename, &size);
		if (!data)
			goto usage;
		if (cmd == CMD_EXECUTE)		// Net Yaroze ECOFF executable: run as a PS-X EXE
		{
			int		exe_size;
			uint8_t	*exe = EcoffToPsxExe(data, size, &exe_size);

			if (exe)
			{
				free(data);
				data = exe;
				size = exe_size;
				if (!libps)
					printf("Note: a Net Yaroze program needs the Net Yaroze library: -y libps.exe\n");
			}
			else if ((size < 2048) || memcmp(data, "PS-X EXE", 8))
			{
				printf("'%s' is not a PS-X EXE or a Net Yaroze (ECOFF) executable\n", filename);
				goto failed;
			}
		}
	}

	// -gdb: reset only if a program runs (the menu firmware may be a test build running from RAM)
	if ((cmd == CMD_FLASH_ROM) && !(flash_data = LoadRom(flash_rom)))
		goto failed;
	if (gdb_port || frame_live || (cmd == CMD_FLASH_ROM) || (cmd == CMD_BOOT_DISC))
	{
		ps1_program_running = true;		// Kept if it can't be read (program with an older kernel hook)
		USB_DetectPacketSize();
		reset = ps1_program_running;
	}
	// -synth: kept if it already runs (unless -r), otherwise started (after a reset if another program runs)
	if (cmd == CMD_SYNTH)
	{
		ps1_program_running = true;
		USB_DetectPacketSize();
		if (!reset)
			synth_start = !ps1_program_running || !SynthRunning();
		reset = synth_start && ps1_program_running;
	}
	// Memory card on a slot: the card tool server is kept if it already runs, otherwise started
	if (cmd == CMD_CARD_LIVE)
	{
		ps1_program_running = true;
		USB_DetectPacketSize();
		if (!reset)
			card_start = !ps1_program_running || !CardServerRunning();
		reset = card_start && ps1_program_running;
	}

	if (reset)
	{
		// Bug on Linux only (tested on Ubuntu 22.04 VM): sometimes an USB communication issue appears,
		// ps1transfer displays "USB error." then exits when trying to execute a ps-exe file just after
		// resetting (-r and -e options combined)
		// On Windows, it works fine, the data upload seems to wait that USB becomes ready after reset

		// The PS1 can be in the middle of a transfer for a ps1transfer that was stopped (e.g. the console of a program,
		// ended by an IDE): it goes on sending, and takes the bytes of a command as the acknowledgments of its packets
		// (each one: the packet sent again). Its data read until it is quiet (it has given up waiting for an
		// acknowledgment, about 1-2 seconds), then the reset again.
		printf("Resetting the PS1...\n");
		for (i = 0; !USB_Process('R', NULL, 0, 0); i++)
		{
			uint8_t	c;
			int		quiet = 0;
			time_t	start = time(NULL);

			if (i == 3)
				goto USB_error;
			printf("The PS1 didn't take the reset: waiting until it is quiet (a transfer stopped?)...\n");
			while ((quiet < RESET_QUIET_MS) && (time(NULL) - start < RESET_DRAIN_S))
			{
				if (USB_ReadByteNoWait(&c))
					quiet = 0;
				else
				{
					SleepMs(5);
					quiet += 5;
				}
			}
		}

#ifndef _WIN32
		// Closing and opening again the USB device seems to help but not in every case
		if ((ret = ftdi_usb_close(ftdi)) < 0)
		{
			fprintf(stderr, "Unable to close USB device: %d (%s)\n", ret, ftdi_get_error_string(ftdi));
			ftdi_free(ftdi);
			return EXIT_FAILURE;
		}

		if (!USB_OpenDevice(deviceid, serial))
		{
			ftdi_free(ftdi);
			return EXIT_FAILURE;
		}
		USB_SetLatency();

		// Wait some time for the USB communication to be ready again before executing a ps-exe file
		// Otherwise, next transfers fail
		if (cmd != CMD_UNKNOWN)
			sleep_ms(1000);
#endif
	}

	// Check if the firmware supports big packets (faster transfers)
	if (cmd != CMD_UNKNOWN)
		USB_DetectPacketSize();
	// Device ID shown on the menu (PS1 DEV CART firmware)
	if ((cmd != CMD_UNKNOWN) && !ps1_program_running && (fw_features & FEATURE_DEVICE_ID))
		USB_SendDeviceId();

	// Handle command
	ptr = data;
	switch (cmd)
	{
		case CMD_EXECUTE:
			ptr32 = (uint32_t*)&data[8+4+4+(2*4)];
			adrs = *ptr32;
			ptr = &data[2048];
			size -= 2048;
			if (libps && !UploadNetYarozeLibrary(libps, adrs, size))
				goto USB_error;
			if (auto_count && !UploadAutoData(adrs, size, libps_adrs, libps_size))	// Prints its own error
				goto failed;
			// The profiler's RAM fill (free RAM above the program) would overwrite data files loaded above it
			for (i = 0; prof_on && prof_fill && (i < auto_count); i++)
				if (auto_data[i].adrs + auto_data[i].size > adrs + size)
				{
					printf("Profiler: %s is above the program, the free RAM isn't filled (RAM use not measured)\n",
						auto_data[i].path);
					prof_fill = false;
				}
			if (model && !UploadModelData(model, tims, tim_count, tod_files, tod_count))	// Prints its own file or USB error
				goto failed;
			if (sound_count && !UploadSoundData(sound_files, sound_count))
				goto failed;
			if (timview_count && !UploadTimData(timview_files, timview_count))
				goto failed;
			if (card_mode && !UploadCardData(card_mode, card_slot, card_file))
			{
				ttymode = false;
				goto failed;
			}
			if (gdb_port || frame_live)
			{
				// The firmware must have the debug monitor (its kernel hook state holds a magic number)
				uint32_t	magic = 0;

				if (!USB_Process('D', (uint8_t *)&magic, MONITOR_STATE_ADRS, 4))
					goto USB_error;
				if (magic != MONITOR_MAGIC)
				{
					printf("-gdb needs the PS1 DEV CART firmware with the debug monitor in the cartridge.\n");
					goto failed;
				}
				// Program stopped at its entry point: break instruction there, put back by the GDB server
				gdb_entry = *(uint32_t *)&data[0x10];
				if ((gdb_entry < adrs) || (gdb_entry + 4 > adrs + size) || (gdb_entry & 3))
				{
					printf("Entry point 0x%08X outside of the program.\n", gdb_entry);
					goto failed;
				}
				memcpy(&gdb_entry_insn, ptr + gdb_entry - adrs, 4);
				*(uint32_t *)(ptr + gdb_entry - adrs) = 0x0005000D;	// break 5
				tty_mode |= EXEC_DEBUG;
			}
			printf("Uploading %u bytes to 0x%X...\n", size, adrs);
			start_ms = NowMs();
			if (USB_Process('U', ptr, adrs, size))
			{
				PrintUploadTime(size, start_ms);
				// Net Yaroze: the library start code runs first (stub), like with the boot disc
				if (libps && libps_init && !gdb_port && !frame_live)
				{
					uint32_t	stub = UploadNetYarozeStart(adrs, *(uint32_t *)&data[16]);

					if (!stub)
						goto USB_error;
					*(uint32_t *)&data[16] = stub;
				}
				printf("Executing...\n");

				// The TTY redirect choice is sent with the Execute command so it is known before the program starts
				printf("TTY: %s\n", ((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_USB) ? "redirected to USB" :
					(((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_SIO) ? "serial port driver" :
					(((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_LINK) ? "USB link driver (-relay)" : "BIOS device kept")));
				if (prof_on && prof_fill)
					printf("Profiler: free RAM filled to measure what the program uses\n");
				if (remote_pad)
				{
					MonitorState	ms;

					if (USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)) && (ms.magic == MONITOR_MAGIC) &&
						(ms.version >= 8))
						printf("Remote pad: controller port 1 can be driven from the computer%s\n",
							(ms.version >= 9) ? " (port 2 too: padport 1)" : "");
					else
					{
						printf("Remote pad: the firmware in the cartridge is too old (flash the new one)\n");
						remote_pad = false;
					}
				}
				if (!USB_Process('E', &data[8 + 4 + 4], tty_mode | ((prof_on && prof_fill) ? EXEC_PROF_FILL : 0) |
					(remote_pad ? EXEC_PAD : 0), 15 * 4))
					goto USB_error;
				// Initial stack pointer (header s_addr + s_size, 0: the one of the firmware, unknown)
				if (*(uint32_t *)&data[0x30])
					prof.stack_top = *(uint32_t *)&data[0x30] + *(uint32_t *)&data[0x34];
				// Your executable (not a PS1 DEV CART tool): its symbol file is used automatically by -crash, -peek, @names
				if (!model && !sound_count && !timview_count && !card_mode && !link_test)
					SaveLastExe(filename);
				if (prof_on)	// Profiler of the program just started
				{
					prof.symfile = symfile;
					if (UsbProfile(&prof))
						goto failed;
					goto done;
				}
				if (net_host)	// The program's USB link connected to the server
				{
					if (UsbNet(serial ? serial : "PS1", net_host))
						goto failed;
					goto done;
				}
				if (gdb_port || frame_live)
				{
					// The program runs: only the debug monitor/kernel hook answers (2KB packets)
					ps1_program_running = true;
					usb_big_packet_size = 0;
					if (path)
						chdir(path);	// PCDRV files of the program
					if (frame_live)
						exit_code = FrameLive(gdb_entry, gdb_entry_insn);
					else
						GdbServer(gdb_port, gdb_entry, gdb_entry_insn, relay_serial);
					goto done;
				}
			}
			else
				goto USB_error;
		break;

		case CMD_PEEK:	// Hexadecimal dump with symbol names (live while a program runs with the PS1 DEV CART firmware)
		{
			uint8_t		buf[4096];
			uint32_t	j, k;

			if (!size || (size > sizeof(buf)))
				size = (size > sizeof(buf)) ? sizeof(buf) : 4;
			if (!USB_Process('D', buf, adrs, size))
				goto USB_error;
			AutoLoadSymbols(symfile);
			for (j = 0; j < size; j += 16)
			{
				Symbol	*s = FindSymbol(adrs + j);

				printf("%08X ", adrs + j);
				for (k = j; k < j + 16; k++)
					printf((k < size) ? " %02X" : "   ", buf[k]);
				printf("  ");
				for (k = j; (k < j + 16) && (k < size); k++)
					putchar(((buf[k] >= 32) && (buf[k] < 127)) ? buf[k] : '.');
				if (s)
					printf("  %s+0x%X", s->name, adrs + j - s->adrs);
				printf("\n");
			}
			if ((size == 1) || (size == 2) || (size == 4))
			{
				uint32_t	v = buf[0] | ((size > 1) ? buf[1] << 8 : 0) | ((size > 2) ? (buf[2] << 16) | ((uint32_t)buf[3] << 24) : 0);

				printf("Value: %u (0x%X), signed %d\n", v, v, (size == 1) ? (int8_t)v : ((size == 2) ? (int16_t)v : (int32_t)v));
			}
		}
		break;

		case CMD_MEMLIVE:	// Memory read/written on request while the program runs (the GUI's watch list)
			if (path)
				chdir(path);	// (its file server: PCDRV files of the program)
			if (MemLive())
				goto failed;
		break;

		case CMD_POKE:	// Write a 1/2/4 bytes little endian value
		{
			uint8_t	buf[4] = {poke_value, poke_value >> 8, poke_value >> 16, poke_value >> 24};

			if ((size != 1) && (size != 2))
				size = 4;
			printf("Writing %u (0x%X), %u byte(s) at 0x%08X\n", poke_value, poke_value, size, adrs);
			if (!USB_Process('U', buf, adrs, size))
				goto USB_error;
		}
		break;

		case CMD_UPLOAD:
		case CMD_FLASH:
			printf("Uploading %u bytes to 0x%X...\n", size, adrs);
			start_ms = NowMs();
			if (USB_Process('U', ptr, adrs, size))
			{
				PrintUploadTime(size, start_ms);
				if (cmd == CMD_FLASH)
				{
					printf("Flashing Action Replay...\n");
					if (!USB_Process('F', NULL, adrs, (size+127)/128))
						goto USB_error;
				}
			}
			else
				goto USB_error;
		break;

		case CMD_DOWNLOAD:
			data = malloc(size);
			if (!data)
			{
				printf("Memory Allocation Error to download data of size %d\n", size);
				goto usage;
			}
			ptr = data;
			printf("Downloading %u bytes from 0x%X...\n", size, adrs);
			if (USB_Process('D', ptr, adrs, size))
			{
				FILE	*f;

				f = fopen(filename, "wb");
				fwrite(data, 1, size, f);
				fclose(f);
			}
			else
				goto USB_error;
		break;

		case CMD_CRASH:
			if (!CrashReport(symfile, filename))
				goto USB_error;
		break;

		case CMD_HOT:
			if (!HotReload(hot_files, hot_count, hot_watch))
				goto failed;
		break;

		case CMD_INFO:
			PrintInfo();
		break;

		case CMD_BOOT_DISC:
			// After a reset (a program was running): the menu answers a few seconds later
			for (i = 0; reset && !(fw_features & FEATURE_BOOT_DISC) && (i < 10); i++)
			{
				SleepMs(1000);
				USB_DetectPacketSize();
			}
			if (!(fw_features & FEATURE_BOOT_DISC))
			{
				printf("The firmware of this cartridge can't boot the disc: flash the latest ROM.\n");
				goto failed;
			}
			if (!USB_Process('C', NULL, 0, 0))
				goto USB_error;
			printf("The PS1 boots the disc (messages on the TV).\n");
		break;

		case CMD_FLASH_ROM:
			if (!FlashRom(flash_rom, flash_data, argv[0]))
				goto failed;
		break;

		case CMD_SYNTH:
			if (SynthBridge(argv[0], synth_start))
				goto failed;
		break;

		case CMD_CARD_LIVE:
			if (CardLiveCommand(argc, argv, card_live_arg, argv[0], card_start) != EXIT_SUCCESS)
				goto failed;
		break;

		case CMD_RELAY:
			if (!ps1_program_running)
			{
				printf("Start the link tester on both consoles first (-link usb).\n");
				goto failed;
			}
			if (UsbRelay(serial ? serial : "first cartridge", relay_serial))
				goto failed;
		break;

		case CMD_PROF:
			if (!ps1_program_running)
			{
				printf("Start a program first (or add -e -f program.ps-exe).\n");
				goto failed;
			}
			prof.symfile = symfile;
			if (UsbProfile(&prof))
				goto failed;
		break;

		case CMD_NET:
			if (!ps1_program_running)
			{
				printf("Start a program using the USB link first (or add -e -f program.ps-exe).\n");
				goto failed;
			}
			if (UsbNet(serial ? serial : "PS1", net_host))
				goto failed;
		break;

		case CMD_SPU:
		{
			// The debug monitor reads them (DMA) through its 'M' service while the program runs, or directly when the
			// program is stopped in it. File: the 512KB of SPU RAM, then the 512 bytes of SPU registers.
			MonitorState	ms;
			uint8_t			*spu = malloc(MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE);
			bool			stopped, ok;
			FILE			*f;

			if (!spu)
				goto failed;
			if (!ps1_program_running)
			{
				printf("No program runs: the SPU RAM is read while a program runs (by the debug monitor)\n");
				goto failed;
			}
			if (!USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, 16))
				goto USB_error;
			if ((ms.magic != MONITOR_MAGIC) || (ms.version < 10))
			{
				printf("The firmware in the cartridge is too old to read the SPU RAM (debug monitor version 10): "
					"flash the new one\n");
				goto failed;
			}
			stopped = (ms.reason != MON_REASON_NONE);
			printf("Reading the SPU registers and RAM (512KB)...\n");
			ok = stopped ? USB_Process(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE) :
				HotService(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE);
			ok = ok && (stopped ? USB_Process(MON_CMD_SPU_READ, spu, 0, MON_SPU_RAM_SIZE) :
				HotService(MON_CMD_SPU_READ, spu, 0, MON_SPU_RAM_SIZE));
			if (!ok)
				goto USB_error;
			if (!(f = fopen(spu_file, "wb")) || (fwrite(spu, 1, MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE, f) !=
				MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE))
			{
				printf("Can't write %s\n", spu_file);
				if (f)
					fclose(f);
				goto failed;
			}
			fclose(f);
			printf("SPU RAM and registers written to %s\n", spu_file);
			free(spu);
		}
		break;

		case CMD_VRAM:
			data = malloc(w*h*(sizeof(uint16_t)+3));
			if (!data)
			{
				printf("Memory Allocation Error to download VRAM image\n");
				goto usage;
			}
			ptr = data;
			if ((ps1_program_running || vram_live) && (fw_features & FEATURE_LIVE_ACCESS))
			{
				// Live screenshot of the running program, read from the GPU by the kernel hook, or by the debug monitor
				// (monitor version 2 or later, through its 'M' service, unless the program is stopped in the monitor).
				// At the menu (-live), the firmware reads it ('V')
				MonitorState	ms;
				bool			service = ps1_program_running && USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, 16) &&
										(ms.magic == MONITOR_MAGIC) && (ms.version >= 2) && (ms.reason == MON_REASON_NONE);
				bool			held = false, ok;

				// -v auto: the screen on the TV as the program tells it (the TIM viewer: held on a complete frame)
				if (vram_auto)
				{
					if (ps1_program_running && (held = TimViewScreen(&x, &y)))
						printf("Screen on the TV (TIM viewer): %d,%d\n", x, y);
					else
						printf("The screen on the TV isn't known for this program: 0,0 (320x240)\n");
				}
				printf("Downloading live VRAM from coordinate %d,%d of size %d,%d...\n", x, y, w, h);
				ok = service ? HotService('V', ptr, (x << 16) | y, (w << 16) | h) :
					USB_Process('V', ptr, (x << 16) | y, (w << 16) | h);
				if (held)
					TimViewRelease();
				if (!ok)
					goto USB_error;
				ptr += w * h * sizeof(uint16_t);
			}
			else
			{
				printf("Downloading VRAM saved at boot from coordinate %d,%d of size %d,%d...\n", x, y, w, h);
				// VRAM is saved at boot by PS1 DEV CART to RAM address 0x80020000 (e.g. last screen of a program after a reset)
				for (i = 0; i < h; i++)
				{
					printf("Downloading line %d of %d...\n", i+1, h);
					adrs = 0x80020000 + ((((y + i) * 1024) + x) * sizeof(uint16_t));
					size = w * sizeof(uint16_t);
					if (USB_Process('D', ptr, adrs, size))
						ptr += size;
					else
						goto USB_error;
				}
			}
			printf("Saving to PNG image: %s\n", filename);
			VramToPng(filename, (uint16_t *)data, w, h);
		break;

		default:
			if (!reset)
				printf("No command specified.\n");
	}

	goto done;

USB_error:
	printf("\nUSB error.\n");
failed:		// Non-zero exit code on errors
	exit_code = EXIT_FAILURE;
	goto done;

usage:
	exit_code = EXIT_FAILURE;
	if (ttymode)
		goto done;

	printf("\nUsage: ps1transfer parameters...\n");
	printf("\t-u Upload file to specified address\n");
	printf("\t-d Download file from specified address and size\n");
	printf("\t-v Download VRAM image from following parameters: x y w h\n");
	printf("\t-spu file Write the SPU RAM (512KB) then the SPU registers (512 bytes, 0x1F801C00) of the running program to file (SPU RAM Viewer)\n");
	printf("\t-live With -v at the menu: the screen as it is now (default: the VRAM saved at boot)\n");
	printf("\t   (live screenshot if a program runs, otherwise the VRAM saved at the last boot)\n");
	printf("\t-d/-u/-v also work while a program runs (memory peek/poke, live screenshots) with the PS1 DEV CART firmware\n");
	printf("\t-crash [game.map] Crash report while the crash screen is displayed: cause, registers, call stack\n");
	printf("\t   with function names from a .map, .sym (nm output) or .elf file (PS1 DEV CART firmware)\n");
	printf("\t   -f frame.png: also saves the last frame the program displayed (double buffered programs)\n");
	printf("\t-sym file Symbol file (by default the .map/.sym/.elf next to the last executable run with -e)\n");
	printf("\t-peek address [-s size] Hexadecimal dump (address in hexadecimal or @symbol[+offset])\n");
	printf("\t-poke address value [-s 1|2|4] Write a value (e.g. -poke @lives 3 -s 1)\n");
	printf("\t-e Execute psexe file (use -r if a program is already running), or a Net Yaroze ECOFF executable (with -y)\n");
	printf("\t-r Reset the PS1 (will be done first if combined with actions above)\n");
	printf("\t-f file.bin\n");
	printf("\t-a 0xAddress (hexadecimal address for download/upload)\n");
	printf("\t-s size (size of download in decimal)\n");
	printf("\t-c Switch to TTY console mode after executing command.\n");
	printf("\t-c off (or -coff) Keep the PS1 BIOS TTY device, printf/TTY is not redirected to USB\n");
	printf("\t-c sio (or -csio) Install a serial port TTY driver like the Net Yaroze boot disc (e.g. link play)\n");
	printf("\t-c link (or -clink) The same TTY driver on the USB link instead of the serial port: link play without\n");
	printf("\t   cable, through the cartridges and this computer (run the program on both consoles, then -relay)\n");
	printf("\t-p path (indicate path to PCDRV file server, require -c)\n");
	printf("\t   The program can only open files in that folder (and below); -pcdrvany allows any path\n");
	printf("\t-pcdrvlog File server activity as pcdrv=... lines (open, create, close with bytes read/written, errors)\n");
	printf("\t-z deviceid (only relevant when multiple PS1 DEV CART devices are connected via USB)\n");
	printf("\t-n serial (open the PS1 DEV CART device with this USB serial number, stable unlike -z)\n");
	printf("\t-l List connected USB devices with their index and serial number\n");
	printf("\t-flash rom.rom Flash a cartridge ROM (ar_flash.ps-exe next to ps1transfer), then read it back to check\n");
	printf("\t-bootcd Boot the disc in the drive (the CD drive unlocked first: burned and import discs, no modchip)\n");
	printf("\t   Same as START + SELECT on the menu. Not on Japanese consoles (licensed discs still boot).\n");
	printf("\t-info Cartridge state as key=value lines (device, state: menu/running/stopped/crashed, firmware...)\n");
	printf("\t-synth SPU synth (synth.ps-exe next to ps1transfer): 16 sample slots, slot n plays on MIDI channel n.\n");
	printf("\t   Commands on its input: midi HEX..., load SLOT file.vag, clear SLOT, set SLOT PARAM VALUE, state, reset, quit\n");
	printf("\t-progress Transfer progress as progress=done/total lines (for programs using ps1transfer)\n");
	printf("\t-y libps.exe (Net Yaroze library from the Net Yaroze CD-ROM, uploaded before -e)\n");
	printf("\t-auto auto -y libps.exe Net Yaroze auto script (siocons): its program and \"dload\" data files\n");
	printf("\t-m model.tmd (display a TMD model with the model viewer, tmdviewer.ps-exe next to ps1transfer)\n");
	printf("\t-t texture.tim (texture for -m, can be used up to %d times)\n", VIEWER_MAX_TIM);
	printf("\t-tod anim.tod (TOD animation for -m, up to %d: played, paused, stepped on the PS1)\n", VIEWER_MAX_TOD);
	printf("\t-mdllive With -m: commands on the input for the PS1 Dev Cart Manager (TOD, frame, play, view; state lines)\n");
	printf("\t-memlive Memory of the running program read/written on request (PS1 Dev Cart Manager's watch list), its printf and file server meanwhile\n");
	printf("\t-padtest Run the control tester (padtest.ps-exe next to ps1transfer) with -pad: the controllers of both ports as a program sees them, real or emulated (Remote Controller page)\n");
	printf("\t-pad With -e: a controller port can be driven from the computer (-memlive \"pad\"/\"padport\" commands), for programs using the BIOS pad functions; port 2 needs the debug monitor version 9\n");
	printf("\t-frame -f game.exe Frame capture (PS1 Dev Cart Manager): the program runs in a debug session, \"capture\" on the input reads the GPU packets of a frame\n");
	printf("\t-snd file (play VH+VB/VAB banks, SEQ music, VAG samples with the sound tester, up to %d files)\n", TESTER_MAX_FILES);
	printf("\t-sndlive With -snd: commands on the input for the PS1 Dev Cart Manager (program, note, volume, play, SEQ; state lines)\n");
	printf("\t-tim file.tim (TIM and VRAM viewer: VRAM map, overlaps, CLUTs, up to %d files)\n", TIMVIEW_MAX_FILES);
	printf("\t-timlive With -tim -c: commands on the input for the PS1 Dev Cart Manager (live palette, VRAM read)\n");
	printf("\t-mcbackup card.mcd (save the memory card to a 128KB image), -mcrestore card.mcd (write it back,\n");
	printf("\t   confirmation on the PS1), -slot 1|2 (default 1)\n");
	printf("\t-link [yaroze] Link cable tester (run it on both consoles, e.g. with -n serial): serial port used like\n");
	printf("\t   a PsyQ game (libcomb, \"sio:\"), or with \"yaroze\" like a Net Yaroze game (\"tty:\" driver, as -c sio)\n");
	printf("\t   \"usb\": no cable, the consoles are linked through their cartridges and this computer (-relay)\n");
	printf("\t-relay SERIAL2 USB link relay between the cartridge of -n and SERIAL2 (both run -link usb), until stopped\n");
	printf("\t-net host:port Connects the USB link of the running program (psxcomb USB link, or -c link) to a TCP\n");
	printf("\t   server, until stopped; with -e -f program.ps-exe: executes it first (printf kept on the BIOS device)\n");
	printf("\t-prof [rate[,counter]] Profiler of the running program (with -e -f program.ps-exe: started first, its free\n");
	printf("\t   RAM filled to measure what it uses, -nofill: not filled): CPU load and the functions where the time goes\n");
	printf("\t   (symbols: .map/.elf next to the program, or -sym), GPU busy, frames, DMA, stack, RAM, SPU voices, every\n");
	printf("\t   second until stopped. rate: samples a second (default 1000), counter: root counter used (default 0)\n");
	printf("\t   -json: \"PROF {...}\" lines, -csv file: one line per second in a CSV file, -idle f1,f2: more\n");
	printf("\t   functions where the program waits (not CPU load), names or addresses as shown\n");
	printf("\t-hot file[@where] Hot reload into the running program (several -hot): file.tim (VRAM at its position,\n");
	printf("\t   or file.tim@x,y), sound.vag@0x1010 / bank.vb@0x1010 (SPU RAM), data.bin@0x80123456 (RAM)\n");
	printf("\t-watch With -hot: send each file again when it changes, and show the program printf\n");
	printf("\t-gdb [port] -f game.exe Debug with GDB/VS Code: the program is loaded stopped at its entry point,\n");
	printf("\t   GDB server on localhost:port (default 3333), breakpoints, step, watchpoint, pause (PS1 DEV CART firmware)\n");
	printf("\tMemory card images (no PS1 needed, a .bak copy is kept when an image is modified):\n");
	printf("\t   -mclist card.mcd [-icons dir]   saves with titles (and icons as PNG)\n");
	printf("\t   -mcexport card.mcd SAVE out.mcs   -mcimport card.mcd in.mcs   -mcdelete card.mcd SAVE\n");
	printf("\t   slot1 or slot2 instead of card.mcd: on the card itself, only the sectors needed are read/written\n");
	printf("\t   (writes confirmed on the PS1), e.g. -mclist slot1, -mcexport slot1 SAVE out.mcs, -mccopy slot1 SAVE slot2\n");
	printf("\t   -mccopy src.mcd SAVE dst.mcd   -mcformat new.mcd   (SAVE: file name or block number)\n");
	printf("\nData are secured using CRC8.\n");

done:
	if (data)
		free(data);

	// Handle TTY Console mode
	memset(PCdrvFD, 0, sizeof(PCdrvFD));
	if (path)
		chdir(path);
	// -timlive: the TIM viewer just executed takes the GUI's commands (live palette, VRAM), the file server goes on
	if (tim_live && timview_count && ftdi && !exit_code)
	{
		exit_code = TimLive();
		ttymode = false;
	}
	// -mdllive: the model viewer just executed shares its animation and view controls with the GUI
	if (mdl_live && model && ftdi && !exit_code)
	{
		exit_code = MdlLive();
		ttymode = false;
	}
	// -sndlive: the sound tester just executed shares its controls with the GUI (program, note, volume, SEQ, voices)
	if (snd_live && sound_count && ftdi && !exit_code)
	{
		exit_code = SndLive();
		ttymode = false;
	}
	// -c alone (attach): the file server needs the packet size (a query can fail while the program prints)
	for (i = 0; ttymode && ftdi && !hook_packet_detected && (i < 3); i++)
		USB_DetectPacketSize();
	if (ttymode)
		setvbuf(stdout, NULL, _IONBF, 0);	// Show console text immediately, also when redirected to a file
	while (ttymode && ftdi)	// ftdi is NULL if arguments were wrong (device not opened yet)
	{
		char	c;

		if (ftdi_read_data(ftdi, &c, 1) == 1)
		{
			// Data Link Escape (PCDRV mode)
			if (c == 16)
				PC_FileServer();
			else if ((c == CARD_EOT) && exit_on_eot)	// End of a PS1 tool (memory card backup/restore)
				break;
			else
			{
				putchar(c);	// Just echo the received char
				console_last_char = c;
			}
		}
	}

	// Memory card backup: check the file written by the file server and list the saves
	if (card_mode == CARD_BACKUP)
	{
		uint8_t	*img;
		int		isize;
		char	*name = strrchr(card_file, '/') ? strrchr(card_file, '/') + 1 : card_file;

		name = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
		if (!access(name, 0) && (img = SystemGetFile(name, &isize)))	// Only if the PS1 tool has written it
		{
			if (isize == CARD_SIZE)
			{
				printf("\nBackup written: %s (%d bytes)\n", card_file, isize);
				PrintCardDirectory(img);
			}
			else
				printf("\nBackup file has a wrong size: %d bytes\n", isize);
			free(img);
		}
	}

	if (!ftdi)	// Device not opened (wrong arguments)
		return (exit_code);

#ifndef _WIN32
	if ((ret = ftdi_usb_close(ftdi)) < 0)
	{
		fprintf(stderr, "Unable to close USB device: %d (%s)\n", ret, ftdi_get_error_string(ftdi));
		ftdi_free(ftdi);
		return EXIT_FAILURE;
	}
#endif
    ftdi_free(ftdi);

	return (exit_code);
}

