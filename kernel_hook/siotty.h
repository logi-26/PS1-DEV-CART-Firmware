/*
* PS1 DEV CART firmware
* Serial port (SIO) TTY driver, installed by the kernel hook with ps1transfer "-c sio"
*
* The Net Yaroze boot disc monitor installs its own "tty" console driver on the serial port,
* Net Yaroze programs use it with open("tty:"), read(), write() and the SIO ioctls of <sys/ioctl.h>
* (e.g. for link play between two consoles).
* This is a small polled driver compatible with these calls, so programs can do the same when
* executed by PS1 DEV CART. The receive FIFO is also emptied by the kernel hook on every exception,
* so less data is lost when the program doesn't read often enough.
*
* USB link ("-c link"): the same driver without the serial port, for link play through the PS1 DEV CART
* cartridges and the computer ("ps1transfer -relay"): the computer writes the received bytes to rx_buffer and reads
* the sent bytes from tx_buffer (SioBuffers, address in the kernel hook params). Each side writes only its half of
* SioBuffers, the computer in one USB transfer (atomic for the program: the kernel hook runs it in an exception).
* Ring positions are masked indexes (empty: head == tail, full: head + 1 == tail).
* Frame pacing (the timing of a serial cable): each write of the program is a "unit" (e.g. a YarIO packet), the
* computer passes the units on with their size, and the receiving driver gives the program one unit per frame (frames
* counted at the vertical blanks, when the kernel hook runs). Programs like YarIO keep only the last packet read in a frame: two
* packets arriving in the same frame (USB delivery isn't as regular as a cable) would lose one and desynchronise the
* consoles. A backlog of more than 1 unit is given at once (like the bytes waiting in a serial port buffer).
*
* The driver's code is in the debug monitor (cartridge flash, see monitor/monitor.c) to make room in the kernel area:
* the kernel hook only holds its state and buffers (RAM: the monitor has no writable variables) and calls the monitor
* to set it up (MON_REASON_SIO_SETUP) and at each exception (MON_REASON_SIO_UPDATE). This file: the types and constants
* shared with the monitor, and (kernel hook, HOOK defined) that part.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _SIOTTY_H_
#define _SIOTTY_H_

#include <stdint.h>
#include <stdbool.h>

#define	SIO_BAUD_DEFAULT	115200
#define	SIO_RX_BUFFER_SIZE	256		// Power of 2
#define	LINK_TX_BUFFER_SIZE	64		// Power of 2, USB link only
#define	LINK_UNITS			8		// Power of 2, frame units of the USB link

typedef struct
{
	// Written by the computer (USB link), or rx_head by SIO_Poll (serial port)
	volatile uint32_t	rx_head;
	volatile uint32_t	tx_tail;
	volatile uint32_t	rx_unit_head;
	volatile uint32_t	tx_unit_tail;
	volatile uint16_t	rx_units[LINK_UNITS];		// Sizes of the units received
	volatile uint8_t	rx_buffer[SIO_RX_BUFFER_SIZE];
	// Written by the program (driver) and the kernel hook
	volatile uint32_t	rx_tail;
	volatile uint32_t	tx_head;
	volatile uint32_t	rx_unit_tail;
	volatile uint32_t	tx_unit_head;
	volatile uint16_t	tx_units[LINK_UNITS];		// End (tx_buffer position) of each unit sent; tx_unit_head: open unit
	volatile uint8_t	tx_buffer[LINK_TX_BUFFER_SIZE];
} SioBuffers;

// The driver's state (set up by the monitor)
typedef struct
{
	volatile uint32_t	link_frame;		// Vertical blanks seen by the kernel hook (USB link)
	uint32_t			link_released;	// Frame of the last unit given to the program
	uint32_t			link_allow;		// Bytes of the released units not read yet
	uint32_t			baud;
	uint16_t			mode, ctrl;		// Serial port mode and control
	bool				link;			// USB link: no serial port
	volatile bool		link_open;		// The unit at tx_unit_head has bytes (not closed yet: unit ring full)
	volatile bool		busy;			// The kernel hook doesn't poll while the program uses the driver
	bool				raw;			// TIOCRAW: no '\r' added before '\n'
} SioState;

#ifdef HOOK
SioBuffers		sio_buf;	// (global: the profiler's RAM area, see monitor/monitor.h)
SioState		sio_state;
bool			is_sio_tty_enabled;		// Set by the monitor

// Replace the BIOS TTY device (stdin/stdout) by the serial port driver, like the Net Yaroze boot disc
// (link: USB link instead of the serial port)
void	SetupSIOtty(bool link)
{
	if (mon_state.entry)
		((void (*)(int, uint32_t *))mon_state.entry)(MON_REASON_SIO_SETUP, (uint32_t *)(link ? 1 : 0));
}

// Called by the kernel hook on every exception: frames counted (USB link), the receive FIFO emptied (serial port)
static inline void	SIO_tty_Update(void)
{
	if (is_sio_tty_enabled)
		((void (*)(int, uint32_t *))mon_state.entry)(MON_REASON_SIO_UPDATE, NULL);
}
#endif

#endif
