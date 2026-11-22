/*
* PS1 DEV CART Transfer tool: the cartridge's USB connection: FTDI D2XX / libftdi / serial port, commands, packets, exchange (from main.c)
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
#include "crc.h"

/****************************************/

#ifdef _WIN32

UsbDevice	*ftdi;
const char	*com_name;		// -com: the port ("COM7") or the USB serial number of the device on it

uint32_t	ftdi_read_data(UsbDevice *dev, void *data, uint32_t size)
{
	DWORD	rbyte;

	if (dev->com)
		return (SerialRead(dev->com, (uint8_t *)data, size));
	if (FT_Read(dev->ft, data, size, &rbyte) == FT_OK)
		return (rbyte);
	return (0);
}

uint32_t	ftdi_write_data(UsbDevice *dev, uint8_t *data, uint32_t size)
{
	DWORD	rbyte;

	if (dev->com)
		return (SerialWrite(dev->com, data, size));
	if (FT_Write(dev->ft, data, size, &rbyte) == FT_OK)
		return (rbyte);
	return (0);
}

// Old data (e.g. printf text of a program after a "ps1transfer -c" was stopped) would be read as the answer to a command
void	USB_Purge(UsbDevice *dev)
{
	if (dev->com)
		SerialPurge(dev->com);
	else
		FT_Purge(dev->ft, FT_PURGE_RX | FT_PURGE_TX);
}

void	ftdi_tcioflush(UsbDevice *dev)
{
	if (dev->com)
		SerialPurge(dev->com);
	else
		FT_ResetDevice(dev->ft);
}

void	ftdi_free(UsbDevice *dev)
{
	if (!dev)
		return;
	if (dev->com)
		SerialClose(dev->com);
	else
		FT_Close(dev->ft);
	free(dev);
}

#else
struct ftdi_context *ftdi = NULL;

#endif

/****************************************/

/****************************************/
// USB device selection (by index or serial number, for multiple PS1 DEV CART devices)

#ifdef _WIN32
// The PS1 answers each 2KB packet with a single CRC byte, which the FTDI chip only sends to the computer
// when its latency timer expires, so a short latency is what makes transfers fast
void	USB_SetLatency(void)
{
	if (ftdi->com)	// (an FTDI chip's latency timer is the VCP driver's setting, see serialport.c)
	{
		SerialSetTimeouts(ftdi->com, USB_READ_POLL_MS, USB_READ_TIMEOUT * 1000);
		return;
	}
	FT_SetLatencyTimer(ftdi->ft, USB_LATENCY_MS);
	FT_SetTimeouts(ftdi->ft, USB_READ_POLL_MS, USB_READ_TIMEOUT * 1000);	// Default is no timeout: a missing answer froze ps1transfer
}

// -com NAME: that serial port. Else the FTDI chip (by serial number or index), or the serial port of a USB device with
// this serial number (not an FTDI chip, e.g. a Raspberry Pi Pico)
bool	USB_OpenDevice(int deviceid, char *serial)
{
	UsbDevice	*dev = calloc(1, sizeof(UsbDevice));
	const char	*name = com_name ? com_name : serial;
	bool		ok = false;

	if (!dev)
		return (false);
	if (!com_name)
		ok = serial ? (FT_OpenEx(serial, FT_OPEN_BY_SERIAL_NUMBER, &dev->ft) == FT_OK) :
			(FT_Open(deviceid, &dev->ft) == FT_OK);
	if (!ok && name)
	{
		HANDLE	h = SerialOpen(name, NULL, dev->serial);

		if (h != INVALID_HANDLE_VALUE)
		{
			dev->com = h;
			ok = true;
		}
	}
	if (!ok)
	{
		free(dev);
		return (false);
	}
	ftdi = dev;
	return (true);
}

