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
#include "common/psxlibc/device.h"		// (nugget: the BIOS File and Device structures)
#include "../kernel_hook/siotty.h"

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
extern SioBuffers	sio_buf;				// Serial port / USB link tty driver: its buffers and state (kernel_hook/siotty.h)
extern SioState		sio_state;
extern bool			is_sio_tty_enabled;
void	ReplaceTTY(struct Device *dev);
int		USB_tty_dummy(void);
_Static_assert(offsetof(MonitorState, pad_buf) == MON_STATE_PAD_BUF, "MON_STATE_PAD_BUF");
_Static_assert(offsetof(MonitorState, pad_orig) == MON_STATE_PAD_ORIG, "MON_STATE_PAD_ORIG");
_Static_assert(offsetof(MonitorState, prof_draw) == MON_STATE_PAD_BUF2, "MON_STATE_PAD_BUF2 (pad_buf2 reuses prof_draw)");
_Static_assert(sizeof(MonitorState) <= 0x70, "MonitorState: bigger than its place in the kernel hook RAM");
_Static_assert(offsetof(MonitorState, ram_lo) == MON_STATE_PAD_DEVICE, "MON_STATE_PAD_DEVICE");
_Static_assert(sizeof(PadDevice) <= offsetof(MonitorState, prof_call) - offsetof(MonitorState, ram_lo), "PadDevice");

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
	"INTERRUPT", "TLB MODIFY", "TLB MISS ON LOAD", "TLB MISS ON STORE", "ADDRESS ERROR ON LOAD",
	"ADDRESS ERROR ON STORE", "BUS ERROR ON INSTRUCTION FETCH", "BUS ERROR ON DATA ACCESS", "SYSTEM CALL",
	"BREAKPOINT INSTRUCTION", "RESERVED (ILLEGAL) INSTRUCTION", "COPROCESSOR UNUSABLE", "INTEGER OVERFLOW", "RESERVED"
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

// Look of the PS1 tools (devcart_ui.h): colors as GP0 commands want them (0xBBGGRR)
#define	CRASH_RGB(_r_, _g_, _b_)	(((_b_) << 16) | ((_g_) << 8) | (_r_))
#define	CRASH_BG		CRASH_RGB(14, 16, 28)
#define	CRASH_BAR		CRASH_RGB(40, 50, 120)
#define	CRASH_PANEL		CRASH_RGB(20, 24, 40)
#define	CRASH_W			320

