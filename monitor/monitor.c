/*
* PS1 DEV CART debug monitor
*
* Called by the kernel hook (kernel_hook/debugmon.h) when a program stops: crash, breakpoint, hardware breakpoint,
* pause. Runs with interrupts disabled until the computer resumes the program:
* - crash outside a debug session: crash screen (moved here from the kernel hook to make room), as before
* - debug session ("ps1transfer -gdb"): the computer is told with a 'B' request, then it reads/writes the memory
*   and the saved registers ('D'/'U') and resumes the program ('C'). Breakpoints are written by the computer.
*
* Executed from the cartridge flash (no writable variables: state in the kernel hook), or from RAM for tests.
* Calls the USB functions of the kernel hook (linked with its symbols).
*
* Crash screen based on the PS1 Crash Handler by Spencer Alves (impiaaa), public domain (unlicense.org),
* with the changes of the original firmware (font, cause, registers display).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "monitor.h"

#define	MON_STR(_x_)	#_x_
#define	MON_XSTR(_x_)	MON_STR(_x_)

// Kernel hook functions and variables (see usb.h, kernel_hook/debugmon.h)
int		USB_Process(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
bool	USB_GetByte(volatile uint8_t *data);
bool	USB_GetByteNoTimeout(volatile uint8_t *data);
bool	USB_SendByteTimeout(uint8_t data, uint32_t timeout);
bool	USB_SendData(volatile uint8_t *ptr, uint32_t size, uint32_t packet_size);
bool	USB_GetData(volatile uint8_t *ptr, uint32_t size, uint32_t packet_size);
void	ResetPS1(void);
extern MonitorState	mon_state;

#define	TTY_MODE			(*((volatile uint8_t *)0x8000DF71))	// KernelHookParams.tty_mode (EXEC_DEBUG)
#define	MAX_PACKET_SIZE		16384	// HOOK_PACKET_SIZE of usb.h (FEATURE_HOOK_BIG_PACKETS)
#define	TIMEOUT_CNT			5000000
#define	POLLHOST_BREAK		0x0001000D	// "break 1024" (pollhost of the official tools): ignored

#define	GP0					(*((volatile uint32_t *)0xBF801810))
#define	GP1					(*((volatile uint32_t *)0xBF801814))

void	MonitorEntry(int reason, uint32_t *regs);

// Command packet CRC, the same as usb.h (reflected, polynomial 0x8C) bit by bit: the kernel hook's is inlined (space)
static uint8_t	CRC8_Compute(const uint8_t *data, uint32_t size)
{
	uint8_t	crc = 0xFF;
	int		b;

	while (size--)
	{
		crc ^= *data++;
		for (b = 0; b < 8; b++)
			crc = (crc & 1) ? ((crc >> 1) ^ 0x8C) : (crc >> 1);
	}
	return (crc);
}

// Header read by the firmware to find the monitor
const uint32_t	monitor_header[3] __attribute__ ((section (".start"))) = {MONITOR_MAGIC, (uint32_t)MonitorEntry, MONITOR_VERSION};

/****************************************/
// Crash screen

extern const uint8_t	font8x8[];	// Glyphs 0x21..0x7E
__asm__(".section .rodata\n.global font8x8\nfont8x8:\n.incbin \"font8x8.bin\"\n.text\n");

static const char	*const cause_str[] = {
	"Interrupt", "TLB Modify", "TLB miss on load", "TLB miss on store", "Address error on load",
	"Address error on store", "Bus error on instruction fetch", "Bus error on data access", "System call",
	"Breakpoint instruction", "Reserved (illegal) instruction", "Coprocessor unusable", "Integer overflow", "Reserved"
};
static const char	reg_str[] = "PCSRATV0V1A0A1A2A3T0T1T2T3T4T5T6T7S0S1S2S3S4S5S6S7T8T9K0K1GPSPFPRA";

typedef struct
{
	int	x, y;
} Cursor;

static void	GpuSync(void)	// A(4Eh)
{
	register volatile int n asm("t1") = 0x4E;
	__asm__ volatile("" : "=r"(n) : "r"(n));
	((void (*)(void))0xA0)();
}

static void	DarkenScreen(void)
{
	GP1 = 0x01000000;	// clear command buffer
	GP1 = 0x04000000;	// disable DMA
	GP1 = 0x08000001 | (((GP1 & 0x100000) != 0) << 3);	// 320x240, no interlace, 15bpp, preserve video mode
	GpuSync();
	GP0 = 0xE1000400;	// 50/50 blend mode, texture don't care, drawing allowed
	GpuSync();
	GP0 = 0xE3000000;	// drawing area top-left 0,0
	GpuSync();
	GP0 = 0xE4000000 | (256 << 10) | 320;	// drawing area bottom-right
	GpuSync();
	GP0 = 0xE5000000;	// drawing offset top-left
	GpuSync();
	GP0 = 0xE6000000;	// disable masking
	GpuSync();
	GP0 = 0x63000000;	// black, semitransparent rectangle
	GP0 = 0x00000000;	// top-left
	GP0 = (256 << 16) | 320;	// full screen
}

