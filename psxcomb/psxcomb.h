/*
* PS1 DEV CART psxcomb: link cable library for PSn00bSDK and PsyQo, compatible with the PlayStation link cable library
* libcomb (same functions and macros, "sio:" device, asynchronous read/write ended by events, same signals on the
* cable), plus a USB link: two consoles linked through their cartridges and the computer ("ps1transfer -relay").
* PsyQ code including <libcomb.h> builds unchanged: libcomb.h in this folder includes this file.
* Without PSn00bSDK (PsyQo, or any program keeping the BIOS kernel), psxcomb.c uses the BIOS itself (psxcomb_bios.h):
* compile psxcomb.c with your program as a C file. PsyQo: don't call takeOverKernel() (the BIOS devices, events and
* the cartridge's kernel hook are needed); the events can be waited for with psyqo::Kernel::openEvent.
*
* Use: AddCOMB(), then open("sio:", O_RDONLY | O_NOWAIT) / open("sio:", O_WRONLY | O_NOWAIT), read()/write(), and the
* events HwSIO EvSpIOER (read ended), EvSpIOEW (write ended), EvSpERROR, EvSpTIMOUT (wait callback said stop).
* CombSetLink(COMB_LINK_USB) uses the USB link instead of the serial port (same calls).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PSXCOMB_H_
#define _PSXCOMB_H_

#include <stdint.h>

// Serial controller status bits (CombSioStatus)
#define	COMB_CTS		0x100
#define	COMB_DSR		0x80
#define	COMB_FE			0x20		// Framing error
#define	COMB_OE			0x10		// Overrun error
#define	COMB_PERROR		0x8			// Parity error
#define	COMB_TXU		0x4			// Transmitter empty
#define	COMB_RXRDY		0x2
#define	COMB_TXRDY		0x1

// Control lines (CombSetControl, CombControlStatus)
#define	COMB_BIT_DTR	0x1
#define	COMB_BIT_RTS	0x2

// Link (CombSetLink, CombGetLink)
#define	COMB_LINK_SERIAL	0		// Link cable (default)
#define	COMB_LINK_USB		1		// Through the cartridges and the computer ("ps1transfer -n SERIAL1 -relay SERIAL2")

#define	CombSioStatus()			_comb_control(0, 0, 0)	// Serial controller status
#define	CombControlStatus()		_comb_control(0, 1, 0)	// Control lines: DTR bit 0, RTS bit 1
#define	CombGetMode()			_comb_control(0, 2, 0)	// Communication mode (serial mode register)
#define	CombGetBPS()			_comb_control(0, 3, 0)	// Transfer rate
#define	CombGetPacketSize()		_comb_control(0, 4, 0)	// Bytes per handshake
#define	CombBytesToWrite()		_comb_control(0, 5, 0)	// Bytes left in the write in progress
#define	CombBytesToRead()		_comb_control(0, 5, 1)	// Bytes left to read
#define	CombBytesRemaining(a)	_comb_control(0, 5, a)
#define	CombAsyncRequest(a)		_comb_control(0, 6, a)	// Write (0) / read (1) in progress

#define	CombSetControl(a)		_comb_control(1, 1, a)	// DTR / RTS
#define	CombSetMode(a)			_comb_control(1, 2, a)
#define	CombSetBPS(a)			_comb_control(1, 3, a)	// -1 if 2073600 / rate isn't a whole number
#define	CombSetPacketSize(a)	_comb_control(1, 4, a)	// 1, 2, 4 or 8 bytes per handshake

#define	CombReset()				_comb_control(2, 0, 0)	// Serial controller reset (settings kept)
#define	CombResetError()		_comb_control(2, 1, 0)
#define	CombCancelWrite()		_comb_control(2, 2, 0)
#define	CombCancelRead()		_comb_control(2, 3, 0)

#define	CombSetRTS(a)			_comb_control(3, 0, a)
#define	CombCTS()				_comb_control(3, 1, 0)

#define	CombWaitCallback(a)		_comb_control(4, 0, (unsigned long)(a))	// long func(long rw, long count): 0 = stop
#define	CombResetVBLANK()		_comb_control(5, 0, 0)	// (nothing to do here, kept for compatibility)

#define	CombGetLink()			_comb_control(6, 0, 0)
#define	CombSetLink(a)			_comb_control(6, 1, a)	// COMB_LINK_SERIAL / COMB_LINK_USB

// Events (class HwSIO) and open() flags, as the PlayStation SDK headers define them
#ifndef EvSpIOER
#define	EvSpIOER		0x0400		// Read ended
#endif
#ifndef EvSpIOEW
#define	EvSpIOEW		0x0800		// Write ended
#endif
#ifndef O_RDONLY
#define	O_RDONLY		0x0001
#endif
#ifndef O_WRONLY
#define	O_WRONLY		0x0002
#endif
#ifndef O_RDWR
#define	O_RDWR			0x0003
#endif
#ifndef O_NOWAIT
#define	O_NOWAIT		0x8000		// Asynchronous read/write (FASYNC)
#endif

#if defined(__cplusplus)
extern "C" {
#endif

void	AddCOMB(void);
void	DelCOMB(void);
void	ChangeClearSIO(long mode);
long	_comb_control(unsigned long cmd, unsigned long sub, unsigned long arg);

#if defined(__cplusplus)
}
#endif

#endif