static void	Rect(int x, int y, int w, int h, uint32_t color)
{
	GpuSync();
	GP0 = 0x60000000 | color;	// Opaque rectangle (drawing offset: the crash screen's top-left)
	GP0 = (y << 16) | x;
	GP0 = (h << 16) | w;
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

// The crash screen is drawn in the program's draw area: the buffer it was drawing when it crashed, so the frame it
// displayed last stays in VRAM (double buffered programs) and the crash report can save it. Where it was, and the
// program's display mode, are kept in MonitorState.prof_draw (MON_CRASH_FRAME, see monitor.h).
static void	CrashScreen(uint32_t *regs)
{
	Cursor		c;
	int			i, r, x, y, h, bottom;
	uint32_t	*sp, mode, area;
	char		reg[4] = {0, 0, '=', 0};
	bool		pal = (*((volatile char *)0xBFC7FF52) == 'E');	// BIOS region ('E' = PAL)

	mode = (GP1 >> 16) & 0x7F;	// GPUSTAT bits 16-22: the program's display mode
	GP1 = 0x01000000;	// clear command buffer
	GP1 = 0x04000000;	// disable DMA
	GpuSync();
	GP1 = 0x10000003;	// GPU info: draw area top-left
	area = GP0 & 0xFFFFF;
	mon_state.prof_draw = MON_CRASH_FRAME | (mode << 20) | area;

	h = pal ? 256 : 240;
	x = area & 0x3FF;
	y = (area >> 10) & 0x1FF;
	if (x > 1024 - CRASH_W)
		x = 1024 - CRASH_W;
	if (y > 512 - h)
		y = 512 - h;
	GpuSync();
	GP0 = 0xE1000400;	// drawing to the displayed area allowed
	GpuSync();
	GP0 = 0xE3000000 | (y << 10) | x;	// drawing area
	GpuSync();
	GP0 = 0xE4000000 | ((y + h - 1) << 10) | (x + CRASH_W - 1);
	GpuSync();
	GP0 = 0xE5000000 | (y << 11) | x;	// drawing offset: coordinates below from the crash screen's top-left
	GpuSync();
	GP0 = 0xE6000000;	// disable masking

	// Background, title bar, cause panel, registers and stack panels, help bar
	bottom = h - 14 - 6;
	Rect(0, 0, CRASH_W, h, CRASH_BG);
	Rect(0, 0, CRASH_W, 14, CRASH_BAR);
	Rect(6, 20, 308, 20, CRASH_PANEL);
	Rect(6, 46, 200, bottom - 46, CRASH_PANEL);
	Rect(212, 46, 102, bottom - 46, CRASH_PANEL);
	Rect(0, h - 14, CRASH_W, 14, CRASH_PANEL);
	c.x = 8;
	c.y = 4;
	PrintString(&c, "PS1 DEV CART CRASH SCREEN");
	c.x = 8;
	c.y = h - 11;
	PrintString(&c, "CRASH REPORT: DEBUG PAGE OR -CRASH");

	c.x = 14;
	c.y = 26;
	PrintString(&c, "CAUSE: ");
	i = (regs[MON_REG_CAUSE] >> 2) & 0xF;
	PrintString(&c, cause_str[(i < 13) ? i : 13]);

	// Registers: PC, SR, then r1..r31
	for (i = 0; i < 33; i++)
	{
		r = (i == 0) ? MON_REG_EPC : ((i == 1) ? MON_REG_SR : i - 1);
		c.x = (i <= 16) ? 14 : 110;
		c.y = 52 + 8 * ((i <= 16) ? i : i - 17);
		reg[0] = reg_str[i * 2];
		reg[1] = reg_str[i * 2 + 1];
		PrintString(&c, reg);
		PrintHex(&c, regs[r]);
	}

	// Stack dump
	c.x = 220;
	c.y = 52;
	PrintString(&c, "STACK");
	c.y = 64;
	for (sp = (uint32_t *)regs[29]; (((uint32_t)sp & ~0xE0000000) < 0x200000) && (c.y <= bottom - 14); sp++)
	{
		c.x = 228;
		PrintHex(&c, *sp);
		c.y += 8;
	}

	// Display: the crash screen, 320x240 (PAL: 256 lines), the crashed program may have left another mode
	GpuSync();
	GP1 = 0x05000000 | (y << 10) | x;			// display start
	GP1 = 0x06C60260;							// horizontal display range
	GP1 = pal ? 0x07048C23 : 0x07040010;		// vertical display range
	GP1 = pal ? 0x08000009 : 0x08000001;		// 320x240, PAL/NTSC
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

// The GPU's DMA finished, then a GPUSTAT bit set (26: ready for a command, 27: VRAM data ready), polled a limited
// number of times (about a second from the flash): a GPU that never gets ready (e.g. a program's bad primitive) makes
// the command fail (the computer sees an error), instead of hanging the console with its interrupts off
#define	GPU_WAIT		400000

static bool	GpuReady(uint32_t stat_bit)
{
	uint32_t	n;

	for (n = 0; DMA_CHCR(2) & (1 << 24); n++)
		if (n > GPU_WAIT)
			return (false);
	for (n = 0; !(GPU_STAT & stat_bit); n++)
		if (n > GPU_WAIT)
			return (false);
	return (true);
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
		if (!GpuReady(1 << 26))				// GPU DMA (e.g. ordering table being drawn) finished, ready
			return (false);
		GP0 = 0xC0000000;					// Copy rectangle VRAM to CPU
		GP0 = (y << 16) | x;
		GP0 = (1 << 16) | w;
		if (!GpuReady(1 << 27))				// VRAM data ready
			return (false);
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
		if (!GpuReady(1 << 26))				// GPU DMA (e.g. ordering table being drawn) finished, ready
			return (false);
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

#define	DMA_MADR(_ch_)	(*((volatile uint32_t *)(0x1F801080 + (_ch_) * 16)))
#define	DMA_BCR(_ch_)	(*((volatile uint32_t *)(0x1F801084 + (_ch_) * 16)))
#define	DMA_DPCR		(*((volatile uint32_t *)0x1F8010F0))
#define	DMA_DICR		(*((volatile uint32_t *)0x1F8010F4))
#define	SPU_DTC			(*((volatile uint16_t *)0x1F801DAC))
#define	SPU_REGS		((volatile uint16_t *)0x1F801C00)
#define	SPU_CHUNK		56		// Bytes kept per DMA of one 16-word block (the SPU FIFO): each block read gives first a
#define	SPU_STALE		2		// stale halfword, then 31 of the address set; 56: the next one on an 8-byte address
#define	DICR_SPU_IRQ	(1 << 20)
#define	DICR_SPU_FLAG	(1 << 28)

extern uint32_t	mon_stack[];	// Kernel hook: the monitor's stack (its bottom is the SPU DMA buffer, see SpuRead)

static void	SpuMode(uint16_t cnt, uint16_t mode)
{
	SPU_CNT = (cnt & ~0x30) | mode;
	while ((SPU_STAT & 0x30) != mode)
		;
}

// SPU RAM read (MON_CMD_SPU_READ): the SPU has no manual read, only DMA, done by chunks of 56 bytes (a 16-word block
// read, its first halfword stale) into the bottom 64 bytes of the kernel hook's monitor stack (free: the 'M' service doesn't run on it, the loop of a stopped program uses its
// top 230 bytes; the profiler's sampler there doesn't run with a USB command). The program's SPU and DMA settings are
// restored, its DMA interrupt isn't triggered. adrs MON_SPU_REGS: the SPU registers instead (16-bit reads).
static bool	SpuRead(uint32_t adrs, uint32_t size)
{
	volatile uint8_t	*buf = (volatile uint8_t *)mon_stack, *data = buf;
	uint16_t			cnt = SPU_CNT, dtc = SPU_DTC, start = SPU_ADDR, v;
	uint32_t			dpcr = DMA_DPCR, dicr = DMA_DICR, n, pos, i;
	uint8_t				crc;
	bool				regs = (adrs == MON_SPU_REGS), ok = true;

	if (!regs)
	{
		while (DMA_CHCR(4) & (1 << 24))		// The program's SPU DMA finished
			;
		while (SPU_STAT & (1 << 10))
			;
		DMA_DICR = (dicr & 0x00FF7FFF) & ~DICR_SPU_IRQ;	// (flags kept: 0 written)
		DMA_DPCR = dpcr | (8 << 16) | ((dpcr & (8 << 16)) ? 0 : (3 << 16));	// Channel 4 on
		SPU_DTC = 4;
		data = buf + SPU_STALE;
	}
	for (pos = 0; (pos < size) && ok; )
	{
		n = ((size - pos) > MON_SPU_PACKET) ? MON_SPU_PACKET : (size - pos);
		crc = 0xFF;
		for (i = 0; i < n; i++)
		{
			if (!(i % SPU_CHUNK))
			{
				if (regs)
				{
					for (v = 0; v < SPU_CHUNK / 2; v++)		// (0x1F801DA8: the transfer FIFO, not read)
					{
						uint32_t	r = (pos + i) / 2 + v;
						uint16_t	value = ((r == (0x1DA8 - 0x1C00) / 2) || (r >= MON_SPU_REGS_SIZE / 2)) ? 0 : SPU_REGS[r];

						buf[v * 2] = value;
						buf[v * 2 + 1] = value >> 8;
					}
				}
				else
				{
					SpuMode(cnt, 0);
					SPU_ADDR = (adrs + pos + i) >> 3;
					SpuMode(cnt, 0x30);				// DMA read
					DMA_MADR(4) = (uint32_t)buf & 0xFFFFFF;
					DMA_BCR(4) = (1 << 16) | 16;
					DMA_CHCR(4) = 0x01000200;		// SPU to RAM, by blocks, start
					while (DMA_CHCR(4) & (1 << 24))
						;
				}
			}
			crc = crc8_tab[crc ^ data[i % SPU_CHUNK]];
			if (!USB_SendByteTimeout(data[i % SPU_CHUNK], TIMEOUT_CNT))
			{
				ok = false;
				break;
			}
		}
		if (ok && PacketEnd(crc, &ok))
			pos += n;
	}
	if (!regs)
	{
		SpuMode(cnt, 0);
		SPU_ADDR = start;
		SPU_DTC = dtc;
		SPU_CNT = cnt;
		DMA_DPCR = dpcr;
		DMA_DICR = (dicr & 0x00FFFFFF) | ((dicr & DICR_SPU_FLAG) ? 0 : DICR_SPU_FLAG);	// Our DMA's flag cleared
	}
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

// MON_CMD_GPU_INFO: the drawing state of the GPU (GP1 10h: the answer in GPUREAD), for the frame capture
static void	GpuInfo(void)
{
	static const uint8_t	which[4] = {3, 4, 5, 7};	// Draw area top left, bottom right, offset, version
	uint32_t				info[4];
	int						i;

	for (i = 0; i < 4; i++)
	{
		GP1 = 0x10000000 | which[i];
		info[i] = GP0;		// (GPUREAD)
	}
	USB_SendData((uint8_t *)info, sizeof(info), MAX_PACKET_SIZE);
}

/****************************************/
// Serial port (SIO) / USB link TTY driver ("ps1transfer -c sio" / "-c link"), see kernel_hook/siotty.h: moved here
// from the kernel hook (room in the kernel area). Its state (sio_state) and buffers (sio_buf) are in the kernel hook
// RAM; the BIOS calls these functions (its tty device) from the program, the kernel hook at each exception.

#define	SIO_DATA	*((volatile uint8_t *)0x1F801050)
#define	SIO_STAT	*((volatile uint16_t *)0x1F801054)
#define	SIO_MODE	*((volatile uint16_t *)0x1F801058)
#define	SIO_CTRL	*((volatile uint16_t *)0x1F80105A)
#define	SIO_BAUD	*((volatile uint16_t *)0x1F80105E)

#define	SIO_STAT_TX_READY	(1 << 0)
#define	SIO_STAT_RX_READY	(1 << 1)
#define	SIO_STAT_RX_ERRORS	((1 << 3) | (1 << 4) | (1 << 5))	// Parity, overrun, framing
#define	SIO_STAT_DSR		(1 << 7)
#define	SIO_STAT_CTS		(1 << 8)

#define	SIO_CTRL_TX_ENABLE	(1 << 0)
#define	SIO_CTRL_DTR		(1 << 1)
#define	SIO_CTRL_RX_ENABLE	(1 << 2)
#define	SIO_CTRL_ACK		(1 << 4)
#define	SIO_CTRL_RTS		(1 << 5)	// The other console can only send when its CTS (our RTS) is on
#define	SIO_CTRL_RESET		(1 << 6)

#define	SIO_MODE_STOP_MASK	(3 << 6)	// 1:1 2:1.5 3:2 stop bits
#define	SIO_MODE_PARITY_MASK	(3 << 4)	// bit 4: enable, bit 5: odd
#define	SIO_MODE_CHAR_MASK	(3 << 2)	// 0:5 1:6 2:7 3:8 bits
#define	SIO_MODE_DEFAULT	((1 << 6) | (3 << 2) | 2)	// 8N1, baud rate factor x16

#define	SIO_CLOCK			(33868800 / 16)
#define	GPU_STAT_ODD_LINE	(1 << 31)	// 0 during the vertical blank (and on even lines)
#define	SIO_TX_TIMEOUT		3000	// Avoid freezing printf when nothing is connected (CTS off): about 0.1 s (the loop runs from
								// flash, ~35x slower than the 100000 of the driver in RAM)
#define	USB_STATUS_MASK_USB_READY	(1 << 2)

// ioctl commands, same values as Net Yaroze and PsyQ <sys/ioctl.h>
#define	FIOCNBLOCK	(('f' << 8) | 1)	// set non-blocking io
#define	FIOCSCAN	(('f' << 8) | 2)	// scan for input
#define	TIOCRAW		(('t' << 8) | 1)	// disable xon/xoff control (and '\n' -> "\r\n" here)
#define	TIOCFLUSH	(('t' << 8) | 2)	// flush input buffer
#define	TIOCREOPEN	(('t' << 8) | 3)	// reopen
#define	TIOCBAUD	(('t' << 8) | 4)	// set baud rate
#define	TIOCDTR		(('t' << 8) | 6)	// control DTR line
#define	TIOCRTS		(('t' << 8) | 7)	// control RTS line
#define	TIOCLEN		(('t' << 8) | 8)	// stop << 16 | char
#define	TIOCPARITY	(('t' << 8) | 9)	// parity 0:none 1:even 3:odd
#define	TIOSTATUS	(('t' << 8) | 10)	// return status
#define	TIOERRRST	(('t' << 8) | 11)	// error reset
#define	TIOEXIST	(('t' << 8) | 12)	// exist test with DSR/CTS
#define	TIORLEN		(('t' << 8) | 13)	// receive buffer length

#define	SIO_EINVAL	22

#define	SIO_rx_buffer		sio_buf.rx_buffer
#define	SIO_rx_head			sio_buf.rx_head		// Head is only written by SIO_Poll (or the computer), tail by the reader
#define	SIO_rx_tail			sio_buf.rx_tail
#define	SIO_RX_HEAD			(sio_buf.rx_head & (SIO_RX_BUFFER_SIZE - 1))	// Read masked: a bad value from the computer
																		// (e.g. a transfer cut off) can't loop read()

static void	SIO_Delay(void)
{
	volatile int	i;

	for (i = 0; i < 1500; i++)
		;
}

// Reset the serial port with the current settings (received data are lost)
static __attribute__((noinline)) int	SIO_Init(void)
{
	if (sio_state.link)	// USB link: the computer writes rx_head, received data are dropped
	{
		SIO_rx_tail = SIO_RX_HEAD;
		sio_buf.rx_unit_tail = sio_buf.rx_unit_head & (LINK_UNITS - 1);
		sio_state.link_allow = 0;
		return (0);
	}
	SIO_CTRL = SIO_CTRL_RESET | SIO_CTRL_ACK;
	SIO_Delay();
	SIO_BAUD = SIO_CLOCK / sio_state.baud;
	SIO_MODE = sio_state.mode;
	SIO_CTRL = sio_state.ctrl;
	SIO_rx_head = SIO_rx_tail = 0;
	return (0);
}

// Move received bytes from the 8 bytes hardware FIFO to the receive buffer
static __attribute__((noinline)) void	SIO_Poll(void)
{
	uint16_t	stat;
	uint32_t	next;
	uint8_t		c;

	if (sio_state.link)
		return;
	while ((stat = SIO_STAT) & (SIO_STAT_RX_READY | SIO_STAT_RX_ERRORS))
	{
		if (stat & SIO_STAT_RX_READY)
		{
			c = SIO_DATA;
			next = (SIO_rx_head + 1) & (SIO_RX_BUFFER_SIZE - 1);
			if (next != SIO_rx_tail)	// Byte is dropped if the buffer is full
			{
				SIO_rx_buffer[SIO_rx_head] = c;
				SIO_rx_head = next;
			}
		}
		if (stat & SIO_STAT_RX_ERRORS)
			SIO_CTRL = sio_state.ctrl | SIO_CTRL_ACK;
	}
}

static uint32_t	SIO_RxLength(void)
{
	uint32_t	n = (SIO_RX_HEAD - SIO_rx_tail) & (SIO_RX_BUFFER_SIZE - 1);

	return ((sio_state.link && (n > sio_state.link_allow)) ? sio_state.link_allow : n);
}

// USB link: the open unit is closed if the unit ring has room (else it grows with the next writes)
static void	LINK_CloseUnit(void)
{
	uint32_t	next = (sio_buf.tx_unit_head + 1) & (LINK_UNITS - 1);

	if (sio_state.link_open && (next != (sio_buf.tx_unit_tail & (LINK_UNITS - 1))))
	{
		sio_buf.tx_unit_head = next;
		sio_state.link_open = false;
	}
}

// USB link, at each run of the kernel hook: frames counted at the vertical blanks
static void	LINK_Tick(void)
{
	if (!(GPU_STAT & GPU_STAT_ODD_LINE))
		sio_state.link_frame++;
	LINK_CloseUnit();
}

// USB link, before a read: one more unit for the program in a new frame (more if the backlog grows)
static void	LINK_Release(void)
{
	uint32_t	t = sio_buf.rx_unit_tail;

	if (sio_state.link_released == sio_state.link_frame)
		return;
	while (t != (sio_buf.rx_unit_head & (LINK_UNITS - 1)))
	{
		sio_state.link_allow += sio_buf.rx_units[t];
		t = (t + 1) & (LINK_UNITS - 1);
		sio_state.link_released = sio_state.link_frame;
		if (((sio_buf.rx_unit_head - t) & (LINK_UNITS - 1)) <= 1)
			break;
	}
	sio_buf.rx_unit_tail = t;
}

static __attribute__((noinline)) bool	SIO_SendByte(uint8_t c)
{
	uint32_t	timeout = SIO_TX_TIMEOUT, next;

	if (sio_state.link)	// USB link: byte dropped if the computer doesn't keep up
	{
		next = (sio_buf.tx_head + 1) & (LINK_TX_BUFFER_SIZE - 1);
		if (next == (sio_buf.tx_tail & (LINK_TX_BUFFER_SIZE - 1)))
			return (false);
		sio_buf.tx_buffer[sio_buf.tx_head] = c;
		sio_buf.tx_head = next;
		return (true);
	}
	while (!(SIO_STAT & SIO_STAT_TX_READY))
	{
		SIO_Poll();
		if (!(timeout--))
			return (false);
	}
	SIO_DATA = c;
	return (true);
}

// USB link: the kernel hook answers the computer only at some interrupts (about once per frame), so it also answers
// at each read/write of the program (with interrupts off, like in the exception handler): lower latency
static __attribute__((noinline)) void	LINK_Serve(void)
{
	uint32_t	sr;

	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr & ~1));
	if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
		USB_Process(0, 0, NULL, 0, NULL);
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr));
}

