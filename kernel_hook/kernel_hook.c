#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_WIDTH	320
#define SCREEN_HEIGHT	240

#include "../usb.h"
#include "ttyhook.h"
#include "debugmon.h"
#include "siotty.h"

bool	is_tty_enabled;

typedef struct
{
	bool	is_enabled;
	uint8_t	tty_mode;	// EXEC_TTY_MODE_* | EXEC_DEBUG, set by the Execute command
	uint8_t	usb_packet_log2;	// Set by the firmware, see usb.h
	uint8_t	features;			// Set by the firmware, see usb.h
	int		(* p_USB_Process)(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
} KernelHookParams;

// Kernel hook params are defined at the end of some available reserved kernel space
// See linker script in the current folder
static KernelHookParams	kernel_hook_params __attribute__ ((section (".kernel_hook_params")));

static inline void	**GetB0Table(void);
void	ReturnFromExceptionHook(void);
void	InstallUSBHook(void);

// Important: main() has to be the first defined function in order to be placed at the entry point 0x8000C160
void	main(void)
{
	// (set here: the startup code clears everything up to __bss_end, the monitor state included)
	mon_state.link_buffers = (uint32_t)&sio_buf;
	InstallExceptionHandler();
	InstallUSBHook();
}

static inline void	**GetB0Table(void)
{
	register volatile int n asm("t1") = 0x57;
	__asm__ volatile("" : "=r"(n) : "r"(n));
	return ((void **(*)(void))0xB0)();
}

// Code engine: the cheat table's codes applied (see usb.h, CheatTable)
static void	ApplyCheats(void)
{
	volatile CheatTable	*t = (volatile CheatTable *)CHEAT_TABLE_ADRS;
	uint32_t			i, n;

	if (t->magic != CHEAT_TABLE_MAGIC)
		return;
	n = (t->count < CHEAT_MAX) ? t->count : CHEAT_MAX;
	for (i = 0; i < n; i++)
	{
		uint32_t			code = t->codes[i].code, type = code >> 24, v = t->codes[i].value, cur;
		volatile uint8_t	*a = (volatile uint8_t *)(0x80000000 | (code & 0x1FFFFF));
		bool				wide = (type == 0x80) || ((type & 0xF0) == 0xD0) || (type == 0xC0);

		if (wide && (code & 1))		// (a 16-bit access on an odd address: a bus error)
			continue;
		if (type == 0x80)
			*(volatile uint16_t *)a = v;
		else if (type == 0x30)
			*a = v;
		else if (((type & 0xF0) == 0xD0) || ((type & 0xF0) == 0xE0))
		{
			bool	ok;

			if (type & 0x0C)		// (D4-DF, E4-EF: not tests)
				continue;
			cur = (type & 0xF0) == 0xD0 ? *(volatile uint16_t *)a : *a;
			if ((type & 0xF0) == 0xE0)
				v &= 0xFF;
			switch (type & 3)
			{
				case 0:		ok = cur == v;	break;
				case 1:		ok = cur != v;	break;
				case 2:		ok = cur < v;	break;
				default:	ok = cur > v;	break;
			}
			if (!ok)
				i++;				// The next code skipped
		}
		else if ((type == 0x50) && (i + 1 < n))
		{
			uint32_t	next = t->codes[i + 1].code, w = t->codes[i + 1].value, k, count = (code >> 8) & 0xFF;
			uint32_t	step = code & 0xFF, ntype = next >> 24, base = 0x80000000 | (next & 0x1FFFFF);

			i++;
			for (k = 0; k < count; k++, base += step, w += v)
			{
				if (ntype == 0x30)
					*(volatile uint8_t *)base = w;
				else if ((ntype == 0x80) && !(base & 1))
					*(volatile uint16_t *)base = w;
			}
		}
		else if ((type == 0xC0) && (*(volatile uint16_t *)a != v))
			return;					// The codes after it only when the value is equal
	}
}

void	ReturnFromExceptionHook(void)
{
	int	ret;

	if (kernel_hook_params.is_enabled)
	{
		ApplyCheats();

		// TTY device is replaced by the USB one (default), the serial port one (Net Yaroze like) or kept (BIOS one)
		if (!is_tty_enabled)
		{
			uint8_t	mode = kernel_hook_params.tty_mode & ~EXEC_DEBUG;

			if (mode == EXEC_TTY_MODE_USB)
				SetupTTYhook();
			else if (mode >= EXEC_TTY_MODE_SIO)	// Serial port or USB link tty driver
				SetupSIOtty(mode == EXEC_TTY_MODE_LINK);
			// Debug session ("ps1transfer -gdb"): breakpoints are handled by the monitor
			if (kernel_hook_params.tty_mode & EXEC_DEBUG)
				InstallDebugVectors();
			is_tty_enabled = true;
		}

		SIO_tty_Update();

		// Profiler ("ps1transfer -prof"): the monitor's sampler handles the ticks (exception vector); the monitor is
		// called to set it up, stop it, and for the SPU voices and RAM scan (prof_armed not 1)
		if (mon_state.prof_on && (mon_state.prof_armed != 1))
			((void (*)(int, uint32_t *))mon_state.entry)(MON_REASON_PROFILE, TcbRegs());

		if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
			ret = USB_Process(0, 0, NULL, 0, NULL);
	}

	ReturnFromExceptionOld();
}

void	InstallUSBHook(void)
{
	void	**b0table = GetB0Table();

	kernel_hook_params.p_USB_Process = &USB_Process;

	ReturnFromExceptionOld = b0table[0x17];		// (0x8000DF7C, see debugmon.h)
	b0table[0x17] = ReturnFromExceptionHook;
}
