/*
* PS1USB firmware by OrionSoft [2024]
* USB packet transfer protocol with CRC8 integrity check
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef HOOK
static struct EXEC	exe_header;		// Execute command (firmware only)
#endif
bool				is_usb_busy_sending;

#ifndef HOOK
typedef struct
{
	bool	is_enabled;
	uint8_t	tty_mode;	// EXEC_TTY_MODE_*, uses existing struct padding, layout stays compatible with psxpcdrv
	uint8_t	usb_packet_log2;	// Big packet size supported by the firmware ('u'/'d' commands), 0 = none (original firmware)
	uint8_t	features;			// FEATURE_* supported by this firmware, 0 = none (original firmware)
	int		(* p_USB_Process)(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
} KernelHookParams;

// Kernel hook params are defined at the end of available reserved kernel space
// The address can be decreased to get more room if needed
// The linker script in the kernel_hook folder would need to be adjusted too
static KernelHookParams	*kernel_hook_params = (KernelHookParams *)0x8000DF70;
#endif

#ifdef HOOK
// Kernel hook, 4 bits at a time with a 16 entries table (the 256 entries one took kernel space needed for
// the USB link tty driver); same CRC (reflected, polynomial 0x8C)
static const uint8_t CRC8_Tab16[16] = {0, 157, 35, 190, 70, 219, 101, 248, 140, 17, 175, 50, 202, 87, 233, 116};

// crc ^ byte -> new crc, 4 bits at a time
static __attribute__((noinline)) uint8_t	CRC8_Step(uint8_t x)
{
	x = (x >> 4) ^ CRC8_Tab16[x & 15];
	return ((x >> 4) ^ CRC8_Tab16[x & 15]);
}

// (only the command packets sent, one call: inlined; the data transfer loops have their own)
static inline __attribute__((always_inline)) uint8_t	CRC8_Compute(uint8_t *data, uint32_t size)
{
	uint8_t	crc = 0xFF;

	while (size--)
		crc = CRC8_Step(crc ^ *data++);
	return (crc);
}
#else
static const uint8_t CRC8_Tab[256] =
{
	  0, 94,188,226, 97, 63,221,131,194,156,126, 32,163,253, 31, 65,
	157,195, 33,127,252,162, 64, 30, 95,  1,227,189, 62, 96,130,220,
	 35,125,159,193, 66, 28,254,160,225,191, 93,  3,128,222, 60, 98,
	190,224,  2, 92,223,129, 99, 61,124, 34,192,158, 29, 67,161,255,
	 70, 24,250,164, 39,121,155,197,132,218, 56,102,229,187, 89,  7,
	219,133,103, 57,186,228,  6, 88, 25, 71,165,251,120, 38,196,154,
	101, 59,217,135,  4, 90,184,230,167,249, 27, 69,198,152,122, 36,
	248,166, 68, 26,153,199, 37,123, 58,100,134,216, 91,  5,231,185,
	140,210, 48,110,237,179, 81, 15, 78, 16,242,172, 47,113,147,205,
	 17, 79,173,243,112, 46,204,146,211,141,111, 49,178,236, 14, 80,
	175,241, 19, 77,206,144,114, 44,109, 51,209,143, 12, 82,176,238,
	 50,108,142,208, 83, 13,239,177,240,174, 76, 18,145,207, 45,115,
	202,148,118, 40,171,245, 23, 73,  8, 86,180,234,105, 55,213,139,
	 87,  9,235,181, 54,104,138,212,149,203, 41,119,244,170, 72, 22,
	233,183, 85, 11,136,214, 52,106, 43,117,151,201, 74, 20,246,168,
	116, 42,200,150, 21, 75,169,247,182,232, 10, 84,215,137,107, 53
};

uint8_t		CRC8_Compute(uint8_t *data, uint32_t size)
{
	uint8_t	crc = 0xFF;

	while (size--)
		crc = CRC8_Tab[crc ^ (*data++)];

	return (crc);
}
#endif


/****************************************/

