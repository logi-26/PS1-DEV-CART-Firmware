/*
* PS1USB firmware by OrionSoft [2024]
* Hook to redirect printf/TTY to USB
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "common/syscalls/syscalls.h"

// Not in syscall.h :/
static __attribute__((always_inline)) int syscall_removeDevice(const char *filename) {
    register int n asm("t1") = 0x48;
    __asm__ volatile("" : "=r"(n) : "r"(n));
    return ((int (*)(const char *))0xb0)(filename);
}

int	USB_tty_open(struct File *file, const char *filename, int mode)
{
	return (file->deviceId);
}

int	USB_tty_action(struct File *file, enum FileAction action)
{
	int					i;
	volatile uint8_t	*buf8 = (volatile uint8_t *)file->buffer;

	// Reads: no tty input comes from the computer (ps1transfer sends none), only its commands, read by the kernel
	// hook. A read returns at once without bytes: it used to wait for bytes (5,000,000 polls each) and took the
	// commands' bytes, so a program reading this tty (e.g. a link game left on it) couldn't be reached any more
	if (action != PSXWRITE)
		return (0);
	// The byte is dropped if the USB FIFO is full (no ps1transfer -c reading), it was reported as
	// not written and the BIOS retried forever with interrupts disabled: programs printing a lot froze
	for (i = 0; i < file->count; i++)
		USB_SendByte(buf8[i]);
	return (file->count);
}

int	USB_tty_dummy(void)
{
	return (0);
}

struct Device	USB_tty_dev =
{
	"tty",
	PSXDTTYPE_CHAR | PSXDTTYPE_CONS,
	0,
	"PS1 DEV CART USB tty",
	(device_init)USB_tty_dummy,
	USB_tty_open,
	USB_tty_action,
	(device_close)USB_tty_dummy,
	(device_ioctl)USB_tty_dummy,
	(device_read)USB_tty_dummy,
	(device_write)USB_tty_dummy,
	USB_tty_dummy,
	USB_tty_dummy,
	(device_firstFile)USB_tty_dummy,
	(device_nextFile)USB_tty_dummy,
	(device_format)USB_tty_dummy,
	USB_tty_dummy,
	USB_tty_dummy,
	(device_deinit)USB_tty_dummy,
	USB_tty_dummy
};

// Replace the TTY device (stdin/stdout) by one of ours (USB here, serial port or USB link in siotty.h)
static __attribute__((noinline)) void	ReplaceTTY(struct Device *dev)
{
	syscall_close(0);					// Close stdin/stdout handles
	syscall_close(1);
	syscall_removeDevice(dev->name);	// Remove default TTY device
	syscall_addDevice(dev);				// Add our own TTY device
	syscall_open(dev->name, PSXF_READ);	// Reopen stdin/stdout
	syscall_open(dev->name, PSXF_WRITE);
}

#define	SetupTTYhook()	ReplaceTTY(&USB_tty_dev)	// TTY device with USB communication callbacks
