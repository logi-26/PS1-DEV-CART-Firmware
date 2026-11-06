/*
* PS1 DEV CART Transfer tool: a cartridge on a serial port (Windows COM port)
*
*	-com COM7		the cartridge on this port
*	-com NAME		the USB serial port with this serial number (also tried by -n NAME when no FTDI chip has it)
*
* For USB devices that aren't FTDI chips (e.g. a Raspberry Pi Pico showing a USB CDC serial port), and for an FTDI
* chip through its virtual COM port driver (VCP) instead of D2XX. The data is the same either way: the PS1 can't tell.
* What a COM port can't do: set an FTDI chip's latency timer (the PS1 answers each packet with a single CRC byte,
* which the chip only sends when its timer expires: at the 16ms default, transfers are much slower; Device Manager,
* the port's Properties > Port Settings > Advanced > Latency Timer), and write its EEPROM (-rename).
*
* A port's serial number comes from its USB device's instance ID (Windows uses the USB serial number there when the
* device has one): "FTDIBUS\VID_0403+PID_6001+BILLA\0000" (an FTDI chip's VCP, its name followed by the channel A),
* "USB\VID_2E8A&PID_000A\E6613893..." (the device itself, or the parent of a composite device's "&MI_00" interface).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifdef _WIN32
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include "serialport.h"

#define	SERIAL_BUFFER_SIZE	65536	// Driver buffers (USB_WRITE_SIZE writes, 2KB packets read)

// Ports class {4D36E978-E325-11CE-BFC1-08002BE10318} (GUID_DEVCLASS_PORTS, without devguid.h/initguid.h)
static const GUID	ports_class = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

typedef struct
{
	char	port[16];
	char	serial[64];
	char	desc[128];
	bool	ftdi;
	DWORD	latency;	// FTDI VCP latency timer (ms), 0 if unknown
} PortInfo;

// "+PID_6001+BILLA\" -> "BILL": the single channel FTDI chips' serial number, followed by the channel letter
static void	FtdiSerial(const char *id, char *serial)
{
	const char	*p = strstr(id, "+PID_"), *end;
	int			pid, len;

	serial[0] = 0;
	if (!p || (sscanf(p + 5, "%x", &pid) != 1) || !(p = strchr(p + 1, '+')))
		return;
	p++;
	end = strchr(p, '\\');
	len = end ? (int)(end - p) : (int)strlen(p);
	if (len > 63)
		len = 63;
	memcpy(serial, p, len);
	serial[len] = 0;
	// FT232R/FT245R, FT232H, FT-X: D2XX gives the serial number without the letter (multi channel chips: with it)
	if (((pid == 0x6001) || (pid == 0x6014) || (pid == 0x6015)) && (len > 1) && (serial[len - 1] == 'A'))
		serial[len - 1] = 0;
}

// "USB\VID_2E8A&PID_000A\E66138935F2B4A2C" -> its last part, unless Windows made it (no serial number: "6&2F3A..&0&1")
static bool	UsbSerial(const char *id, char *serial)
{
	const char	*p = strrchr(id, '\\');

	serial[0] = 0;
	if (strncmp(id, "USB\\", 4) || strstr(id, "&MI_") || !p || strchr(p + 1, '&') || !p[1])
		return (false);
	snprintf(serial, 64, "%s", p + 1);
	return (true);
}

static bool	PortInfoGet(HDEVINFO set, SP_DEVINFO_DATA *dev, PortInfo *info)
{
	char	id[256];
	HKEY	key;
	DWORD	size, type;

	memset(info, 0, sizeof(*info));
	key = SetupDiOpenDevRegKey(set, dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
	if (key == INVALID_HANDLE_VALUE)
		return (false);
	size = sizeof(info->port) - 1;
	if ((RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)info->port, &size) != ERROR_SUCCESS) ||
		(type != REG_SZ))
		info->port[0] = 0;
	size = sizeof(info->latency);
	if ((RegQueryValueExA(key, "LatencyTimer", NULL, &type, (BYTE *)&info->latency, &size) != ERROR_SUCCESS) ||
		(type != REG_DWORD))
		info->latency = 0;
	RegCloseKey(key);
	if (strncmp(info->port, "COM", 3))
		return (false);

	if (!SetupDiGetDeviceInstanceIdA(set, dev, id, sizeof(id), NULL))
		return (false);
	if (!strncmp(id, "FTDIBUS\\", 8))
	{
		info->ftdi = true;
		FtdiSerial(id, info->serial);
	}
	else if (!strncmp(id, "USB\\", 4))	// (not the computer's own serial ports)
	{
		DEVINST	parent;

		if (!UsbSerial(id, info->serial) && (CM_Get_Parent(&parent, dev->DevInst, 0) == CR_SUCCESS) &&
			(CM_Get_Device_IDA(parent, id, sizeof(id), 0) == CR_SUCCESS))
			UsbSerial(id, info->serial);
	}
	else
		return (false);
	if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_FRIENDLYNAME, NULL, (BYTE *)info->desc,
		sizeof(info->desc) - 1, NULL))
		snprintf(info->desc, sizeof(info->desc), "%s", info->port);
	return (true);
}

// The USB serial ports, each given to "found" (stops when it returns true): true if one was taken
static bool	PortsEnum(bool (*found)(const PortInfo *, void *), void *arg)
{
	HDEVINFO		set = SetupDiGetClassDevsA(&ports_class, NULL, NULL, DIGCF_PRESENT);
	SP_DEVINFO_DATA	dev;
	PortInfo		info;
	DWORD			i;
	bool			taken = false;

	if (set == INVALID_HANDLE_VALUE)
		return (false);
	dev.cbSize = sizeof(dev);
	for (i = 0; !taken && SetupDiEnumDeviceInfo(set, i, &dev); i++)
		if (PortInfoGet(set, &dev, &info))
			taken = found(&info, arg);
	SetupDiDestroyDeviceInfoList(set);
	return (taken);
}

typedef struct
{
	const char	*name;
	PortInfo	info;
} PortFind;

static bool	PortMatch(const PortInfo *info, void *arg)
{
	PortFind	*f = arg;

	if (strcmp(info->serial, f->name) && _stricmp(info->port, f->name))
		return (false);
	f->info = *info;
	return (true);
}

HANDLE	SerialOpen(const char *name, char *port, char *serial)
{
	PortFind	f;
	char		path[32];
	HANDLE		h;
	DCB			dcb;

	memset(&f, 0, sizeof(f));
	f.name = name;
	if (!PortsEnum(PortMatch, &f))
	{
		// A port not listed (not a USB device, e.g. a USB-serial adapter with a driver of its own): by its name
		if (_strnicmp(name, "COM", 3) || !isdigit((unsigned char)name[3]))
			return (INVALID_HANDLE_VALUE);
		snprintf(f.info.port, sizeof(f.info.port), "%s", name);
	}
	snprintf(path, sizeof(path), "\\\\.\\%s", f.info.port);
	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return (h);

	SetupComm(h, SERIAL_BUFFER_SIZE, SERIAL_BUFFER_SIZE);
	// Raw 8-bit data, no flow control. The baud rate doesn't matter (USB FIFO, USB CDC). DTR on: a USB CDC device
	// (TinyUSB's tud_cdc_connected) only sends data once a program has the port open with DTR set.
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (GetCommState(h, &dcb))
	{
		dcb.BaudRate = 921600;
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		dcb.fParity = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDsrSensitivity = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
		dcb.fNull = FALSE;
		dcb.fAbortOnError = FALSE;
		SetCommState(h, &dcb);
	}
	SerialPurge(h);

	if (f.info.ftdi && (f.info.latency > 1))
		fprintf(stderr, "%s: the FTDI latency timer is %lu ms, transfers are slower than with D2XX (Device Manager: "
			"the port's Properties > Port Settings > Advanced > Latency Timer: 1)\n", f.info.port,
			(unsigned long)f.info.latency);
	if (port)
		snprintf(port, 64, "%s", f.info.port);
	if (serial)
		snprintf(serial, 64, "%s", f.info.serial);
	return (h);
}

void	SerialClose(HANDLE h)
{
	CloseHandle(h);
}

uint32_t	SerialRead(HANDLE h, uint8_t *data, uint32_t size)
{
	DWORD	n = 0;

	if (!ReadFile(h, data, size, &n, NULL))
		return (0);
	return (n);
}

uint32_t	SerialWrite(HANDLE h, const uint8_t *data, uint32_t size)
{
	DWORD	n = 0;

	if (!WriteFile(h, data, size, &n, NULL))
		return (0);
	return (n);
}

uint32_t	SerialQueued(HANDLE h)
{
	DWORD	errors;
	COMSTAT	stat;

	if (!ClearCommError(h, &errors, &stat))
		return (0);
	return (stat.cbInQue);
}

void	SerialPurge(HANDLE h)
{
	PurgeComm(h, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR);
}

// A read returns what has arrived at once, else waits for the first byte up to read_ms (as FT_Read with its timeout,
// which callers loop on)
void	SerialSetTimeouts(HANDLE h, uint32_t read_ms, uint32_t write_ms)
{
	COMMTIMEOUTS	t;

	t.ReadIntervalTimeout = MAXDWORD;
	t.ReadTotalTimeoutMultiplier = MAXDWORD;
	t.ReadTotalTimeoutConstant = read_ms;
	t.WriteTotalTimeoutMultiplier = 0;
	t.WriteTotalTimeoutConstant = write_ms;
	SetCommTimeouts(h, &t);
}

static bool	PortPrint(const PortInfo *info, void *arg)
{
	int		*count = ((int *)arg);
	bool	skip_ftdi = count[1];

	if (skip_ftdi && info->ftdi)
		return (false);
	printf("-com %-6s -n %-12s %s\n", info->port, info->serial[0] ? info->serial : "-", info->desc);
	count[0]++;
	return (false);
}

int	SerialList(bool skip_ftdi)
{
	int	count[2] = {0, skip_ftdi};

	PortsEnum(PortPrint, count);
	return (count[0]);
}
#endif