#define	USB_STATUS	*((volatile uint8_t *)0x1F060000)	// USB Status A17+A18
#define	USB_DATA	*((volatile uint8_t *)0x1F020000)	// USB Data A17

// Status bits are inverted (0 = Yes, 1 = No)
#define	USB_STATUS_MASK_HAVE_DATA		(1 << 0)	// FT245R: RXF
#define	USB_STATUS_MASK_CAN_SEND_DATA	(1 << 1)	// FT245R: TXE
#define	USB_STATUS_MASK_USB_READY		(1 << 2)	// FT245R: PWREN

#ifndef HOOK
#define	DCACHE_ADRS	0x1F800000

#define	Init_CRC8()			memcpy((volatile uint8_t *)DCACHE_ADRS, CRC8_Tab, sizeof(CRC8_Tab))
#define	Fast_CRC8(_crc_)	*((volatile uint8_t *)(DCACHE_ADRS+(_crc_)))
#define	Fast_CRC8_Inline(_crc_)	Fast_CRC8(_crc_)
#else
// Kernel hook: CRC8_Step (16 entries table, see CRC8_Compute)
#define	Fast_CRC8(_crc_)	CRC8_Step(_crc_)
// The same, inlined in the data transfer loops (about 1.15x faster transfers than calling CRC8_Step, 60 bytes more)
#define	Fast_CRC8_Inline(_crc_)	({ uint8_t _x_ = (_crc_), _y_ = (_x_ >> 4) ^ CRC8_Tab16[_x_ & 15]; \
									(uint8_t)((_y_ >> 4) ^ CRC8_Tab16[_y_ & 15]); })
#endif

// Decreased timeout value in hook mode to avoid freezing programs for some time
// trying to send printf messages if ps1transfer is not in console mode
// It seems that setting 0 as timeout value for sending data in hook mode is
// perfectly fine (no transfer fails seen) and this causes no printf delay anymore
#ifndef HOOK
#define	TIMEOUT_CNT_SEND	5000000
#else
#define	TIMEOUT_CNT_SEND	0
#endif

#define	TIMEOUT_CNT_GET		5000000
// Execute command param: TTY mode set by the kernel hook when the program starts
#define	EXEC_TTY_MODE_USB	0	// printf/TTY redirected to USB (default)
#define	EXEC_TTY_MODE_BIOS	1	// BIOS TTY device kept untouched
#define	EXEC_TTY_MODE_SIO	2	// Serial port TTY driver installed (like the Net Yaroze boot disc)
#define	EXEC_TTY_MODE_LINK	3	// The same tty driver on the USB link (ps1transfer -relay) instead of the serial port
#define	MAX_PACKET_SIZE		2048	// Please do not modify this value !

// Big packets for the menu upload/download commands 'u'/'d' (less waiting for the CRC/ACK exchange per byte),
// ps1transfer reads kernel_hook_params->usb_packet_log2 to know if they are supported
#ifndef USB_BIG_PACKET_LOG2
#define	USB_BIG_PACKET_LOG2	14	// 16KB (measured: 8KB 609, 16KB 632, 32KB 632, 64KB 634 KB/s download)
#endif
#define	USB_BIG_PACKET_SIZE	(1 << USB_BIG_PACKET_LOG2)
// The kernel hook and the debug monitor use big packets too (FEATURE_HOOK_BIG_PACKETS): memory access while a program
// runs and the file server (measured with 2KB packets: 400/285 KB/s live download/upload, 237/336 KB/s file read/write).
// The USB link exchange ('X') keeps 2KB packets (its transfers are never bigger)
#define	HOOK_PACKET_SIZE	USB_BIG_PACKET_SIZE