static int	SIO_tty_open(struct File *file, const char *filename, int mode)
{
	return (0);
}

static int	SIO_tty_action(struct File *file, enum FileAction action)
{
	volatile uint8_t	*buf8 = (volatile uint8_t *)file->buffer;
	uint32_t			i = 0, tx_start = sio_buf.tx_head;
	bool				serve = sio_state.link;

	// USB link: the computer is answered when a read finds no more bytes (bytes arriving now are read at once; a
	// blocking read waits for them this way) and after a write (bytes written now leave at once)
	sio_state.busy = true;
	if (action == PSXWRITE)
	{
		for (; i < file->count; i++)
		{
			if (!sio_state.raw && (buf8[i] == '\n') && !SIO_SendByte('\r'))
				break;
			if (!SIO_SendByte(buf8[i]))
				break;
		}
	}
	else
	{
		while (i < file->count)
		{
			SIO_Poll();
			if ((SIO_rx_tail != SIO_RX_HEAD) && (!sio_state.link || sio_state.link_allow))
			{
				buf8[i++] = SIO_rx_buffer[SIO_rx_tail];
				SIO_rx_tail = (SIO_rx_tail + 1) & (SIO_RX_BUFFER_SIZE - 1);
				if (sio_state.link)
					sio_state.link_allow--;
			}
			else if (serve)		// (once for a non-blocking read, until it has its bytes for a blocking one)
			{
				LINK_Serve();
				LINK_Release();
				serve = !(file->flags & PSXF_NBLOCK);
			}
			else if (file->flags & PSXF_NBLOCK)
				break;
		}
	}
	sio_state.busy = false;
	if (sio_state.link && (action == PSXWRITE))
	{
		// The bytes written are a unit, closed at once (or at the next vertical blank if the unit ring is full).
		// Units are given by their end position: the bytes put in the buffer (a '\r' is added before each '\n' when
		// not raw) always match.
		if (sio_buf.tx_head != tx_start)
		{
			sio_buf.tx_units[sio_buf.tx_unit_head] = sio_buf.tx_head;
			sio_state.link_open = true;
			LINK_CloseUnit();
		}
		LINK_Serve();
	}

	return (i);
}