static void	PrintChar(Cursor *c, char ch)
{
	int				row, col;
	unsigned int	bits;

	if (ch == '\n')
	{
		c->x = 0;
		c->y += 8;
		return;
	}
	if ((ch > ' ') && (ch < 0x7F))
		for (row = 0; row < 8; row++)
			for (bits = font8x8[8 * (ch - 0x21) + row], col = 7; (col >= 0) && bits; col--, bits >>= 1)
				if (bits & 1)
				{
					GpuSync();
					GP0 = 0x69FFFFFF;	// white pixel
					GP0 = ((c->y + row) << 16) | (c->x + col);
				}
	c->x += 8;
	if (c->x >= 320)
	{
		c->x = 0;
		c->y += 8;
	}
}

static void	PrintString(Cursor *c, const char *s)
{
	while (*s)
		PrintChar(c, *s++);
}

static void	PrintHex(Cursor *c, uint32_t v)
{
	int	i, d;

	for (i = 0; i < 8; i++, v <<= 4)
	{
		d = v >> 28;
		PrintChar(c, (d > 9) ? d - 10 + 'A' : d + '0');
	}
}

static void	CrashScreen(uint32_t *regs)
{
	Cursor		c = {0, 0};
	int			i, r;
	uint32_t	*sp;
	char		reg[4] = {0, 0, '=', 0};
	bool		pal;

	DarkenScreen();
	PrintString(&c, "\n CAUSE: ");
	i = (regs[MON_REG_CAUSE] >> 2) & 0xF;
	PrintString(&c, cause_str[(i < 13) ? i : 13]);

	// Registers: PC, SR, then r1..r31
	for (i = 0; i < 33; i++)
	{
		r = (i == 0) ? MON_REG_EPC : ((i == 1) ? MON_REG_SR : i - 1);
		c.x = (i <= 16) ? 8 : 8 * 14;
		c.y = 8 * ((i <= 16) ? i + 4 : i - 16 + 4);
		reg[0] = reg_str[i * 2];
		reg[1] = reg_str[i * 2 + 1];
		PrintString(&c, reg);
		PrintHex(&c, regs[r]);
	}

	// Stack dump
	c.x = 8 * 26;
	c.y = 8 * 3;
	PrintString(&c, "Stack dump:\n");
	for (sp = (uint32_t *)regs[29]; (((uint32_t)sp & ~0xE0000000) < 0x200000) && (c.y < 256); sp++)
	{
		c.x = 8 * 27;
		PrintHex(&c, *sp);
		PrintChar(&c, '\n');
	}

	// Display mode from the BIOS region ('E' = PAL), the crashed program may have left another one
	GpuSync();
	pal = (*((volatile char *)0xBFC7FF52) == 'E');
	GP1 = 0x06C60260;							// horizontal display range
	GP1 = pal ? 0x07048C23 : 0x07040010;		// vertical display range
	GP1 = pal ? 0x08000009 : 0x08000001;		// 320x240, PAL/NTSC
	GP1 = 0x05000000;							// top-left corner
	GP1 = 0x03000000;							// enable display
}

/****************************************/
// Hot reload: data streamed from USB to VRAM or SPU RAM (no RAM buffer: a packet with a bad CRC is sent again and
// written again at the same place). Same packet exchange as an upload: data, CRC from the PS1, 'N'ext/'A'gain.

#define	DMA_CHCR(_ch_)	(*((volatile uint32_t *)(0x1F801088 + (_ch_) * 16)))
#define	GPU_STAT		(*((volatile uint32_t *)0x1F801814))
#define	SPU_ADDR		(*((volatile uint16_t *)0x1F801DA6))
#define	SPU_FIFO		(*((volatile uint16_t *)0x1F801DA8))
#define	SPU_CNT			(*((volatile uint16_t *)0x1F801DAA))
#define	SPU_STAT		(*((volatile uint16_t *)0x1F801DAE))

static const uint8_t	crc8_tab[256] = {
	  0, 94,188,226, 97, 63,221,131,194,156,126, 32,163,253, 31, 65,157,195, 33,127,252,162, 64, 30, 95,  1,227,189, 62, 96,130,220,
	 35,125,159,193, 66, 28,254,160,225,191, 93,  3,128,222, 60, 98,190,224,  2, 92,223,129, 99, 61,124, 34,192,158, 29, 67,161,255,
	 70, 24,250,164, 39,121,155,197,132,218, 56,102,229,187, 89,  7,219,133,103, 57,186,228,  6, 88, 25, 71,165,251,120, 38,196,154,
	101, 59,217,135,  4, 90,184,230,167,249, 27, 69,198,152,122, 36,248,166, 68, 26,153,199, 37,123, 58,100,134,216, 91,  5,231,185,
	140,210, 48,110,237,179, 81, 15, 78, 16,242,172, 47,113,147,205, 17, 79,173,243,112, 46,204,146,211,141,111, 49,178,236, 14, 80,
	175,241, 19, 77,206,144,114, 44,109, 51,209,143, 12, 82,176,238, 50,108,142,208, 83, 13,239,177,240,174, 76, 18,145,207, 45,115,
	202,148,118, 40,171,245, 23, 73,  8, 86,180,234,105, 55,213,139, 87,  9,235,181, 54,104,138,212,149,203, 41,119,244,170, 72, 22,
	233,183, 85, 11,136,214, 52,106, 43,117,151,201, 74, 20,246,168,116, 42,200,150, 21, 75,169,247,182,232, 10, 84,215,137,107, 53
};

