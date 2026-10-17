/*
* PS1 DEV CART USB link: two consoles linked through their cartridges and this computer instead of a serial cable.
* Shared by the link tester (linktest.c, "ps1transfer -link usb") and the relay (ps1transfer/usblink.c,
* "ps1transfer -n SERIAL1 -relay SERIAL2").
*
* Each console keeps two rings in RAM: the bytes it sends (tx) and the bytes it receives (rx). The relay reads a
* console's tx ring and writes the bytes into the other console's rx ring, with the memory access of the kernel hook
* ('D'/'U' while a program runs, PS1 DEV CART firmware). Indexes count bytes since the start (they wrap at 2^32), the
* position in a ring is index & (USBLINK_RING - 1).
* - tx_head: written by the PS1 after the bytes; tx_tail: written by the relay once it has read them
* - rx_head: written by the relay after the bytes; rx_tail: written by the PS1 once it has read them
* The PS1 program must keep the kernel hook answering often (e.g. 1kHz timer interrupt).
* The rings are at USBLINK_ADRS, or at the address a program publishes at 0x8000DF38 (psxcomb's USB link, with its
* own ring size).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _USBLINK_H_
#define _USBLINK_H_

#include <stdint.h>

#define	USBLINK_ADRS		0x80100000
#define	USBLINK_MAGIC		0x4B4E4C55		// "ULNK", set once the rings are ready
#define	USBLINK_VERSION		1
#define	USBLINK_RING		16384			// Bytes, power of 2

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	tx_head;
	uint32_t	tx_tail;
	uint32_t	rx_head;
	uint32_t	rx_tail;
	uint32_t	ring_size;					// Bytes per ring (0: USBLINK_RING), power of 2
	uint32_t	reserved;
	uint8_t		tx[USBLINK_RING];
	uint8_t		rx[USBLINK_RING];
} UsbLink;

#endif