// The FTDI chips (D2XX), then the serial ports of the other USB devices
void	USB_ListDevices(void)
{
	DWORD		i, n = 0, flags, type, id, loc;
	char		serial[16], desc[64];
	FT_HANDLE	handle;

	if (FT_CreateDeviceInfoList(&n) != FT_OK)
		n = 0;
	for (i = 0; i < n; i++)
	{
		if (FT_GetDeviceInfoDetail(i, &flags, &type, &id, &loc, serial, desc, &handle) == FT_OK)
			printf("-z %lu  -n %-12s %s%s\n", i, serial, desc, (flags & FT_FLAGS_OPENED) ? " (in use)" : "");
	}
	if (!SerialList(true) && !n)
		printf("No USB device found.\n");
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
	const char	*com = com_name;

	com_name = NULL;	// (the other cartridge: by its own serial number)
	if (USB_OpenDevice(0, serial))
	{
		USB_Purge(ftdi);
		USB_SetLatency();
		handle = (void *)ftdi;
	}
	com_name = com;
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
	ftdi_free((UsbDevice *)handle);
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

// USB serial number of the opened cartridge (at least 64 bytes)
bool	USB_GetSerial(char *serial)
{
	memset(serial, 0, 64);
#ifdef _WIN32
	{
		FT_DEVICE	type;
		DWORD		id;
		char		desc[64];

		if (ftdi->com)
			memcpy(serial, ftdi->serial, 64);
		else if (FT_GetDeviceInfo(ftdi->ft, &type, &id, serial, desc, NULL) != FT_OK)
			return (false);
	}
#else
	if (ftdi_usb_get_strings2(ftdi, libusb_get_device(ftdi->usb_dev), NULL, 0, NULL, 0, serial, 64) < 0)
		return (false);
#endif
	return (serial[0] != 0);
}

// The USB chip of the cartridge and its driver, as text (bus timing test report)
void	USB_ChipText(char *out, int size)
{
#ifdef _WIN32
	FT_DEVICE	type;
	DWORD		id, drv = 0, lib = 0;
	char		serial[64], desc[64];

	if (ftdi->com)
	{
		snprintf(out, size, "serial port %s (USB serial number \"%s\")", com_name ? com_name : "?", ftdi->serial);
		return;
	}
	if (FT_GetDeviceInfo(ftdi->ft, &type, &id, serial, desc, NULL) != FT_OK)
	{
		snprintf(out, size, "FTDI chip (no information)");
		return;
	}
	FT_GetDriverVersion(ftdi->ft, &drv);
	FT_GetLibraryVersion(&lib);
	snprintf(out, size, "FTDI type %u, id %04X:%04X, \"%s\", driver %X.%02X.%02X, D2XX library %X.%02X.%02X", (unsigned)type,
			 (unsigned)(id >> 16), (unsigned)(id & 0xFFFF), desc, (unsigned)(drv >> 16) & 0xFF, (unsigned)(drv >> 8) & 0xFF,
			 (unsigned)drv & 0xFF, (unsigned)(lib >> 16) & 0xFF, (unsigned)(lib >> 8) & 0xFF, (unsigned)lib & 0xFF);
#else
	snprintf(out, size, "FTDI chip through libftdi, type %d", (int)ftdi->type);
#endif
}

void	USB_SendDeviceId(void)
{
	char	serial[64];

	if (USB_GetSerial(serial))
		USB_Process('I', (uint8_t *)serial, 0, DEVICE_ID_SIZE);
}

// One byte sent as it is (no command: the disc reader's stream reads it, see disclive.c)
bool	USB_SendRawByte(uint8_t c)
{
	return (ftdi_write_data(ftdi, &c, 1) == 1);
}

static Ahead	aheads[2];

static Ahead	*AheadOf(void)
{
	int	i;

	for (i = 0; i < 2; i++)
		if (aheads[i].owner == (void *)ftdi)
			return (&aheads[i]);
	for (i = 0; i < 2; i++)
		if (!aheads[i].owner || (aheads[i].start == aheads[i].end))
		{
			aheads[i].owner = (void *)ftdi;
			aheads[i].start = aheads[i].end = 0;
			return (&aheads[i]);
		}
	return (&aheads[0]);
}

void	USB_Unread(const uint8_t *data, int n)
{
	int	have = ahead_end - ahead_start;

	if (n > 64)
		n = 64;
	if (ahead_start >= n)
		ahead_start -= n;
	else
	{
		memmove(ahead + n, ahead + ahead_start, have);
		ahead_start = 0;
		ahead_end = n + have;
	}
	memcpy(ahead + ahead_start, data, n);
}

// Read exactly "size" bytes (reads may return less data than requested), fails after USB_READ_TIMEOUT seconds without data
bool	USB_ReadData(uint8_t *data, uint32_t size)
{
	time_t	last = time(NULL);
	int		n;

	while (size && (ahead_start < ahead_end))
	{
		*data++ = ahead[ahead_start++];
		size--;
	}
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
// -vlink: a cartridge not answering is waited for (BEN's USB went silent for over 5 s while its menu video played: the
// relay gave up although the console was fine); a disconnected one still fails (the read itself fails)
volatile int		usb_patient;
static bool	USB_ReadFrom(void *handle, uint8_t *data, uint32_t size)
{
	time_t	last = time(NULL);
	int		n;

	while (size)
	{
#ifdef _WIN32
		n = ftdi_read_data((UsbDevice *)handle, data, size);
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
		else if ((time(NULL) - last > USB_READ_TIMEOUT) && !(usb_patient && !LinkStopFlag()))
			return (false);
	}
	return (true);
}

static bool	USB_WriteTo(void *handle, uint8_t *data, uint32_t size)
{
#ifdef _WIN32
	return (ftdi_write_data((UsbDevice *)handle, data, size) == size);
#else
	return (ftdi_write_data((struct ftdi_context *)handle, data, size) == (int)size);
#endif
}

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
	uint32_t		size = (wsize << 16) | rsize, tail = 0;
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
	// (a size 0: no packet, so no CRC and no acknowledgement for it; e.g. a read alone, the rings of -net)
	if (wsize)
	{
		memcpy(out + 10, wdata, wsize);
		out[10 + wsize + tail++] = 'N';		// Written packet taken (its CRC is checked below)
	}
	if (rsize)
		out[10 + wsize + tail++] = 'N';		// Read packet taken
	if (!USB_WriteTo(handle, out, served ? 10 : 10 + wsize + tail))
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
		if (served)
			putchar(ack);
		// (else the program's text on its USB tty, skipped as USB_Process does)
	}
	if (served && !USB_WriteTo(handle, out + 10, wsize + tail))	// (answered: the rest at once)
		return (false);
	if (wsize)
	{
		if (!USB_ReadFrom(handle, &crc, 1))
			return (false);
		if (crc != CRC8_Compute(wdata, wsize))
			link_crc_errors++;
	}
	if (!rsize)
		return (true);
	if (!USB_ReadFrom(handle, in, rsize) || !USB_ReadFrom(handle, &ack, 1))
		return (false);
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

// The exchange in two halves, so that the next one can be sent before this one's answer is read (-vlink: one always on
// its way, the cartridge takes them from its USB FIFO in order): the command with its written packet, then the answer
// (wdata: what was sent, for the CRC check). The program's text before an answer is skipped, as USB_Exchange does.
bool	USB_ExchangeSend(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint32_t rsize)
{
	uint8_t		out[1+4+4+1 + MAX_PACKET_SIZE + 2];
	uint32_t	size = (wsize << 16) | rsize, tail = 0;

	if ((wsize > MAX_PACKET_SIZE) || (rsize > MAX_PACKET_SIZE))
		return (false);
	out[0] = 'X';
	out[1] = adrs >> 24;
	out[2] = adrs >> 16;
	out[3] = adrs >> 8;
	out[4] = adrs;
	out[5] = size >> 24;
	out[6] = size >> 16;
	out[7] = size >> 8;
	out[8] = size;
	out[9] = CRC8_Compute(out, 1+4+4);
	if (wsize)
	{
		memcpy(out + 10, wdata, wsize);
		out[10 + wsize + tail++] = 'N';
	}
	if (rsize)
		out[10 + wsize + tail++] = 'N';
	return (USB_WriteTo(handle, out, 10 + wsize + tail));
}

bool	USB_ExchangeRecv(void *handle, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize)
{
	uint8_t		in[MAX_PACKET_SIZE], ack, crc;
	int			n;

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
			if (ack != 'q')
				return (false);
		}
	}
	if (wsize)
	{
		if (!USB_ReadFrom(handle, &crc, 1))
			return (false);
		if (crc != CRC8_Compute(wdata, wsize))
			link_crc_errors++;
	}
	if (!rsize)
		return (true);
	if (!USB_ReadFrom(handle, in, rsize) || !USB_ReadFrom(handle, &ack, 1))
		return (false);
	if (ack != CRC8_Compute(in, rsize))
		link_crc_errors++;
	else
		memcpy(rdata, in, rsize);
	return (true);
}