// End of a packet: CRC to the computer, true if it says 'N'ext (false: 'A'gain or error, *ok = false on error)
static bool	PacketEnd(uint8_t crc, bool *ok)
{
	uint8_t	ack;

	if (!USB_SendByteTimeout(crc, TIMEOUT_CNT) || !USB_GetByte(&ack))
	{
		*ok = false;
		return (false);
	}
	return (ack == 'N');
}

// VRAM read ('V', screenshot): one packet per line, read from the GPU while it is sent (monitor version 2: the kernel
// hook doesn't have it any more, the computer uses the 'M' service while the program runs)
static bool	VramRead(uint32_t xy, uint32_t wh)
{
	uint32_t	x = xy >> 16, y = xy & 0xFFFF, w = wh >> 16, h = wh & 0xFFFF, i, word = 0;
	uint8_t		crc, c;
	bool		ok = true;

	while (h && ok)
	{
		while (DMA_CHCR(2) & (1 << 24))		// GPU DMA (e.g. ordering table being drawn) finished
			;
		while (!(GPU_STAT & (1 << 26)))		// GPU ready for a command
			;
		GP0 = 0xC0000000;					// Copy rectangle VRAM to CPU
		GP0 = (y << 16) | x;
		GP0 = (1 << 16) | w;
		while (!(GPU_STAT & (1 << 27)))		// VRAM data ready
			;
		crc = 0xFF;
		for (i = 0; i < w * 2; i++)
		{
			if (!(i & 3))
				word = GP0;
			c = word;
			word >>= 8;
			crc = crc8_tab[crc ^ c];
			if (!USB_SendByteTimeout(c, TIMEOUT_CNT))
				return (false);
		}
		if (PacketEnd(crc, &ok))
		{
			y++;
			h--;
		}
	}
	return (ok);
}

static bool	VramWrite(uint32_t xy, uint32_t wh)
{
	uint32_t	x = xy >> 16, y = xy & 0xFFFF, w = wh >> 16, h = wh & 0xFFFF, n, i, count, word;
	uint8_t		crc, c;
	bool		ok = true;

	while (h && ok)
	{
		n = (h > MON_VRAM_LINES(w)) ? MON_VRAM_LINES(w) : h;
		while (DMA_CHCR(2) & (1 << 24))		// GPU DMA (e.g. ordering table being drawn) finished
			;
		while (!(GPU_STAT & (1 << 26)))		// GPU ready for a command
			;
		GP0 = 0xA0000000;					// Copy rectangle CPU to VRAM
		GP0 = (y << 16) | x;
		GP0 = (n << 16) | w;
		count = w * n * 2;
		crc = 0xFF;
		word = 0;
		for (i = 0; i < count; i++)
		{
			if (!USB_GetByte(&c))
				return (false);
			crc = crc8_tab[crc ^ c];
			word |= c << ((i & 3) * 8);
			if ((i & 3) == 3)
			{
				GP0 = word;
				word = 0;
			}
		}
		if (count & 3)						// Odd number of pixels: last halfword
			GP0 = word;
		if (PacketEnd(crc, &ok))
		{
			y += n;
			h -= n;
		}
	}
	return (ok);
}

static bool	SpuWrite(uint32_t adrs, uint32_t size)
{
	uint16_t	cnt = SPU_CNT, half = 0;
	uint32_t	n, i;
	uint8_t		crc, c;
	bool		ok = true;

	while (DMA_CHCR(4) & (1 << 24))			// SPU DMA finished
		;
	while (size && ok)
	{
		n = (size > MON_SPU_PACKET) ? MON_SPU_PACKET : size;
		SPU_CNT = cnt & ~0x30;				// Transfer mode: stop
		while (SPU_STAT & 0x30)
			;
		SPU_ADDR = adrs >> 3;
		crc = 0xFF;
		for (i = 0; i < n; i++)
		{
			if (!USB_GetByte(&c))
			{
				SPU_CNT = cnt;
				return (false);
			}
			crc = crc8_tab[crc ^ c];
			if (!(i & 1))
				half = c;
			else
			{
				SPU_FIFO = half | (c << 8);
				if (((i & 63) == 63) || (i == n - 1))	// FIFO full (32 halfwords) or end: manual write
				{
					SPU_CNT = (cnt & ~0x30) | 0x10;
					while (SPU_STAT & (1 << 10))
						;
					SPU_CNT = cnt & ~0x30;
				}
			}
		}
		if (PacketEnd(crc, &ok))
		{
			adrs += n;
			size -= n;
		}
	}
	SPU_CNT = cnt;
	return (ok);
}

/****************************************/
// Profiler ("ps1transfer -prof", see monitor.h): called by the kernel hook at each ReturnFromException while
// MonitorState.prof_on is set, regs = registers of the interrupted program (BIOS TCB)

extern bool	is_usb_busy_sending;	// Kernel hook: the program is in the middle of a request to the computer