static int	SIO_tty_ioctl(struct File *file, int cmd, int arg)
{
	int	ret = 0, len;

	sio_state.busy = true;
	switch (cmd)
	{
		case FIOCNBLOCK:
			if (arg)
				file->flags |= PSXF_NBLOCK;
			else
				file->flags &= ~PSXF_NBLOCK;
		break;

		case FIOCSCAN:
		case TIORLEN:
			SIO_Poll();
			ret = SIO_RxLength();
		break;

		case TIOCRAW:
			sio_state.raw = (arg != 0);
		break;

		case TIOCFLUSH:
			SIO_Poll();
			SIO_rx_tail = SIO_RX_HEAD;
		break;

		case TIOCREOPEN:
			SIO_Init();
		break;

		case TIOCBAUD:
			if (arg <= 0)
				goto error;
			sio_state.baud = arg;
			SIO_Init();
		break;

		case TIOCDTR:
		case TIOCRTS:
			if (arg)
				sio_state.ctrl |= (cmd == TIOCDTR) ? SIO_CTRL_DTR : SIO_CTRL_RTS;
			else
				sio_state.ctrl &= ~((cmd == TIOCDTR) ? SIO_CTRL_DTR : SIO_CTRL_RTS);
			SIO_CTRL = sio_state.ctrl;
		break;

		case TIOCLEN:	// Char length is accepted as 0..3 (documented) or 5..8 (number of bits)
			len = arg & 0xF;
			if (len >= 5)
				len -= 5;
			if (len > 3)
				goto error;
			sio_state.mode &= ~(SIO_MODE_STOP_MASK | SIO_MODE_CHAR_MASK);
			sio_state.mode |= (len << 2) | ((((arg >> 16) & 3) ? ((arg >> 16) & 3) : 1) << 6);
			SIO_Init();
		break;

		case TIOCPARITY:
			sio_state.mode = (sio_state.mode & ~SIO_MODE_PARITY_MASK) | ((arg & 3) << 4);
			SIO_Init();
		break;

		case TIOSTATUS:
			ret = SIO_STAT;
		break;

		case TIOERRRST:
			SIO_CTRL = sio_state.ctrl | SIO_CTRL_ACK;
		break;

		case TIOEXIST:
			ret = (SIO_STAT & (SIO_STAT_DSR | SIO_STAT_CTS)) != 0;
		break;

		default:
			goto error;
	}
	sio_state.busy = false;
	return (ret);

error:
	sio_state.busy = false;
	file->errno = SIO_EINVAL;
	return (-1);
}