// Expansion 1 (cartridge port) bus timing used during the big packet transfers, 0 = keep the BIOS timing (0x0013243F:
// 16 cycles write delay, 4 cycles read delay). The BIOS timing is restored after each transfer (flash ROM accesses).
// Measured (1.5MB): BIOS timing 749 KB/s download, 2 cycles write delay: same, 2 cycles read+write delay: 787 KB/s,
// 1 cycle read+write delay: transfer hangs. Kept disabled, the small gain isn't worth the risk on other cartridges/consoles.
#define	EXP1_DELAY_SIZE	*((volatile uint32_t *)0x1F801008)
#ifndef USB_EXP1_FAST_TIMING
#define	USB_EXP1_FAST_TIMING	0
#endif

enum
{
	USB_STATE_NONE,
	USB_STATE_BAD_CMD,
	USB_STATE_BAD_CRC,
	USB_STATE_TIMEOUT,
	USB_STATE_OK,
	USB_STATE_BOOT_DISC	// 'C' command: the menu boots the disc (FEATURE_BOOT_DISC, see ps1devcart.c)
};

void	ResetPS1(void)
{
	int	i;

	// Stop the DMA channels (e.g. a GPU ordering table being drawn) and reset the GPU before the BIOS
	// reboot, like a hardware reset: the boot could hang when a program was drawing a lot (e.g. the TIM viewer)
	*((volatile uint32_t *)0x1F8010F4) = 0;					// DICR: no DMA interrupt
	for (i = 0; i < 7; i++)
		*((volatile uint32_t *)(0x1F801088 + i * 16)) = 0;	// CHCR: stop channel
	*((volatile uint32_t *)0x1F801814) = 0;					// GP1(0): GPU reset
	((void (*)())0xBFC00000)();
}

bool	USB_GetByte(volatile uint8_t *data)
{
	uint32_t	timeout = TIMEOUT_CNT_GET;

	if (USB_STATUS & USB_STATUS_MASK_USB_READY)
		return (false);

	while (USB_STATUS & USB_STATUS_MASK_HAVE_DATA)
		if (!(timeout--))
			return (false);

	*data = USB_DATA;
	return (true);
}

bool	USB_GetByteNoTimeout(volatile uint8_t *data)
{
	if (USB_STATUS & USB_STATUS_MASK_USB_READY)
		return (false);
	if (USB_STATUS & USB_STATUS_MASK_HAVE_DATA)
		return (false);
	*data = USB_DATA;
	return (true);
}

bool	USB_SendByteTimeout(uint8_t data, uint32_t timeout)
{
	if (USB_STATUS & USB_STATUS_MASK_USB_READY)
		return (false);

	while (USB_STATUS & USB_STATUS_MASK_CAN_SEND_DATA)
		if (!(timeout--))
			return (false);

	USB_DATA = data;
	return (true);
}

#define	USB_SendByte(_data_)		USB_SendByteTimeout((_data_), TIMEOUT_CNT_SEND)
// Data transfers wait for the FIFO also in the kernel hook (USB_SendByte doesn't wait there, so printf never blocks a program)
#define	USB_SendByteData(_data_)	USB_SendByteTimeout((_data_), TIMEOUT_CNT_GET)

bool	USB_SendData(volatile uint8_t *ptr, uint32_t size, uint32_t packet_size)
{
	uint8_t						crc, c, ack;	// (ack in memory: its address is taken, crc and c stay in registers)
	register uint32_t			lsize, olsize;

	while (size)
	{
		olsize = lsize = (size > packet_size) ? packet_size : size;
		crc = 0xFF;
		while (lsize--)
		{
			c = *ptr++;	// Read once, kept in a register
			crc = Fast_CRC8_Inline(crc ^ c);
			// Fast path: one status read per byte, the checked/timeout path is only used when the FIFO is full
			if (!(USB_STATUS & USB_STATUS_MASK_CAN_SEND_DATA))
				USB_DATA = c;
			else if (!USB_SendByteData(c))
				return (false);
		}
		if (!USB_SendByteData(crc))	// Send CRC of data
			return (false);
		if (!USB_GetByte(&ack))	// Wait ACK (Next block or retry ?)
			return (false);
		if (ack == 'N')	// Next
			size -= olsize;
		else			// Rewind
			ptr -= olsize;
	}

	return (true);
}