#define	I_STAT				(*((volatile uint16_t *)0x1F801070))
#define	I_MASK				(*((volatile uint16_t *)0x1F801074))
#define	RCNT_MODE(_n_)		(*((volatile uint16_t *)(0x1F801104 + (_n_) * 16)))
#define	RCNT_TARGET(_n_)	(*((volatile uint16_t *)(0x1F801108 + (_n_) * 16)))
#define	GPU_READ			(*((volatile uint32_t *)0x1F801810))
#define	CD_STATUS			(*((volatile uint8_t *)0x1F801800))
#define	SPU_ENV(_v_)		(*((volatile uint16_t *)(0x1F801C0C + (_v_) * 16)))	// Current ADSR volume
#define	SPU_ENDX(_i_)		(*((volatile uint16_t *)(0x1F801D9C + (_i_) * 2)))
#define	DMA_BUSY(_ch_)		(DMA_CHCR(_ch_) & (1 << 24))
#define	PROF_TIMEOUT		1000	// Wait for room in the USB FIFO inside a packet (the computer reads all the time)

static void	Put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

// Not sent if the USB FIFO is full (nobody reads) or the program is sending a request to the computer
static void	ProfPacket(uint8_t type, const uint8_t *data, int size)
{
	int	i;

	if (is_usb_busy_sending || !USB_SendByteTimeout(16, 0) || !USB_SendByteTimeout(type, PROF_TIMEOUT))
		return;
	for (i = 0; i < size; i++)
		if (!USB_SendByteTimeout(data[i], PROF_TIMEOUT))
			return;
}

#define	USB_STATUS			(*((volatile uint8_t *)0x1F060000))	// Bits inverted: 1 = no (see usb.h)
#define	PROF_STATE			((MonitorState *)MONITOR_STATE_ADRS)
#define	PROF_SAVE			(PROF_AREA - 16)	// t0-t3 of the program, saved by ProfVector (end of the RAM area)
#define	PROF_BUF			(PROF_AREA - 32)	// Sample packet
#define	PROF_RAM(_f_)		((uint32_t)mon_stack + ((uint8_t *)(_f_) - __prof_start))	// Address of the RAM copy

extern uint32_t	mon_stack[];				// Kernel hook (debugmon.h): RAM area of the sampler (see monitor.h)
extern uint8_t	__prof_start[], __prof_end[];	// The sampler in the monitor (monitor.ld)
void			ProfVector(void);