// The tty device (the BIOS copies it in its device table: it can be in flash)
static const struct Device	SIO_tty_dev =
{
	"tty",
	PSXDTTYPE_CHAR | PSXDTTYPE_CONS,
	0,
	"PS1 DEV CART SIO tty",
	(device_init)SIO_Init,
	SIO_tty_open,
	SIO_tty_action,
	(device_close)USB_tty_dummy,
	SIO_tty_ioctl,
	(device_read)USB_tty_dummy,
	(device_write)USB_tty_dummy,
	(void *)USB_tty_dummy,
	(void *)USB_tty_dummy,
	(device_firstFile)USB_tty_dummy,
	(device_nextFile)USB_tty_dummy,
	(device_format)USB_tty_dummy,
	(void *)USB_tty_dummy,
	(void *)USB_tty_dummy,
	(device_deinit)USB_tty_dummy,
	(void *)USB_tty_dummy
};

// MON_REASON_SIO_SETUP: the BIOS TTY device replaced by this driver (serial port, or USB link)
static void	SioSetup(bool link)
{
	sio_state.link = link;
	sio_state.baud = SIO_BAUD_DEFAULT;
	sio_state.mode = SIO_MODE_DEFAULT;
	sio_state.ctrl = SIO_CTRL_TX_ENABLE | SIO_CTRL_RX_ENABLE | SIO_CTRL_RTS;
	SIO_Init();
	ReplaceTTY((struct Device *)&SIO_tty_dev);
	is_sio_tty_enabled = true;
}