bool	USB_GetData(volatile uint8_t *ptr, uint32_t size, uint32_t packet_size)
{
	uint8_t						crc, c, slow;	// (slow in memory: its address is taken, crc and c stay in registers)
	register uint32_t			lsize, olsize;

	while (size)
	{
		olsize = lsize = (size > packet_size) ? packet_size : size;
		crc = 0xFF;
		while (lsize--)
		{
			// Fast path: one status read per byte, the checked/timeout path is only used when the FIFO is empty
			if (!(USB_STATUS & USB_STATUS_MASK_HAVE_DATA))
				c = USB_DATA;
			else if (USB_GetByte(&slow))
				c = slow;
			else
				return (false);
			*ptr++ = c;	// The byte is kept in a register for the CRC (no read back from RAM)
			crc = Fast_CRC8_Inline(crc ^ c);
		}
		if (!USB_SendByteData(crc))	// Send CRC of data
			return (false);
		if (!USB_GetByte(&slow))	// Wait ACK (Next block or retry ?)
			return (false);
		if (slow == 'N')	// Next
			size -= olsize;
		else			// Rewind
			ptr -= olsize;
	}

	return (true);
}

int		USB_ProcessSend(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret)
{
	uint8_t						USB_cmd[1+1+4+4+1];
	register volatile uint8_t	*ptr;
	int							i;

	// Check if USB is connected
	if (USB_STATUS & USB_STATUS_MASK_USB_READY)
		return (false);

	// Send a special char to PC to tells it to switch from TTY Console to Binary Data Transfer
	USB_cmd[0] = 16;	// Data Link Escape
	USB_cmd[1] = cmd;
	USB_cmd[2] = (param >> 24) & 0xFF;
	USB_cmd[3] = (param >> 16) & 0xFF;
	USB_cmd[4] = (param >> 8) & 0xFF;
	USB_cmd[5] = param & 0xFF;
	USB_cmd[6] = (size >> 24) & 0xFF;
	USB_cmd[7] = (size >> 16) & 0xFF;
	USB_cmd[8] = (size >> 8) & 0xFF;
	USB_cmd[9] = size & 0xFF;
	USB_cmd[10] = CRC8_Compute(&USB_cmd[1], 1+4+4);

	// Send command packet
	for (i = 0; i < sizeof(USB_cmd); i++)
		if (!USB_SendByte(USB_cmd[i]))
			return (false);

	// Wait ACK
	if (!USB_GetByte(&USB_cmd[0]))
		return (false);

	// Check ACK
	if (USB_cmd[0] != 'G')
		return (false);

	// Read/Write data
	if (data)
	{
		// Read data from PC
		ptr = data;
		if (cmd == 'R')
		{
			if (!USB_GetData(ptr, size, HOOK_PACKET_SIZE))
				return (false);
		}
		else // Write data to PC
		{
			if (!USB_SendData(ptr, size, HOOK_PACKET_SIZE))
				return (false);
		}
	}

	// Get Return value
	i = 4;
	ptr = USB_cmd;
	while (i--)
	{
		if (!USB_GetByte(ptr))
			return (false);
		ptr++;
	}
	if (ret)
		*ret = (USB_cmd[0] << 24) | (USB_cmd[1] << 16) | (USB_cmd[2] << 8) | USB_cmd[3];

	return (true);
}

#define	FEATURE_LIVE_ACCESS	(1 << 0)	// 'D'/'U' memory access and 'V' VRAM read also while a program runs (kernel hook)
#define	FEATURE_DEVICE_ID	(1 << 1)	// 'I' command (menu): device ID (USB serial number) shown on the menu, see ps1devcart.c
#define	FEATURE_HOOK_BIG_PACKETS	(1 << 2)	// Kernel hook and debug monitor use HOOK_PACKET_SIZE packets (file server, live 'D'/'U')
#define	FEATURE_BOOT_DISC	(1 << 3)	// 'C' command (menu): the CD drive unlocked and the disc booted, see ps1devcart.c