// The relay of a debug session (gdbserver.c): the debugged cartridge's exchange, the stop notifications kept
bool	USB_ExchangeServed(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize)
{
	return (ExchangeCmd(handle, MON_CMD_EXCHANGE, true, adrs, wdata, wsize, rdata, rsize));
}

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

uint32_t	NowMs(void)
{
#ifdef _WIN32
	return (GetTickCount());
#else
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

uint32_t	USB_NowMs(void)
{
	return (NowMs());
}

// After an upload (its progress line ended): its transfer rate and time
void	PrintUploadTime(uint32_t bytes, uint32_t start_ms)
{
	uint32_t	ms = NowMs() - start_ms;

	printf((ms < 10000) ? "\nTransfer rate: %u KB/s\nTime taken: %.2f seconds\n" :
		"\nTransfer rate: %u KB/s\nTime taken: %.1f seconds\n",
		ms ? (uint32_t)((uint64_t)bytes * 1000 / 1024 / ms) : 0, ms / 1000.0);
}

int	USB_EscapedAnswers(void)
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

// Bus timing test (-timingtest, timingtest.c): each packet that fails is looked at (what arrived: bytes damaged, one
// too many, one missing) instead of only being asked again, and counted
bool		usb_test_mode;
uint32_t	usb_retry_count;	// Packets asked again (all transfers)
void		(*usb_test_packet)(char cmd, uint32_t offset, const uint8_t *got, uint32_t got_n, uint32_t want_n,
							   uint32_t extra);

void	USB_PurgeNow(void)
{
	USB_Purge(ftdi);
}

// A download packet and its CRC byte, test mode: 1 good (in data), 0 bad (the PS1 is asked again), -1 USB error.
// What arrived is kept whatever it is: fewer bytes than sent (a byte missing: the CRC never comes), more (a byte
// written twice: the CRC read is a data byte, the real one comes after).
static int	USB_TestPacket(char cmd, uint8_t *data, uint32_t lsize, uint32_t offset)
{
	static uint8_t	*buf;
	static uint32_t	buf_size;
	uint32_t		n = 0, extra = 0, last;
	uint8_t			c;
	int				r;

	if (buf_size < lsize + 4096)
	{
		free(buf);
		buf_size = lsize + 4096;
		if (!(buf = malloc(buf_size)))
		{
			buf_size = 0;
			return (-1);
		}
	}
	for (last = NowMs(); n < lsize + 1; )
	{
		r = ftdi_read_data(ftdi, buf + n, lsize + 1 - n);
		if (r < 0)
			return (-1);
		if (r > 0)
		{
			n += r;
			last = NowMs();
		}
		else if (NowMs() - last > 400)
			break;
	}
	// Test of the test: TIMINGTEST_INJECT=N, every Nth packet damaged as the bus could (a bit, a byte written twice,
	// a byte missing, the CRC byte), the console asked again as for a real error
	{
		static int	inject = -1, count;

		if (inject < 0)
			inject = getenv("TIMINGTEST_INJECT") ? atoi(getenv("TIMINGTEST_INJECT")) : 0;
		if ((inject > 0) && (n == lsize + 1) && (lsize > 400) && !(++count % inject))
		{
			switch ((count / inject) & 3)
			{
				case 0:	buf[100] ^= 0x08; break;
				case 1:	memmove(buf + 201, buf + 200, n - 200); buf[200] = buf[199]; n++; break;
				case 2:	memmove(buf + 300, buf + 301, n - 301); n--; break;
				case 3:	buf[lsize] ^= 0xFF; break;
			}
			if (n > lsize + 1)
				extra = n - (lsize + 1);
			usb_retry_count++;
			if (usb_test_packet)
				usb_test_packet(cmd, offset, buf, n, lsize + 1, extra);
			return (0);
		}
	}
	if ((n == lsize + 1) && (buf[lsize] == CRC8_Compute(buf, lsize)))
	{
		memcpy(data, buf, lsize);
		return (1);
	}
	// Bytes after it (a byte too many: they were sent with the packet)
#ifdef _WIN32
	Sleep(30);
#else
	usleep(30000);
#endif
	while ((n + extra < buf_size) && USB_ReadByteNoWait(&c))
		buf[n + extra++] = c;
	usb_retry_count++;
	if (usb_test_packet)
		usb_test_packet(cmd, offset, buf, n + extra, lsize + 1, extra);
	return (0);
}

bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size)
{
	uint8_t		retry, ack, crc8, *odata, *start = data;
	uint32_t	lsize, usize, packet_size = MAX_PACKET_SIZE, header_size = size, total;
	bool		ack_ok;
	int			skipped;
	bool		escaped, request_dropped = false;
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

	retry = usb_test_mode ? 12 : 3;

	// Get Ack Packet. Before it: text the program printed on its USB tty (e.g. a link game run with the default tty,
	// writing every frame), skipped; while profiling, the profiler packets (DLE + type, see profiler.c); "DLE q"
	// (answer to the 'Q'). Escaped answer: "DLE g" / "DLE b", the only one accepted when the firmware has them (text
	// can't be taken for it). Otherwise 'G' / 'B' (a 'G' or 'B' in text just before is taken for it: older firmwares).
	// A request of the program's file server meanwhile (e.g. the profiler or the frame capture of a program reading
	// files): the program sent it before this command reached the PS1 (its USB chip keeps the bytes a few milliseconds),
	// then took this command's first byte for the answer to its request. The request was not done: dropped here (not
	// served: the PS1 doesn't wait for it any more). The kernel hook answers the rest of this command, with a plain
	// 'G' / 'B' (its 'Q' taken), then the program sends its request again (PS1 DEV CART 1.4, usb.h USB_ProcessSend;
	// older firmwares return an error to the program).
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
			if (ack == 'q')
				;
			else if (PC_IsFileServerRequest(ack))
			{
				uint8_t	rest[4+4+1];

				if (!(ack_ok = USB_ReadData(rest, sizeof(rest))))
					break;
				request_dropped = true;
				skipped = 0;
			}
			else if (prof_active && !(ack_ok = ProfReadPacketType(ack)))
				break;
		}
		else if ((!escaped || request_dropped) && ((ack == 'G') || (ack == 'B')))
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
					if (usb_test_mode && ((cmd == 'D') || (cmd == 'd')))
					{
						int	got = USB_TestPacket(cmd, data, lsize, data - start);

						if (got < 0)
							return (false);
						if (got)
						{
							ack = 'N';
							size -= lsize;
							data += lsize;
						}
						else
						{
							ack = 'A';
							if (!--retry)
								return (false);
						}
						if (ftdi_write_data(ftdi, &ack, 1) != 1)
							return (false);
						continue;
					}
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
							usb_retry_count++;
							if (!usb_test_mode)
								printf("\nretry...\n");
							else if (usb_test_packet)
								usb_test_packet(cmd, odata - start, NULL, 0, lsize, 0);
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
							usb_retry_count++;
							if (!usb_test_mode)
								printf("\nretry...\n");
							else if (usb_test_packet)
								usb_test_packet(cmd, odata - start, NULL, 0, lsize, 0);
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
#endif
	int		n;

	if (ahead_start == ahead_end)
	{
		ahead_start = ahead_end = 0;
#ifdef _WIN32
		if (ftdi->com)
			queued = SerialQueued(ftdi->com);
		else if (FT_GetQueueStatus(ftdi->ft, &queued) != FT_OK)
			queued = 0;
		if (!queued)
			return (false);
		n = ftdi_read_data(ftdi, ahead, (queued < AHEAD_SIZE) ? queued : AHEAD_SIZE);
#else
		n = ftdi_read_data(ftdi, ahead, 1);
#endif
		if (n <= 0)
			return (false);
		ahead_end = n;
	}
	*c = ahead[ahead_start++];
	return (true);
}

bool	USB_WriteBytes(uint8_t *data, uint32_t size)
{
	return (ftdi_write_data(ftdi, data, size) == size);
}