// MON_REASON_SIO_UPDATE, at each exception: frames counted (USB link), the receive FIFO emptied
static void	SioUpdate(void)
{
	if (sio_state.link)
		LINK_Tick();
	if (!sio_state.busy)
		SIO_Poll();
}

/****************************************/
// Remote pad (EXEC_PAD, see monitor.h): InitPAD of the program goes through PadInitHook (in flash), which keeps the
// buffers of both ports and goes on to the BIOS InitPAD

void	PadInitHook(void);
__asm__(
	".set push\n"
	".set noreorder\n"
	".text\n"
	".global PadInitHook\n"
	"PadInitHook:\n"
	"	lui		$t0, %hi(mon_state)\n"
	"	addiu	$t0, $t0, %lo(mon_state)\n"
	"	sw		$a0, " MON_XSTR(MON_STATE_PAD_BUF) "($t0)\n"	// pad_buf: port 1
	"	sw		$a2, " MON_XSTR(MON_STATE_PAD_BUF2) "($t0)\n"	// pad_buf2: port 2 (reuses prof_draw)
	"	lw		$t0, " MON_XSTR(MON_STATE_PAD_ORIG) "($t0)\n"	// pad_orig: the BIOS InitPAD
	"	nop\n"
	"	jr		$t0\n"
	"	nop\n"
	".set pop\n"
);

