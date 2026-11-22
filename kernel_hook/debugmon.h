/*
* PS1 DEV CART debug monitor entry points in the kernel hook
*
* - A(40h) unresolved exception (crash, or a break while no debug session runs): the monitor shows the crash screen
*   and keeps answering USB commands ("ps1transfer -crash", "-r"). Replaces the crash handler that was here
*   (the former exhandler.h), now in the monitor to make room.
* - Debug session (Execute with EXEC_DEBUG, "ps1transfer -gdb"): the exception vectors are patched on the first
*   ReturnFromException (by the monitor: MON_REASON_DEBUG_SETUP). Break exceptions (software breakpoints) and COP0
*   debug exceptions (hardware breakpoints) are handled before the BIOS exception handler, so a breakpoint in an
*   interrupt callback doesn't overwrite the program registers kept by the BIOS (TCB). Other exceptions go on to the
*   BIOS handler. Only the check of the 0x80 vector is here (it sees every exception during a debug session: kept in
*   RAM, fast); the registers are saved by the monitor's MonSave (flash, monitor version 15).
* - Pause: the computer sends a 'P' byte, the monitor is called from ReturnFromException (registers in the TCB).
*
* The monitor itself is in the cartridge flash (or RAM for tests), its address is set by the firmware.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "../monitor/monitor.h"

MonitorState	mon_state __attribute__ ((section (".monitor_state"), used));
uint32_t		mon_regs[MON_REGS] __attribute__ ((used));		// Registers saved by MonSave (monitor)
uint32_t		mon_stack[MON_STACK_SIZE / 4] __attribute__ ((used));
uint32_t		mon_save_adrs __attribute__ ((used));	// The monitor's MonSave, set with the vectors (MonBreakVector)

// The BIOS ReturnFromException the hook calls at the end, at a fixed address: kept for the uninstall by the firmware,
// and replaced for a moment by a program calling the hook from a BIOS interrupt handler (psxdevcart, PsyQo programs)
#define	ReturnFromExceptionOld	(*(void (* volatile *)(void))0x8000DF7C)
void			MonBreakVector(void);

// Location is hardcoded at the end of unused kernel space to make sure the original function address is valid so the hook can be uninstalled
static uint32_t	*p_UnresolvedExceptionOld = (uint32_t *)0x8000DF78;
static void		(** const a0table)() = (void (**)())0x200;

// Registers of the current task, saved by the BIOS exception handler: 0x108 points to the TCB header, whose first
// word points to the current TCB (status, mode, then the registers)
static uint32_t	*TcbRegs(void)
{
	return (*(*((uint32_t ***)0x108)) + 2);
}

static void	MonitorCall(int reason, uint32_t *regs)
{
	is_usb_busy_sending = false;	// The program may have been stopped during a printf
	if (mon_state.entry)
		((void (*)(int, uint32_t *))mon_state.entry)(reason, regs);
	else
		while (1)	// No monitor: reset and memory access still work
			USB_Process(0, 0, NULL, 0, NULL);
}

// The monitor sets something up for the program (MON_REASON_TTY_SETUP, MON_REASON_DEBUG_SETUP); nothing without it
static void	MonitorSetup(int reason)
{
	if (mon_state.entry)
		((void (*)(int, uint32_t *))mon_state.entry)(reason, NULL);
}

static void	UnresolvedException(void)
{
	MonitorCall(MON_REASON_CRASH, TcbRegs());
	ReturnFromExceptionOld();	// Resumed by the computer (e.g. a breakpoint hit before the vectors were installed)
}

// 'P' byte received while the program runs (debug session), see USB_ProcessReceive
void	MonitorPause(void)
{
	if (mon_state.debug)
		MonitorCall(MON_REASON_PAUSE, TcbRegs());
}

// 'M' byte received while the program runs: the monitor runs one command (e.g. hot reload) and returns
void	MonitorService(void)
{
	if (mon_state.version)
		((void (*)(int, uint32_t *))mon_state.entry)(MON_REASON_SERVICE, NULL);
}

void	InstallExceptionHandler(void)
{
	// Keep the previous handler so the firmware can uninstall the hook
	*p_UnresolvedExceptionOld = (uint32_t)a0table[0x40];
	a0table[0x40] = UnresolvedException;
	mon_state.magic = MONITOR_MAGIC;
}

// Exception vector 0x80000080 while debugging (written by the monitor). Only k0/k1 can be used before the registers
// are saved: a break goes to the monitor's MonSave (k1 = MON_REASON_BREAK), anything else to the BIOS handler.
__asm__(
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".section .text.monitor_used, \"ax\"\n"	// (kept by the linker script: only the monitor refers to it)
	".global MonBreakVector\n"
	"MonBreakVector:\n"				// From 0x80000080: all exceptions
	"	mfc0	$k0, $13\n"			// Cause
	"	nop\n"
	"	andi	$k0, $k0, 0x7C\n"
	"	xori	$k0, $k0, (9 << 2)\n"	// ExcCode 9: break
	"	bnez	$k0, 1f\n"
	"	li		$k1, 1\n"			// MON_REASON_BREAK
	"	lui		$k0, %hi(mon_save_adrs)\n"
	"	lw		$k0, %lo(mon_save_adrs)($k0)\n"
	"	nop\n"
	"	jr		$k0\n"
	"	nop\n"
	"1:	lui		$k0, %hi(mon_state + 40)\n"	// MON_STATE_CHAIN80: original vector code (BIOS handler)
	"	addiu	$k0, $k0, %lo(mon_state + 40)\n"
	"	jr		$k0\n"
	"	nop\n"
	".set pop\n"
);
