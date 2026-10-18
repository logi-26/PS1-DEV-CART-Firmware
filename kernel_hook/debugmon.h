/*
* PS1 DEV CART debug monitor entry points in the kernel hook
*
* - A(40h) unresolved exception (crash, or a break while no debug session runs): the monitor shows the crash screen
*   and keeps answering USB commands ("ps1transfer -crash", "-r"). Replaces the crash handler that was here
*   (the former exhandler.h), now in the monitor to make room.
* - Debug session (Execute with EXEC_DEBUG, "ps1transfer -gdb"): the exception vectors are patched on the first
*   ReturnFromException. Break exceptions (software breakpoints) and COP0 debug exceptions (hardware breakpoints)
*   are handled here before the BIOS exception handler, so a breakpoint in an interrupt callback doesn't overwrite
*   the program registers kept by the BIOS (TCB). Other exceptions go on to the BIOS handler.
* - Pause: the computer sends a 'P' byte, the monitor is called from ReturnFromException (registers in the TCB).
*
* The monitor itself is in the cartridge flash (or RAM for tests), its address is set by the firmware.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "../monitor/monitor.h"

#define	MON_STACK_SIZE	512	// Measured use about 230 bytes (MonitorEntry, MonReceive, USB_GetData; -fstack-usage)
#define	MON_STR(_x_)	#_x_
#define	MON_XSTR(_x_)	MON_STR(_x_)

MonitorState	mon_state __attribute__ ((section (".monitor_state"), used));
uint32_t		mon_regs[MON_REGS] __attribute__ ((used));		// Registers saved by MonSave
uint32_t		mon_stack[MON_STACK_SIZE / 4] __attribute__ ((used));

// The BIOS ReturnFromException the hook calls at the end, at a fixed address: kept for the uninstall by the firmware,
// and replaced for a moment by a program calling the hook from a BIOS interrupt handler (psxdevcart, PsyQo programs)
#define	ReturnFromExceptionOld	(*(void (* volatile *)(void))0x8000DF7C)
void			MonBreakVector(void);
void			MonDebugVector(void);

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

static void	WriteJump(volatile uint32_t *vector, void (*target)(void))
{
	uint32_t	adrs = (uint32_t)target;

	vector[0] = 0x3C1A0000 | ((adrs + 0x8000) >> 16);	// lui   k0, hi
	vector[1] = 0x275A0000 | (adrs & 0xFFFF);			// addiu k0, k0, lo
	vector[2] = 0x03400008;								// jr    k0
	vector[3] = 0;										// nop
}

static void	InstallDebugVectors(void)
{
	volatile uint32_t	*v80 = (uint32_t *)0x80000080;
	int					i;

	if (!mon_state.entry)
		return;
	if (v80[0] != (0x3C1A0000 | (((uint32_t)MonBreakVector + 0x8000) >> 16)))	// Not installed yet
		for (i = 0; i < 4; i++)
			mon_state.chain80[i] = v80[i];
	WriteJump(v80, MonBreakVector);
	WriteJump((uint32_t *)0x80000040, MonDebugVector);
	mon_state.debug = 1;

	// FlushCache() A(44h): only uses registers and cop0 (no syscall), fine in an exception
	{
		register volatile int n asm("t1") = 0x44;
		__asm__ volatile("" : "=r"(n) : "r"(n));
		((void (*)(void))0xA0)();
	}
}

// Exception vectors while debugging. Only k0/k1 can be used before the registers are saved.
// The monitor runs with interrupts disabled (exception), on its own stack, and returns to resume the program.
__asm__(
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".text\n"
	".global MonBreakVector\n"
	"MonBreakVector:\n"				// From 0x80000080: all exceptions
	"	mfc0	$k0, $13\n"			// Cause
	"	nop\n"
	"	andi	$k0, $k0, 0x7C\n"
	"	xori	$k0, $k0, (9 << 2)\n"	// ExcCode 9: break
	"	bnez	$k0, 1f\n"
	"	li		$k1, 1\n"			// MON_REASON_BREAK
	"	j		MonSave\n"
	"	nop\n"
	"1:	lui		$k0, %hi(mon_state + 40)\n"	// MON_STATE_CHAIN80: original vector code (BIOS handler)
	"	addiu	$k0, $k0, %lo(mon_state + 40)\n"
	"	jr		$k0\n"
	"	nop\n"
	".global MonDebugVector\n"
	"MonDebugVector:\n"				// From 0x80000040: COP0 hardware breakpoint
	"	li		$k1, 2\n"			// MON_REASON_DEBUG
	"MonSave:\n"
	// Hardware breakpoints off before any memory access (a watchpoint could trigger again), DCIC status bits
	// (which breakpoint triggered) kept in k1: reason | status << 8
	"	mfc0	$k0, $7\n"
	"	mtc0	$0, $7\n"
	"	andi	$k0, $k0, 0x3F\n"
	"	sll		$k0, $k0, 8\n"
	"	or		$k1, $k1, $k0\n"
	"	lui		$k0, %hi(mon_regs)\n"
	"	addiu	$k0, $k0, %lo(mon_regs)\n"
	"	sw		$0, 0($k0)\n"
	"	sw		$1, 4($k0)\n"
	"	sw		$2, 8($k0)\n"
	"	sw		$3, 12($k0)\n"
	"	sw		$4, 16($k0)\n"
	"	sw		$5, 20($k0)\n"
	"	sw		$6, 24($k0)\n"
	"	sw		$7, 28($k0)\n"
	"	sw		$8, 32($k0)\n"
	"	sw		$9, 36($k0)\n"
	"	sw		$10, 40($k0)\n"
	"	sw		$11, 44($k0)\n"
	"	sw		$12, 48($k0)\n"
	"	sw		$13, 52($k0)\n"
	"	sw		$14, 56($k0)\n"
	"	sw		$15, 60($k0)\n"
	"	sw		$16, 64($k0)\n"
	"	sw		$17, 68($k0)\n"
	"	sw		$18, 72($k0)\n"
	"	sw		$19, 76($k0)\n"
	"	sw		$20, 80($k0)\n"
	"	sw		$21, 84($k0)\n"
	"	sw		$22, 88($k0)\n"
	"	sw		$23, 92($k0)\n"
	"	sw		$24, 96($k0)\n"
	"	sw		$25, 100($k0)\n"
	"	sw		$28, 112($k0)\n"
	"	sw		$29, 116($k0)\n"
	"	sw		$30, 120($k0)\n"
	"	sw		$31, 124($k0)\n"
	"	mfc0	$8, $14\n"			// EPC
	"	mfc0	$9, $12\n"			// SR
	"	mfc0	$10, $13\n"			// Cause
	"	srl		$11, $k1, 8\n"		// DCIC status bits
	"	mfhi	$12\n"
	"	mflo	$13\n"
	"	sw		$8, 128($k0)\n"		// MON_REG_EPC
	"	sw		$12, 132($k0)\n"	// MON_REG_HI
	"	sw		$13, 136($k0)\n"	// MON_REG_LO
	"	sw		$9, 140($k0)\n"		// MON_REG_SR
	"	sw		$10, 144($k0)\n"	// MON_REG_CAUSE
	"	lui		$8, %hi(mon_state)\n"
	"	sw		$11, %lo(mon_state + 36)($8)\n"	// dcic_stop (the breakpoints are set again when resuming)
	"	lw		$9, %lo(mon_state + 4)($8)\n"	// MON_STATE_ENTRY
	"	lui		$sp, %hi(mon_stack + " MON_XSTR(MON_STACK_SIZE) ")\n"
	"	addiu	$sp, $sp, %lo(mon_stack + " MON_XSTR(MON_STACK_SIZE) ")\n"
	"	andi	$a0, $k1, 0xFF\n"
	"	jalr	$9\n"
	"	move	$a1, $k0\n"
	"	lui		$k0, %hi(mon_regs)\n"
	"	addiu	$k0, $k0, %lo(mon_regs)\n"
	"	lw		$8, 132($k0)\n"
	"	lw		$9, 136($k0)\n"
	"	mthi	$8\n"
	"	mtlo	$9\n"
	"	lw		$1, 4($k0)\n"
	"	lw		$2, 8($k0)\n"
	"	lw		$3, 12($k0)\n"
	"	lw		$4, 16($k0)\n"
	"	lw		$5, 20($k0)\n"
	"	lw		$6, 24($k0)\n"
	"	lw		$7, 28($k0)\n"
	"	lw		$8, 32($k0)\n"
	"	lw		$9, 36($k0)\n"
	"	lw		$10, 40($k0)\n"
	"	lw		$11, 44($k0)\n"
	"	lw		$12, 48($k0)\n"
	"	lw		$13, 52($k0)\n"
	"	lw		$14, 56($k0)\n"
	"	lw		$15, 60($k0)\n"
	"	lw		$16, 64($k0)\n"
	"	lw		$17, 68($k0)\n"
	"	lw		$18, 72($k0)\n"
	"	lw		$19, 76($k0)\n"
	"	lw		$20, 80($k0)\n"
	"	lw		$21, 84($k0)\n"
	"	lw		$22, 88($k0)\n"
	"	lw		$23, 92($k0)\n"
	"	lw		$24, 96($k0)\n"
	"	lw		$25, 100($k0)\n"
	"	lw		$28, 112($k0)\n"
	"	lw		$29, 116($k0)\n"
	"	lw		$30, 120($k0)\n"
	"	lw		$31, 124($k0)\n"
	"	lw		$k0, 128($k0)\n"	// EPC (may have been changed by the computer)
	"	nop\n"
	"	jr		$k0\n"
	"	rfe\n"
	".set pop\n"
);