static void	**B0Table(void)
{
	register volatile int n asm("t1") = 0x57;
	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((void **(*)(void))0xB0)());
}

// Before the program starts (firmware): InitPAD through PadInitHook, no button from the computer yet, port 1 driven
static void	PadSetup(void)
{
	void		**b0 = B0Table();
	uint32_t	n;

	if (b0[0x12] != (void *)PadInitHook)
		mon_state.pad_orig = (uint32_t)b0[0x12];
	if (!mon_state.pad_orig)	// (the BIOS one is unknown: the program reads its pad as usual)
		return;
	b0[0x12] = (void *)PadInitHook;
	mon_state.pad_buf = 0;
	mon_state.prof_draw = 0;	// pad_buf2 (port 2), reused: not called yet
	mon_state.prof_frames = 0;	// pad_port (reused): port 1 by default
	for (n = 0; n < sizeof(PadDevice); n++)	// PadDevice (reused): a digital pad
		((uint8_t *)&mon_state.ram_lo)[n] = 0;
	mon_state.prof_seq = 0;
	mon_state.prof_target = 0;
	mon_state.prof_armed = PROF_ARMED_PAD;
	mon_state.prof_on = 0xFF;	// (the kernel hook calls the monitor at each exception)
}

// At each exception: the buttons pressed from the computer added to the buffer of the port selected (pad_port, reuses
// prof_frames) the BIOS fills (at the vertical blank); only one port is driven, the other keeps a real controller
static int	PadClamp(int v)
{
	return ((v < -128) ? -128 : ((v > 127) ? 127 : v));
}

static void	PadOverride(void)
{
	volatile uint8_t	*buf = (volatile uint8_t *)(mon_state.prof_frames ? mon_state.prof_draw : mon_state.pad_buf);
	PadDevice			*dev = (PadDevice *)&mon_state.ram_lo;
	uint16_t			bits = mon_state.prof_target;
	uint8_t				type = dev->type ? dev->type : PAD_DIGITAL;
	bool				fresh;
	int					i, dx, dy;

	if (!buf || !(mon_state.prof_seq & 1))
		return;
	fresh = buf[0] || (buf[1] != type);		// Filled by the BIOS (a new frame) since our reply, or no device
	if (type == PAD_DIGITAL)
	{
		if (fresh)							// No pad (or not a digital one): a digital pad, nothing pressed
		{
			buf[0] = 0;
			buf[1] = PAD_DIGITAL;
			buf[2] = buf[3] = 0xFF;
		}
		buf[2] &= ~(bits >> 8);				// Buttons active low: PsyQ bits = ~(buf[2] << 8 | buf[3])
		buf[3] &= ~bits;
		return;
	}
	buf[0] = 0;								// Another device: the whole reply
	buf[1] = type;
	buf[2] = ~(bits >> 8);
	buf[3] = ~bits;
	if (type == PAD_MOUSE)
	{
		if (fresh)							// The motion since the last frame, given once
		{
			dx = PadClamp((int16_t)(dev->mouse_x - dev->given_x));
			dy = PadClamp((int16_t)(dev->mouse_y - dev->given_y));
			dev->given_x += dx;
			dev->given_y += dy;
			buf[4] = dx;
			buf[5] = dy;
		}
		return;
	}
	for (i = 0; i < 4; i++)
		buf[4 + i] = dev->axes[i];
}

// Called by the kernel hook (monitor entry) when prof_armed asks for it: 0 set up, 2 the program uses the counter,
// 3 stop (asked by the computer), 4 SPU voices and RAM scan; PROF_ARMED_PAD: remote pad
static void	ProfSlow(uint32_t *regs)
{
	int			n = mon_state.prof_on - 1, i;
	uint16_t	bit = 16 << n;
	uint32_t	v, armed = mon_state.prof_armed;
	uint8_t		p[PROF_VOICES_SIZE];

	if (armed == PROF_ARMED_PAD)
	{
		PadOverride();
		return;
	}

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
	mon_state.prof_gpustat = GPU_STAT;		// (video mode, read by the computer)

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
		(cmd[0] != MON_CMD_VRAM_WRITE) && (cmd[0] != MON_CMD_SPU_WRITE) && (cmd[0] != MON_CMD_GPU_INFO) &&
		(cmd[0] != MON_CMD_SPU_READ) && (cmd[0] != MON_CMD_EXCHANGE))
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
		case MON_CMD_GPU_INFO:		GpuInfo();									break;
		case MON_CMD_SPU_READ:		SpuRead(adrs, size);						break;
		// USB link exchange (as the kernel hook's: write, then read what follows), also while the program is stopped:
		// a relay run by the GDB server (-gdb -relay) whose exchange crosses a stop goes on
		case MON_CMD_EXCHANGE:
			if (USB_GetData((uint8_t *)adrs, size >> 16, MAX_PACKET_SIZE))
				USB_SendData((uint8_t *)adrs + (size >> 16), size & 0xFFFF, MAX_PACKET_SIZE);
		break;
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
/****************************************/
// Disc booted by the firmware (MON_REASON_CD_SETUP, see monitor.h): Load and LoadExec of "cdrom:" files read with polled
// drive commands as the firmware boots the disc (cdread.h), the BIOS CD driver not being there. Called by the game,
// through the A0 table: its stack holds the sector buffer.