// The sampler, copied to RAM by ProfSlow (running code from the flash costs about 35 cycles an instruction), in the
// kernel hook's monitor stack and register save area (only used by debug sessions: no profiling then). Only fixed
// addresses, nothing read from the flash. Hand scheduled assembly (no nop where an instruction can fill a branch or
// load delay slot) to fit in PROF_AREA: the serial / USB link tty drivers (-c sio, -c link) keep their buffers.
//
// ProfVector: the exception vector 0x80000080 jumps here while profiling. The profiler's timer interrupt is handled
// here (the BIOS interrupt handling and the kernel hook would cost about 90us at each tick): a sample is sent to the
// computer (MonitorState offsets: 40 chain80, 60 prof_on, 61 prof_seq, 62 prof_frames, 63 prof_armed, 66 prof_mode,
// 68 prof_draw). The interrupt of the counter is enabled again at each interrupt (e.g. after the program reset its
// interrupts). Other exceptions and interrupts go on to the BIOS handler (original vector code in chain80).
// Only k0/k1 are free, t0-t3 are saved at the end of the RAM area. The kernel hook calls ProfSlow (monitor entry) when
// prof_armed isn't 1 (set up, conflict, stop, SPU voices and RAM scan due).
__asm__(
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".section .prof_ram,\"ax\",@progbits\n"
	".global ProfVector\n"
	"ProfVector:\n"
	"	mfc0	$k0, $13\n"		// Cause
	"	lui		$k1, %hi(mon_state)\n"
	"	lbu		$k1, %lo(mon_state + 60)($k1)\n"		// prof_on: counter + 1
	"	andi	$k0, $k0, 0x7C\n"
	"	bnez	$k0, 9f\n"		// Not an interrupt
	"	 lui	$k0, %hi(mon_stack + " MON_XSTR(PROF_SAVE) ")\n"
	"	beqz	$k1, 9f\n"		// Profiler off
	"	 sw		$t0, %lo(mon_stack + " MON_XSTR(PROF_SAVE) ")($k0)\n"
	"	sw		$t1, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 4)($k0)\n"
	"	sw		$t2, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 8)($k0)\n"
	"	sw		$t3, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 12)($k0)\n"
	"	addiu	$k1, $k1, 3\n"		// Interrupt bit of the counter: 4 + counter
	"	li		$t0, 1\n"
	"	sllv	$t0, $t0, $k1\n"		// t0 = bit
	"	lui		$t1, 0x1F80\n"		// t1 = I/O
	"	lhu		$t2, 0x1074($t1)\n"		// I_MASK
	"	lhu		$t3, 0x1070($t1)\n"		// I_STAT
	"	or		$t2, $t2, $t0\n"
	"	sh		$t2, 0x1074($t1)\n"		// Enabled again (e.g. the program reset its interrupts)
	"	and		$t3, $t3, $t0\n"
	"	beqz	$t3, 7f\n"		// Not the profiler's tick
	"	 sll	$k0, $k1, 4\n"
	"	addu	$k0, $k0, $t1\n"
	"	lhu		$t3, 0x10C4($k0)\n"		// Counter mode, 0x1F801104 + counter * 16 = 0x1F8010C4 + (prof_on + 3) * 16 (the read clears "target reached")
	"	lui		$t2, %hi(mon_state)\n"		// t2 = MonitorState
	"	lhu		$k0, %lo(mon_state + 66)($t2)\n"		// prof_mode
	"	nor		$k1, $zero, $t0\n"		// ~bit
	"	xor		$k0, $k0, $t3\n"
	"	andi	$k0, $k0, 0x3FF\n"
	"	beqz	$k0, 1f\n"
	"	 andi	$t3, $t3, 0x800\n"		// Target reached
	"	li		$k0, 2\n"		// The program set the counter up for itself: stop (prof_armed 2, ProfSlow), its
	"	b		7f\n"		// interrupt left to the BIOS handler
	"	 sb		$k0, %lo(mon_state + 63)($t2)\n"
	"1:	beqz	$t3, 7f\n"		// (counter mode read by somebody else)
	"	 sh		$k1, 0x1070($t1)\n"		// Acknowledged
	// Hardware state: the DMA busy bits (24) moved to their PROF_F_* bit, in t3
	"	lw		$k0, 0x10A8($t1)\n"		// DMA 2 (GPU)
	"	lw		$k1, 0x10C8($t1)\n"		// DMA 4 (SPU)
	"	srl		$k0, $k0, 23\n"
	"	andi	$t3, $k0, 0x02\n"
	"	lw		$k0, 0x10B8($t1)\n"		// DMA 3 (CD-ROM)
	"	srl		$k1, $k1, 22\n"
	"	andi	$k1, $k1, 0x04\n"
	"	or		$t3, $t3, $k1\n"
	"	lw		$k1, 0x1088($t1)\n"		// DMA 0 (MDEC in)
	"	srl		$k0, $k0, 21\n"
	"	andi	$k0, $k0, 0x08\n"
	"	or		$t3, $t3, $k0\n"
	"	lw		$k0, 0x1098($t1)\n"		// DMA 1 (MDEC out)
	"	lw		$t0, 0x10E8($t1)\n"		// DMA 6 (ordering table clear)
	"	or		$k0, $k0, $k1\n"
	"	srl		$k0, $k0, 20\n"
	"	andi	$k0, $k0, 0x10\n"
	"	or		$t3, $t3, $k0\n"
	"	srl		$t0, $t0, 19\n"
	"	andi	$t0, $t0, 0x20\n"
	"	or		$t3, $t3, $t0\n"
	"	lbu		$k0, 0x1800($t1)\n"		// CD-ROM controller busy (bit 7)
	"	lw		$k1, 0x1814($t1)\n"		// GPUSTAT
	"	andi	$k0, $k0, 0x80\n"
	"	or		$t3, $t3, $k0\n"
	"	srl		$k0, $k1, 20\n"		// Bit 26 = 0: the GPU executes a command
	"	andi	$k0, $k0, 0x40\n"
	"	xori	$k0, $k0, 0x40\n"
	"	or		$t3, $t3, $k0\n"
	"	andi	$k0, $t3, 0x42\n"		// GPU busy: a command or its DMA
	"	sltu	$k0, $zero, $k0\n"
	"	or		$t3, $t3, $k0\n"
	// Frames: the draw area changes when a double buffered program swaps its buffers (GPU info, not read while the
	// GPU DMA runs or the program reads VRAM: same GPUREAD register)
	"	andi	$k0, $t3, 0x02\n"
	"	srl		$t0, $k1, 27\n"
	"	andi	$t0, $t0, 1\n"
	"	or		$k0, $k0, $t0\n"
	"	bnez	$k0, 2f\n"
	"	 lui	$k0, 0x1000\n"
	"	addiu	$k0, $k0, 3\n"
	"	sw		$k0, 0x1814($t1)\n"		// GP1(10h): draw area top left
	"	lw		$k0, 0x1810($t1)\n"		// GPUREAD
	"	lw		$k1, %lo(mon_state + 68)($t2)\n"		// prof_draw
	"	lbu		$t0, %lo(mon_state + 62)($t2)\n"		// prof_frames
	"	sll		$k0, $k0, 12\n"		// (20 bits)
	"	srl		$k0, $k0, 12\n"
	"	beq		$k0, $k1, 2f\n"
	"	 sw		$k0, %lo(mon_state + 68)($t2)\n"
	"	addiu	$t0, $t0, 1\n"
	"	sb		$t0, %lo(mon_state + 62)($t2)\n"
	// Sequence (counted also when the sample isn't sent: the computer sees the dropped ones); every 32 samples the SPU
	// voices and RAM scan asked for (prof_armed 1 -> 4, done by ProfSlow through the kernel hook)
	"2:	lbu		$k1, %lo(mon_state + 61)($t2)\n"		// prof_seq
	"	sll		$t3, $t3, 24\n"
	"	ori		$t3, $t3, 0x7010\n"		// 16, 'p'
	"	addiu	$k0, $k1, 1\n"
	"	sb		$k0, %lo(mon_state + 61)($t2)\n"
	"	sll		$k0, $k1, 16\n"
	"	or		$t3, $t3, $k0\n"
	"	andi	$k1, $k1, " MON_XSTR(PROF_VOICES_EVERY) " - 1\n"
	"	bnez	$k1, 3f\n"
	"	 lui	$k0, %hi(mon_stack + " MON_XSTR(PROF_BUF) ")\n"
	"	lbu		$k1, %lo(mon_state + 63)($t2)\n"
	"	li		$t0, 1\n"
	"	bne		$k1, $t0, 3f\n"
	"	 li		$t0, 4\n"
	"	sb		$t0, %lo(mon_state + 63)($t2)\n"
	// Packet: 16, 'p', sequence, flags / PC / RA / SP (3 bytes), frames
	"3:	sw		$t3, %lo(mon_stack + " MON_XSTR(PROF_BUF) ")($k0)\n"
	"	mfc0	$t3, $14\n"		// EPC
	"	sw		$ra, %lo(mon_stack + " MON_XSTR(PROF_BUF) " + 8)($k0)\n"
	"	sw		$t3, %lo(mon_stack + " MON_XSTR(PROF_BUF) " + 4)($k0)\n"
	"	lbu		$t3, %lo(mon_state + 62)($t2)\n"		// prof_frames
	"	sll		$t0, $sp, 8\n"
	"	srl		$t0, $t0, 8\n"
	"	sll		$t3, $t3, 24\n"
	"	or		$t0, $t0, $t3\n"
	"	sw		$t0, %lo(mon_stack + " MON_XSTR(PROF_BUF) " + 12)($k0)\n"
	// Not sent if the program is sending a request to the computer, or the USB FIFO is full (nobody reads)
	"	lui		$t3, %hi(is_usb_busy_sending)\n"
	"	lbu		$t3, %lo(is_usb_busy_sending)($t3)\n"
	"	lui		$t1, 0x1F06\n"		// USB status
	"	lbu		$k1, 0($t1)\n"
	"	addiu	$t0, $k0, %lo(mon_stack + " MON_XSTR(PROF_BUF) ")\n"		// t0 = byte to send
	"	andi	$k1, $k1, 6\n"
	"	or		$k1, $k1, $t3\n"
	"	bnez	$k1, 7f\n"
	"	 addiu	$t2, $t0, 16\n"		// t2 = end
	"	lui		$t3, 0x1F02\n"		// USB data
	"	li		$k0, " MON_XSTR(PROF_TIMEOUT) "\n"
	"5:	lbu		$k1, 0($t1)\n"		// Room in the FIFO
	"	addiu	$k0, $k0, -1\n"
	"	andi	$k1, $k1, 6\n"
	"	beqz	$k1, 6f\n"
	"	 lbu	$k1, 0($t0)\n"
	"	bnez	$k0, 5b\n"
	"	 lbu	$k1, 0($t0)\n"
	"6:	addiu	$t0, $t0, 1\n"
	"	sb		$k1, 0($t3)\n"
	"	bne		$t0, $t2, 5b\n"
	"	 li		$k0, " MON_XSTR(PROF_TIMEOUT) "\n"
	// Registers back. Other interrupts pending (or the tick not handled): the BIOS handler, otherwise back to the
	// program (a GTE command interrupted is already done: skipped, as the BIOS handler does)
	"7:	lui		$k0, 0x1F80\n"
	"	lhu		$k1, 0x1070($k0)\n"
	"	lhu		$k0, 0x1074($k0)\n"
	"	lui		$t3, %hi(mon_stack + " MON_XSTR(PROF_SAVE) ")\n"
	"	and		$k0, $k0, $k1\n"
	"	lw		$t0, %lo(mon_stack + " MON_XSTR(PROF_SAVE) ")($t3)\n"
	"	lw		$t1, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 4)($t3)\n"
	"	lw		$t2, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 8)($t3)\n"
	"	bnez	$k0, 9f\n"
	"	 lw		$t3, %lo(mon_stack + " MON_XSTR(PROF_SAVE) " + 12)($t3)\n"
	"	mfc0	$k1, $13\n"
	"	mfc0	$k0, $14\n"
	"	bltz	$k1, 8f\n"		// In a branch delay slot: back to the branch
	"	 lw		$k1, 0($k0)\n"
	"	nop\n"
	"	srl		$k1, $k1, 25\n"
	"	xori	$k1, $k1, 0x25\n"
	"	bnez	$k1, 8f\n"
	"	 nop\n"
	"	addiu	$k0, $k0, 4\n"
	"8:	jr		$k0\n"
	"	 rfe\n"
	"9:	lui		$k0, %hi(mon_state + 40)\n"		// chain80: original vector code
	"	addiu	$k0, $k0, %lo(mon_state + 40)\n"
	"	jr		$k0\n"
	"	 nop\n"
	".previous\n"
	".set pop\n"
);

