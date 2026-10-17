#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_WIDTH	320
#define SCREEN_HEIGHT	240

#include "../usb.h"
#include "ttyhook.h"
#include "siotty.h"
#include "debugmon.h"

void	(* ReturnFromExceptionOld)(void);
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

// Location is hardcoded at the end of unused kernel space to make sure the original function address is valid so the hook can be uninstalled
static uint32_t	*p_ReturnFromExceptionOld = (uint32_t *)0x8000DF7C;

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

void	ReturnFromExceptionHook(void)
{
	int	ret;

	if (kernel_hook_params.is_enabled)
	{
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

	ReturnFromExceptionOld = b0table[0x17];
	b0table[0x17] = ReturnFromExceptionHook;
	*p_ReturnFromExceptionOld = (uint32_t)ReturnFromExceptionOld;
}