#define	CD_REG(_n_)			(*((volatile uint8_t *)(0x1F801800 + (_n_))))
#define	IRQ_STAT			I_STAT
#define	A0_TABLE			A0Table()

// The BIOS A0 function table (RAM 0x200), its address through a register (GCC takes a constant address for an array of
// size 0)
static void	**A0Table(void)
{
	void	**t;

	__asm__("li %0, 0x200" : "=r"(t));
	return (t);
}

// A busy wait (the root counters may be the game's): about 8 loops a microsecond, a little longer than asked
static void	CdDelayUs(uint32_t us)
{
	volatile uint32_t	i;

	for (i = us * 8; i; i--)
		;
}

// Interrupts off (COP0 SR IEc, no syscall), and back as they were: the game may call with them off already
static uint32_t	CdIrqOff(void)
{
	uint32_t	sr;

	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12\nnop\nnop" : : "r"(sr & ~1u));
	return (sr);
}

static void	CdIrqRestore(uint32_t sr)
{
	__asm__ volatile("mtc0 %0, $12\nnop\nnop" : : "r"(sr));
}

#define	CD_DELAY_US(_us_)		CdDelayUs(_us_)
#define	CD_CRITICAL_ENTER()		uint32_t cd_sr = CdIrqOff()
#define	CD_CRITICAL_EXIT()		CdIrqRestore(cd_sr)
#include "../cdread.h"

// The PS-X EXE header from offset 0x10 (the BIOS Load's header buffer, Exec's argument)
typedef struct
{
	uint32_t	pc0, gp0, t_addr, t_size, d_addr, d_size, b_addr, b_size, s_addr, s_size, sp, fp, gp, ret, base;
} MonExe;

// A(42h) Load(filename, headerbuf): 1 when loaded, 0 if not
int	CdLoad(const char *name, MonExe *hdr)
{
	static const char	magic[] = "PS-X EXE";
	uint8_t				sec[CD_SECTOR];
	uint32_t			size, n;
	int					lba, i;

	if (!CD_DevicePrefix(name))		// Another device: the BIOS Load
		return (mon_state.pad_orig ? ((int (*)(const char *, MonExe *))mon_state.pad_orig)(name, hdr) : 0);
	if (((lba = CD_FindFile(name, &size, sec)) <= 0) || (size <= CD_SECTOR) || !CD_ReadSectors(lba, sec, CD_SECTOR))
		return (0);
	for (i = 0; i < 8; i++)
		if (sec[i] != magic[i])
			return (0);
	for (i = 0; i < (int)sizeof(MonExe); i++)
		((uint8_t *)hdr)[i] = sec[0x10 + i];
	n = hdr->t_size;
	if (n > size - CD_SECTOR)		// (a header bigger than the file)
		n = size - CD_SECTOR;
	if (!CD_ReadSectors(lba + 1, (uint8_t *)hdr->t_addr, n))
		return (0);
	FlushCache();
	return (1);
}

// A(51h) LoadExec(filename, stackbase, stackoffset): as the BIOS one, Load then Exec(header, 1, 0) with that stack
void	CdLoadExec(const char *name, uint32_t stack_base, uint32_t stack_offset)
{
	MonExe	hdr;

	if (!CdLoad(name, &hdr))
		return;
	hdr.s_addr = stack_base;
	hdr.s_size = stack_offset;
	CdIrqOff();
	FlushCache();
	((int (*)(MonExe *, int, const char **))A0_TABLE[0x43])(&hdr, 1, 0);	// A(43h) Exec
}

static void	CdSetup(void)
{
	if (A0_TABLE[0x42] != (void *)CdLoad)
		mon_state.pad_orig = (uint32_t)A0_TABLE[0x42];
	A0_TABLE[0x42] = (void *)CdLoad;
	A0_TABLE[0x51] = (void *)CdLoadExec;
}

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
	if (reason == MON_REASON_PAD_SETUP)
	{
		PadSetup();
		return;
	}
	if (reason == MON_REASON_CD_SETUP)
	{
		CdSetup();
		return;
	}
	if (reason == MON_REASON_SIO_UPDATE)	// (at each exception with "-c sio" or "-c link")
	{
		SioUpdate();
		return;
	}
	if (reason == MON_REASON_SIO_SETUP)
	{
		SioSetup(regs != NULL);
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