static void	FlushCache(void)	// A(44h): only uses registers and cop0 (no syscall), fine in an exception
{
	register volatile int n asm("t1") = 0x44;
	__asm__ volatile("" : "=r"(n) : "r"(n));
	((void (*)(void))0xA0)();
}

// Exception vector 0x80000080: jump to the sampler, or the original code back
static void	ProfSetVector(bool on)
{
	volatile uint32_t	*v80 = (uint32_t *)0x80000080;
	uint32_t			adrs = PROF_RAM(ProfVector);
	int					i;

	if (on)
	{
		if (v80[0] != (0x3C1A0000 | ((adrs + 0x8000) >> 16)))	// Not installed yet
			for (i = 0; i < 4; i++)
				mon_state.chain80[i] = v80[i];
		v80[0] = 0x3C1A0000 | ((adrs + 0x8000) >> 16);	// lui   k0, hi
		v80[1] = 0x275A0000 | (adrs & 0xFFFF);			// addiu k0, k0, lo
		v80[2] = 0x03400008;							// jr    k0
		v80[3] = 0;										// nop
	}
	else if (v80[0] == (0x3C1A0000 | ((adrs + 0x8000) >> 16)))
		for (i = 0; i < 4; i++)
			v80[i] = mon_state.chain80[i];
	FlushCache();
}

