/*
* PS1 DEV CART psxdevcart: runs the cartridge's kernel hook at each interrupt, see psxdevcart.h
*
* The kernel hook replaces the BIOS ReturnFromException (B0 table, function 17h): it answers the computer, then calls
* the BIOS ReturnFromException stored at 0x8000DF7C, which ends the interrupt. Called from a BIOS interrupt handler
* (verifier of an entry of the interrupt chain), that one is replaced for the time of the call by a function that
* just returns: the hook answers the computer, and the BIOS goes on with the other handlers (PsyQo's events...).
* Needs monitor version 6 (firmware with this fixed address), checked by DevCartPresent().
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include "psxdevcart.h"

#define	KERNEL_HOOK_START	0x8000C000
#define	KERNEL_HOOK_END		0x8000E000
#define	MONITOR_STATE		((volatile uint8_t *)0x8000DF00)	// MonitorState (monitor/monitor.h)
#define	MONITOR_MAGIC		0x31474244							// "DBG1"
#define	HOOK_RETURN			(*(void (* volatile *)(void))0x8000DF7C)	// The BIOS ReturnFromException (debugmon.h)
#define	HOOK_MIN_VERSION	6

// Interrupt chain entry (BIOS layout: next, handler, verifier)
typedef struct DevCartIntRP
{
	struct DevCartIntRP	*next;
	void				(*handler)(int);
	int					(*verifier)(void);
	uint32_t			padding;
} DevCartIntRP;

static DevCartIntRP	devcart_rp;
static int			devcart_queued;
static void			(*devcart_hook)(void);

static void	**B0Table(void)
{
	register int	n __asm__("t1") = 0x57;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((void **(*)(void))0xB0)());
}

static int	SysEnqIntRP(int priority, DevCartIntRP *rp)
{
	register int	n __asm__("t1") = 0x02;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(int, DevCartIntRP *))0xC0)(priority, rp));
}

static int	SysDeqIntRP(int priority, DevCartIntRP *rp)
{
	register int	n __asm__("t1") = 0x03;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(int, DevCartIntRP *))0xC0)(priority, rp));
}

static void	JustReturn(void)
{
}

// The hook, as at the end of an interrupt, without ending it; 0: not "handled", the BIOS goes on
static int	DevCartVerifier(void)
{
	void	(*bios_return)(void) = HOOK_RETURN;

	HOOK_RETURN = JustReturn;
	devcart_hook();
	HOOK_RETURN = bios_return;
	return (0);
}

static void	DevCartHandler(int v)
{
	(void)v;
}

int	DevCartPresent(void)
{
	uint32_t	hook = (uint32_t)B0Table()[0x17], bios = (uint32_t)HOOK_RETURN;

	return ((hook - KERNEL_HOOK_START < KERNEL_HOOK_END - KERNEL_HOOK_START) &&
		(bios - KERNEL_HOOK_START >= KERNEL_HOOK_END - KERNEL_HOOK_START) && bios &&
		(*(volatile uint32_t *)MONITOR_STATE == MONITOR_MAGIC) && (MONITOR_STATE[14] >= HOOK_MIN_VERSION));
}

int	DevCartInit(void)
{
	if (devcart_queued)
		return (1);
	if (!DevCartPresent())
		return (0);
	devcart_hook = (void (*)(void))B0Table()[0x17];
	devcart_rp.handler = DevCartHandler;
	devcart_rp.verifier = DevCartVerifier;
	SysEnqIntRP(0, &devcart_rp);	// Priority 0: the first one, before the BIOS's own handlers
	devcart_queued = 1;
	return (1);
}

void	DevCartExit(void)
{
	if (!devcart_queued)
		return;
	SysDeqIntRP(0, &devcart_rp);
	devcart_queued = 0;
}
