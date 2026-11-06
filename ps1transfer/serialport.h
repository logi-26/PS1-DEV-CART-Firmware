/*
* PS1 DEV CART Transfer tool: a cartridge on a serial port (Windows COM port), see serialport.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_SERIALPORT_H_
#define _PS1TRANSFER_SERIALPORT_H_

#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

// The port "COM7", or the USB device with this serial number: its handle (INVALID_HANDLE_VALUE if not found or in
// use). port/serial (at least 64 bytes each, may be NULL) receive the port and the device's serial number.
HANDLE		SerialOpen(const char *name, char *port, char *serial);
void		SerialClose(HANDLE h);
// Bytes read (0 after read_ms without any), written (all of them, or fewer after write_ms)
uint32_t	SerialRead(HANDLE h, uint8_t *data, uint32_t size);
uint32_t	SerialWrite(HANDLE h, const uint8_t *data, uint32_t size);
uint32_t	SerialQueued(HANDLE h);
void		SerialPurge(HANDLE h);
void		SerialSetTimeouts(HANDLE h, uint32_t read_ms, uint32_t write_ms);
// The USB serial ports: "-com COM8  -n NAME  description" lines (skip_ftdi: not the FTDI chips' virtual COM ports,
// already listed through D2XX), how many
int			SerialList(bool skip_ftdi);
#endif

#endif
