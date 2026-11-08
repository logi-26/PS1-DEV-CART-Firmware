/*
* PS1 DEV CART bus timing test: definitions shared by timingtest.c (PS1) and ps1transfer ("-timingtest")
*
* The USB link goes through the cartridge port (expansion 1), whose bus timing the BIOS sets at boot (0x1F801008
* EXP1 delay/size, 0x1F801020 COM_DELAY). Some console revisions (SCPH-7502...) may need another timing for the
* cartridge's USB chip. The test program sets each timing asked by the computer, which then reads and writes test
* data through the kernel hook with it and counts the errors (ps1transfer/timingtest.c).
*
* Mailbox (the memory access of the kernel hook while the program runs):
* - the computer writes cmd and its arguments, then increments cmd_seq; done_seq = cmd_seq once done;
* - heartbeat: incremented by the computer at least every second while a timing other than the console's own is
*   set. Without it for WATCHDOG_FRAMES frames (the link broken by that timing), the console's timing is set back.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _TIMINGTEST_H_
#define _TIMINGTEST_H_

#include <stdint.h>

#define	TT_MAILBOX_ADRS		0x80100000
#define	TT_BUFFER_ADRS		0x80101000
#define	TT_BUFFER_SIZE		0x40000				// 256KB test data (the pattern made again after it to check an upload)
#define	TT_MAGIC			0x54544D54			// "TMTT", set once the program is ready
#define	TT_VERSION			1
#define	TT_WATCHDOG_FRAMES	300					// (5 to 6 seconds: longer than the computer's wait after a failed transfer)

// Commands
#define	TT_SET_TIMING		1		// arg0: EXP1 delay/size, arg1: COM_DELAY (0: the console's own values for both)
#define	TT_FILL				2		// arg0: seed. The buffer filled with the test pattern
#define	TT_VERIFY			3		// arg0: seed. result: bytes not as the pattern, bad[]: the first ones
#define	TT_LOAD				4		// arg0: 1 = DMA traffic (GPU and SPU) all the time, as a game, 0 = idle

#define	TT_MAX_BAD			8

typedef struct
{
	uint32_t	offset;
	uint8_t		expected, got, next_expected, next_got;
} TtBadByte;

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	cmd_seq;
	uint32_t	done_seq;
	uint32_t	cmd;
	uint32_t	arg0, arg1;
	int32_t		result;
	uint32_t	heartbeat;			// Written by the computer
	uint32_t	reverted;			// Times the watchdog set the console's timing back
	uint32_t	exp1_now, com_now;	// Read back from the registers
	uint32_t	frames;
	uint32_t	load;
	uint32_t	load_loops;			// DMA transfers done with TT_LOAD 1
	// The console (read at start, before any change)
	uint32_t	memctrl[9];			// 0x1F801000-0x1F801020 (memctrl[2]: EXP1 delay/size, memctrl[8]: COM_DELAY)
	uint32_t	ram_size;			// 0x1F801060
	uint32_t	cop0_prid;
	uint32_t	gpu_version;		// GP1(0x10) info 7
	uint32_t	bios_date;			// 0xBFC00100 (BCD yyyymmdd)
	char		bios_version[48];	// 0xBFC7FF32 (BIOS 2.x and later: "System ROM Version ...")
	uint32_t	verify_shift;		// TT_VERIFY: 1 = a byte missing at the first bad one, 2 = a byte too many, 0 = neither
	TtBadByte	bad[TT_MAX_BAD];
} TtMailbox;

// Test pattern: 4KB of 0x00/0xFF, 4KB of 0x55/0xAA, 4KB of 0..255 (all bus lines switching in every way), then
// pseudo-random bytes (xorshift32 from the seed)
static inline void	TtPattern(uint8_t *buf, uint32_t size, uint32_t seed)
{
	uint32_t	i, x = seed * 0x9E3779B9u + 0x12345u;

	for (i = 0; i < size; i++)
	{
		if (i < 0x1000)
			buf[i] = (i & 1) ? 0xFF : 0x00;
		else if (i < 0x2000)
			buf[i] = (i & 1) ? 0xAA : 0x55;
		else if (i < 0x3000)
			buf[i] = (uint8_t)(i + seed);
		else
		{
			if (!(i & 3))
			{
				x ^= x << 13;
				x ^= x >> 17;
				x ^= x << 5;
			}
			buf[i] = (uint8_t)(x >> ((i & 3) * 8));
		}
	}
}

#endif