#ifndef HOOK
#define	DEVICE_ID_SIZE		16
extern uint8_t	device_id_rx[];		// 'I' command data, see ps1devcart.c
void	DeviceIdReceived(void);
void	ProfFillRam(bool fill);		// Execute: profiler state reset, free RAM filled for the profiler, see ps1devcart.c
void	PadRemoteSetup(void);		// Execute with EXEC_PAD: the remote pad set up by the monitor, see ps1devcart.c
#endif

#ifndef GPU_GP0		// (PSn00bSDK's hwregs_c.h has it for the firmware)
#define	GPU_GP0		*((volatile uint32_t *)0x1F801810)	// Write: GP0 command, read: GPUREAD
#endif
#define	GPU_STAT	*((volatile uint32_t *)0x1F801814)
#define	DMA2_CHCR	*((volatile uint32_t *)0x1F8010A8)	// GPU DMA channel control (bit 24: busy)

// Send a VRAM rectangle line by line, read from the GPU without RAM buffer ('V' command, e.g. live screenshots)
// Each line is a packet with its CRC, a line is read again from the GPU if the computer asks to.
// The GPU must be idle: a program sending a GPU command with the CPU while being interrupted would be disturbed.
bool	USB_SendVRAM(uint32_t xy, uint32_t wh)
{
	uint32_t	x = xy >> 16, y = xy & 0xFFFF, w = wh >> 16, h = wh & 0xFFFF, i, word = 0;
	uint8_t		crc, c;

	while (h)
	{
		while (DMA2_CHCR & (1 << 24))		// Wait end of GPU DMA (e.g. ordering table drawing)
			;
		while (!(GPU_STAT & (1 << 26)))		// Wait GPU ready for a command
			;
		GPU_GP0 = 0xC0000000;				// Copy rectangle VRAM to CPU
		GPU_GP0 = (y << 16) | x;
		GPU_GP0 = (1 << 16) | w;
		while (!(GPU_STAT & (1 << 27)))		// Wait VRAM data ready
			;

		crc = 0xFF;
		for (i = 0; i < w * 2; i++)
		{
			if (!(i & 3))
				word = GPU_GP0;
			c = word;
			word >>= 8;
			crc = Fast_CRC8(crc ^ c);
			if (!USB_SendByteData(c))
				return (false);
		}
		if (!USB_SendByteData(crc))
			return (false);
		if (!USB_GetByte(&crc))	// Wait ACK (Next line or retry ?)
			return (false);
		if (crc == 'N')
		{
			y++;
			h--;
		}
	}

	return (true);
}

#ifdef HOOK
void	MonitorPause(void);	// Debug session: stop the program, see kernel_hook/debugmon.h
void	MonitorService(void);	// One command run by the debug monitor (hot reload), see kernel_hook/debugmon.h
#endif

// Escaped answers: ps1transfer (PS1 DEV CART) sends 'Q' before a command, the answer then comes after a Data Link
// Escape ("DLE g" good, "DLE b" bad) so the text a program prints on the USB tty (it can come just before the answer)
// can't be taken for it. The firmware answers 'Q' with "DLE q" (ps1transfer knows it can ask). Without 'Q' (e.g. the
// original ps1transfer) the answers stay 'G'/'B'.
bool	usb_escaped;

// 'G'/'B', or "DLE g"/"DLE b" when escaped (also "DLE q" for 'Q'). The computer waits for the escaped answer: its
// bytes can wait for room in the FIFO.
static __attribute__((noinline)) bool	USB_Answer(uint8_t c)
{
	if (!usb_escaped)
		return (USB_SendByte(c));
	usb_escaped = false;
	return (USB_SendByteData(16) && USB_SendByteData(c | 0x20));
}