// Called by the kernel hook (monitor entry) when prof_armed asks for it: 0 set up, 2 the program uses the counter,
// 3 stop (asked by the computer), 4 SPU voices and RAM scan
static void	ProfSlow(uint32_t *regs)
{
	int			n = mon_state.prof_on - 1, i;
	uint16_t	bit = 16 << n;
	uint32_t	v, armed = mon_state.prof_armed;
	uint8_t		p[PROF_VOICES_SIZE];

	if (!armed)		// Set up: the counter (unless the program uses its interrupt), the sampler in RAM
	{
		if (mon_state.debug)	// (its RAM is needed)
		{
			mon_state.prof_on = 0;
			return;
		}
		if (RCNT_MODE(n) & 0x30)
			armed = 2;
		else
		{
			for (i = 0; i < __prof_end - __prof_start; i += 4)
				mon_stack[i / 4] = *(uint32_t *)(__prof_start + i);
			ProfSetVector(true);
			RCNT_TARGET(n) = mon_state.prof_target;
			RCNT_MODE(n) = mon_state.prof_mode;		// (restarts the counter)
			I_MASK |= bit;
			mon_state.prof_armed = 1;
			return;
		}
	}
	if ((armed == 2) || (armed == 3))	// Stop
	{
		ProfSetVector(false);
		if (armed == 2)		// The program uses this counter
		{
			p[0] = n;
			ProfPacket(PROF_CONFLICT, p, PROF_CONFLICT_SIZE);
		}
		// Counter stopped and its interrupt masked, unless the program has set it up for itself (an interrupt nobody
		// acknowledges would stop the program)
		if ((armed == 3) || ((RCNT_MODE(n) & 0x3FF) == (mon_state.prof_mode & 0x3FF)))
		{
			RCNT_MODE(n) = 0;
			I_MASK &= ~bit;
			I_STAT = ~bit;
		}
		mon_state.prof_on = 0;
		mon_state.prof_call = mon_state.entry;
		return;
	}
	mon_state.prof_armed = 1;

	for (i = 0; i < 24; i++)
	{
		v = SPU_ENV(i);
		p[i * 2] = v;
		p[i * 2 + 1] = v >> 8;
	}
	v = SPU_ENDX(0) | (SPU_ENDX(1) << 16);
	Put32(p + 48, v);
	v = SPU_CNT;
	p[52] = v;
	p[53] = v >> 8;
	ProfPacket(PROF_VOICES, p, PROF_VOICES_SIZE);

	// RAM scan: words still holding the pattern written by the firmware before the program started
	if (mon_state.ram_hi > mon_state.ram_lo)
	{
		volatile uint32_t	*w = (volatile uint32_t *)mon_state.ram_pos;
		volatile uint32_t	*end = (volatile uint32_t *)mon_state.ram_hi;
		uint32_t			dirty = mon_state.ram_dirty, run = mon_state.ram_run, maxrun = mon_state.ram_maxrun;

		if ((w < (volatile uint32_t *)mon_state.ram_lo) || (w >= end))
			w = (volatile uint32_t *)mon_state.ram_lo;
		for (i = 0; (i < PROF_SCAN_WORDS) && (w < end); i++, w++)
		{
			if (*w == PROF_FREE_PATTERN)
				run++;
			else
			{
				dirty++;
				if (run > maxrun)
					maxrun = run;
				run = 0;
			}
		}
		if (w >= end)	// End of the pass
		{
			if (run > maxrun)
				maxrun = run;
			Put32(p, dirty);
			Put32(p + 4, maxrun);
			ProfPacket(PROF_MEMORY, p, PROF_MEMORY_SIZE);
			w = (volatile uint32_t *)mon_state.ram_lo;
			dirty = run = maxrun = 0;
		}
		mon_state.ram_pos = (uint32_t)w;
		mon_state.ram_dirty = dirty;
		mon_state.ram_run = run;
		mon_state.ram_maxrun = maxrun;
	}
}

/****************************************/
// Stopped program: commands from the computer

// Returns true when the computer resumes the program. wait: waits for the command (service while the program runs).
static bool	MonReceive(bool wait)
{
	uint8_t		cmd[1+4+4+1];
	uint32_t	adrs, size;
	int			i;
	bool		esc = false;

	if (wait ? !USB_GetByte(&cmd[0]) : !USB_GetByteNoTimeout(&cmd[0]))
		return (false);
	while (cmd[0] == 'Q')	// Escaped answer for the command that follows (see usb.h, also a 'Q' alone)
	{
		esc = USB_SendByteTimeout(16, TIMEOUT_CNT) && USB_SendByteTimeout('q', TIMEOUT_CNT);
		if (!USB_GetByte(&cmd[0]))
			return (false);
	}
	if ((cmd[0] != 'D') && (cmd[0] != 'U') && (cmd[0] != 'V') && (cmd[0] != 'R') && (cmd[0] != MON_CMD_CONTINUE) &&
		(cmd[0] != MON_CMD_VRAM_WRITE) && (cmd[0] != MON_CMD_SPU_WRITE))
		return (false);	// E.g. a pause byte sent while the program was stopping
	for (i = 1; i < sizeof(cmd); i++)
		if (!USB_GetByte(&cmd[i]))
			return (false);
	if (esc && !USB_SendByteTimeout(16, TIMEOUT_CNT))
		return (false);
	if (cmd[9] != CRC8_Compute(cmd, 9))
	{
		USB_SendByteTimeout(esc ? 'b' : 'B', TIMEOUT_CNT);	// Bad
		return (false);
	}
	if (!USB_SendByteTimeout(esc ? 'g' : 'G', TIMEOUT_CNT))	// Good
		return (false);

	adrs = (cmd[1] << 24) | (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
	size = (cmd[5] << 24) | (cmd[6] << 16) | (cmd[7] << 8) | cmd[8];
	switch (cmd[0])
	{
		case 'R':	ResetPS1();
		case 'V':	VramRead(adrs, size);										break;
		case 'D':	USB_SendData((uint8_t *)adrs, size, MAX_PACKET_SIZE);		break;
		case 'U':	USB_GetData((uint8_t *)adrs, size, MAX_PACKET_SIZE);		break;
		case MON_CMD_VRAM_WRITE:	VramWrite(adrs, size);						break;
		case MON_CMD_SPU_WRITE:		SpuWrite(adrs, size);						break;
		case MON_CMD_CONTINUE:	return (true);
	}
	return (false);
}

static uint32_t	ReadDCIC(void)
{
	uint32_t	v;

	__asm__ volatile("mfc0 %0, $7" : "=r"(v));
	return (v);
}

// COP0 debug registers set by the computer (hardware breakpoint/watchpoint), DCIC last
static void	SetCop0Debug(void)
{
	__asm__ volatile(
		"mtc0 %0, $3\n"		// BPC
		"mtc0 %1, $11\n"	// BPCM
		"mtc0 %2, $5\n"		// BDA
		"mtc0 %3, $9\n"		// BDAM
		"mtc0 %4, $7\n"		// DCIC
		: : "r"(mon_state.bpc), "r"(mon_state.bpcm), "r"(mon_state.bda), "r"(mon_state.bdam), "r"(mon_state.dcic));
}

void	MonitorEntry(int reason, uint32_t *regs)
{
	uint32_t	epc;
	bool		debug = (TTY_MODE & EXEC_DEBUG) != 0;
	uint32_t	ret;

	if (reason == MON_REASON_PROFILE)	// (first: called at each exception while profiling)
	{
		ProfSlow(regs);
		return;
	}

	// Service ('M' byte while the program runs): one command, e.g. a hot reload VRAM/SPU write, then back to the program
	if (reason == MON_REASON_SERVICE)
	{
		MonReceive(true);
		return;
	}

	epc = regs[MON_REG_EPC] + ((regs[MON_REG_CAUSE] & 0x80000000) ? 4 : 0);	// Branch delay slot

	// "break 1024" (pollhost) only keeps a monitor serviced: skipped
	if ((reason == MON_REASON_BREAK) && (((epc & ~0xE0000000) < 0x200000) && (*((uint32_t *)epc) == POLLHOST_BREAK)))
	{
		regs[MON_REG_EPC] += 4;
		SetCop0Debug();
		return;
	}
	if ((reason == MON_REASON_PAUSE) || (reason == MON_REASON_CRASH))	// Not stopped by the vector code
	{
		mon_state.dcic_stop = ReadDCIC() & 0x3F;	// Status bits: which breakpoint triggered
		__asm__ volatile("mtc0 $0, $7");
	}

	if ((reason == MON_REASON_CRASH) && !debug)
		CrashScreen(regs);

	mon_state.regs = (uint32_t)regs;
	mon_state.reason = reason;
	// Tell ps1transfer (debug session): it reads the registers, its answer doesn't matter
	if (debug)
		USB_Process(MON_NOTIFY_STOP, (uint32_t)regs, NULL, (reason << 8) | ((regs[MON_REG_CAUSE] >> 2) & 31), &ret);

	while (1)
	{
		volatile uint32_t	gpu = GP1;	// Some emulators only refresh the display if the GPU is poked

		(void)gpu;
		if (MonReceive(false))
			break;
	}

	mon_state.regs = 0;
	mon_state.reason = MON_REASON_NONE;
	// Breakpoints written in the program code: FlushCache() A(44h), only uses registers and cop0 (no syscall)
	{
		register volatile int n asm("t1") = 0x44;
		__asm__ volatile("" : "=r"(n) : "r"(n));
		((void (*)(void))0xA0)();
	}
	SetCop0Debug();
}