int		USB_ProcessReceive(void)
{
	uint8_t						USB_cmd[1+4+4+1];
	uint8_t						crc;
	register uint8_t			USB_index;
	register volatile uint8_t	*ptr;
	register uint32_t			size;
#ifndef HOOK
	uint32_t					packet_size;
#endif

	if (is_usb_busy_sending)
		return (USB_STATE_NONE);

	if (!USB_GetByteNoTimeout(&USB_cmd[0]))
		return (USB_STATE_NONE);
	if (USB_cmd[0] == 'Q')	// Escaped answer for the command that follows (usually already there)
	{
		usb_escaped = true;
		usb_escaped = USB_Answer('Q');	// "DLE q", then escaped for the command
		if (!USB_GetByteNoTimeout(&USB_cmd[0]))
			return (USB_STATE_NONE);
	}

#ifdef HOOK
	if (USB_cmd[0] == 'P')	// Pause (single byte, debug session only), returns when the computer resumes the program
	{
		MonitorPause();
		return (USB_STATE_OK);
	}
	if (USB_cmd[0] == 'M')	// Monitor service (single byte): e.g. VRAM/SPU write while the program runs
	{
		MonitorService();
		return (USB_STATE_OK);
	}
#endif

#ifndef HOOK
	if (!((USB_cmd[0] == 'D') || (USB_cmd[0] == 'U') || (USB_cmd[0] == 'E') || (USB_cmd[0] == 'F') || (USB_cmd[0] == 'R') ||
		(USB_cmd[0] == 'd') || (USB_cmd[0] == 'u') || (USB_cmd[0] == 'V') || (USB_cmd[0] == 'I') ||
		(USB_cmd[0] == 'C')))
#else
	// While a program runs: reset, and memory access for live debugging (FEATURE_LIVE_ACCESS), exchange (USB link).
	// Live screenshots ('V') go through the monitor service ('M', monitor version 2): kernel space for the USB link.
	if (!((USB_cmd[0] == 'R') || (USB_cmd[0] == 'D') || (USB_cmd[0] == 'U') || (USB_cmd[0] == 'X') || (USB_cmd[0] == 'Z')))
#endif
		return (USB_STATE_BAD_CMD);

	crc = Fast_CRC8(0xFF ^ USB_cmd[0]);
	USB_index = 1;

	// Receive command packet (all of it: a missing CRC byte was compared unset)
	for (; USB_index <= 1+4+4; USB_index++)
	{
		if (!USB_GetByte(&USB_cmd[USB_index]))
			return (USB_STATE_TIMEOUT);
		if (USB_index < 1+4+4)
			crc = Fast_CRC8(crc ^ USB_cmd[USB_index]);
	}

	// Verify command packet integrity
	if (USB_cmd[1+4+4] != crc)
	{
		USB_Answer('B');	// Bad
		return (USB_STATE_BAD_CRC);
	}

	// Reset: also when the answer can't be sent (e.g. the USB FIFO full of the text a program prints: the computer
	// couldn't reset it any more)
	if (!USB_Answer('G') && (USB_cmd[0] != 'R'))	// Good
		return (USB_STATE_TIMEOUT);

	if (USB_cmd[0] == 'R')	// Reset
		ResetPS1();

#ifndef HOOK
	if (USB_cmd[0] == 'C')	// Boot the disc (FEATURE_BOOT_DISC): done by the menu after the answer
		return (USB_STATE_BOOT_DISC);
#endif

	// Get Address pointer from packet (x << 16 | y for 'V')
	ptr = (volatile uint8_t *)((USB_cmd[1] << 24) | (USB_cmd[2] << 16) | (USB_cmd[3] << 8) | USB_cmd[4]);
	// Get Data size from packet (w << 16 | h for 'V')
	size = (USB_cmd[5] << 24) | (USB_cmd[6] << 16) | (USB_cmd[7] << 8) | USB_cmd[8];

#ifndef HOOK
	if (USB_cmd[0] == 'V')	// VRAM read
		return (USB_SendVRAM((uint32_t)ptr, size) ? USB_STATE_OK : USB_STATE_TIMEOUT);
#endif

#ifdef HOOK
	// Exchange (USB link relay, one transfer instead of two): size = write size << 16 | read size, the
	// computer writes at the address, then reads what follows (all in one go for the program)
	if ((USB_cmd[0] == 'X') || (USB_cmd[0] == 'Z'))	// ('Z': the same, the monitor knows it too: a debug session)
		return ((USB_GetData(ptr, size >> 16, MAX_PACKET_SIZE) &&
			USB_SendData(ptr + (size >> 16), size & 0xFFFF, MAX_PACKET_SIZE)) ? USB_STATE_OK : USB_STATE_TIMEOUT);
	// Memory access while a program runs, big packets (FEATURE_HOOK_BIG_PACKETS)
	if (USB_cmd[0] == 'D')
		return (USB_SendData(ptr, size, HOOK_PACKET_SIZE) ? USB_STATE_OK : USB_STATE_TIMEOUT);
	return (USB_GetData(ptr, size, HOOK_PACKET_SIZE) ? USB_STATE_OK : USB_STATE_TIMEOUT);
#else
	// Lowercase download/upload are the same commands with big packets
	packet_size = MAX_PACKET_SIZE;
	if ((USB_cmd[0] == 'd') || (USB_cmd[0] == 'u'))
	{
		packet_size = USB_BIG_PACKET_SIZE;
		USB_cmd[0] -= 'a' - 'A';
	}

	if (USB_cmd[0] == 'E')	// Execute command (the header only: a bigger size would write over the firmware's data)
	{
		ptr = (volatile uint8_t *)&exe_header;
		if (size > sizeof(exe_header))
			size = sizeof(exe_header);
	}
	if (USB_cmd[0] == 'I')	// Device ID (FEATURE_DEVICE_ID): received like an upload
	{
		ptr = device_id_rx;
		if (size > DEVICE_ID_SIZE)
			size = DEVICE_ID_SIZE;
	}

	{
		uint32_t	exp1_timing = EXP1_DELAY_SIZE;
		bool		ok;

		if (USB_EXP1_FAST_TIMING && (packet_size != MAX_PACKET_SIZE))
			EXP1_DELAY_SIZE = USB_EXP1_FAST_TIMING;

		if (USB_cmd[0] == 'D')	// Download data (PS1 to Computer)
			ok = USB_SendData(ptr, size, packet_size);
		else					// Upload data (Computer to PS1)
			ok = USB_GetData(ptr, size, packet_size);

		EXP1_DELAY_SIZE = exp1_timing;
		if (!ok)
			return (USB_STATE_TIMEOUT);
	}

	if (USB_cmd[0] == 'I')	// Device ID: shown on the menu (no "Transfer Successful" message)
	{
		DeviceIdReceived();
		return (USB_STATE_NONE);
	}

	if (USB_cmd[0] == 'E')	// Execute
	{
		PadStop();
		ResetGraph(0);
		StopCallback();

		// ps1transfer -prof: free RAM above the program filled with a pattern (EXEC_PROF_FILL, not kept in tty_mode)
		ProfFillRam((USB_cmd[4] & EXEC_PROF_FILL) != 0);
		// ps1transfer -pad: the pad of player 1 driven from the computer (EXEC_PAD, see monitor/monitor.h)
		if (USB_cmd[4] & EXEC_PAD)
			PadRemoteSetup();

		kernel_hook_params->is_enabled = true;
		// ps1transfer "-c off" / "-c sio" choose what the kernel hook does with the TTY device (e.g. for serial link play)
		kernel_hook_params->tty_mode = USB_cmd[4] & ~(EXEC_PROF_FILL | EXEC_PAD);

		EnterCriticalSection();
		Exec(&exe_header, 1, NULL);
	}

	return (USB_STATE_OK);
#endif
}

int		USB_Process(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret)
{
	int res;

	if (cmd)
	{
		is_usb_busy_sending = true;
		res = USB_ProcessSend(cmd, param, data, size, ret);
		is_usb_busy_sending = false;

		return res;
	}

	return USB_ProcessReceive();
}
