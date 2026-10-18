/*
* PS1 DEV CART psxcomb: the BIOS services psxcomb.c uses, for programs built without PSn00bSDK (PsyQo, or any program
* that keeps the BIOS kernel). Included by psxcomb.c when PSN00BSDK isn't defined: the same names as PSn00bSDK, so
* psxcomb.c is the same code with both.
*
* The interrupts (serial port, root counter 2) are taken by an entry of the BIOS interrupt chain (SysEnqIntRP),
* queued first: it acknowledges only the interrupts psxcomb asked for, then the chain goes on (the BIOS, PsyQo's
* events and the cartridge's kernel hook keep theirs).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PSXCOMB_BIOS_H_
#define _PSXCOMB_BIOS_H_

#include <stdint.h>

// BIOS device (AddDrv) and file control blocks
typedef struct
{
	char	*name;
	int		flags;
	int		ssize;
	char	*desc;
	void	*f_init, *f_open, *f_inout, *f_close, *f_ioctl, *f_read, *f_write, *f_erase, *f_undelete;
	void	*f_firstfile, *f_nextfile, *f_format, *f_chdir, *f_rename, *f_remove, *f_testdevice;
} DCB;

typedef struct
{
	int			status;
	uint32_t	diskid;
	void		*trns_addr;
	uint32_t	trns_len;
	uint32_t	filepos;
	uint32_t	flags;
	uint32_t	lasterr;
	DCB			*dcb;
	uint32_t	filesize;
	uint32_t	lba;
	uint32_t	fcbnum;
} FCB;

// Events (class HwSIO) not in psxcomb.h, interrupts
#define	HwSIO			0xF000000B
#define	EvSpTIMOUT		0x0100
#define	EvSpERROR		0x8000
#define	IRQ_TIMER2		6
#define	IRQ_SIO1		8

#define	COMB_I_STAT		(*(volatile uint32_t *)0x1F801070)
#define	COMB_I_MASK		(*(volatile uint32_t *)0x1F801074)

static inline int	EnterCriticalSection(void)
{
	register int	a0 __asm__("a0") = 1;
	register int	v0 __asm__("v0");

	__asm__ volatile("syscall\n" : "=r"(v0) : "r"(a0) : "memory");
	return (v0);
}

static inline void	ExitCriticalSection(void)
{
	register int	a0 __asm__("a0") = 2;

	__asm__ volatile("syscall\n" : : "r"(a0) : "memory");
}

static inline void	DeliverEvent(uint32_t cl, uint32_t spec)
{
	register int	n __asm__("t1") = 0x07;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	((void (*)(uint32_t, uint32_t))0xB0)(cl, spec);
}

static inline int	AddDrv(DCB *dcb)
{
	register int	n __asm__("t1") = 0x47;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(DCB *))0xB0)(dcb));
}

static inline int	DelDrv(const char *name)
{
	register int	n __asm__("t1") = 0x48;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(const char *))0xB0)(name));
}

// Interrupt chain entry of psxcomb (BIOS layout: next, handler, verifier). The verifier returns the pending
// interrupts psxcomb asked for (0: not ours), the handler acknowledges each one and calls its callback.
typedef struct CombIntRP
{
	struct CombIntRP	*next;
	void				(*handler)(int);
	int					(*verifier)(void);
	uint32_t			padding;
} CombIntRP;

static void			(*comb_irq_cb[11])(void);
static uint32_t		comb_irq_bits;
static CombIntRP	comb_rp;
static int			comb_rp_queued;

static int	CombIrqVerifier(void)
{
	return ((int)(COMB_I_STAT & COMB_I_MASK & comb_irq_bits));
}

static void	CombIrqHandler(int pending)
{
	int	irq;

	for (irq = 0; irq < 11; irq++)
		if ((pending & (1 << irq)) && comb_irq_cb[irq])
		{
			COMB_I_STAT = ~(1u << irq);		// (only this one: a plain write leaves the others pending)
			comb_irq_cb[irq]();
		}
}

static inline int	CombSysEnqIntRP(int priority, CombIntRP *rp)
{
	register int	n __asm__("t1") = 0x02;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(int, CombIntRP *))0xC0)(priority, rp));
}

static inline int	CombSysDeqIntRP(int priority, CombIntRP *rp)
{
	register int	n __asm__("t1") = 0x03;

	__asm__ volatile("" : "=r"(n) : "r"(n));
	return (((int (*)(int, CombIntRP *))0xC0)(priority, rp));
}

// As PSn00bSDK's InterruptCallback: callback of an interrupt (NULL: none, the interrupt is masked). Called with
// interrupts off (psxcomb.c does it in its critical sections).
static void	InterruptCallback(int irq, void (*cb)(void))
{
	comb_irq_cb[irq] = cb;
	if (cb)
	{
		comb_irq_bits |= 1u << irq;
		if (!comb_rp_queued)
		{
			comb_rp.handler = CombIrqHandler;
			comb_rp.verifier = CombIrqVerifier;
			CombSysEnqIntRP(0, &comb_rp);		// Priority 0: before the BIOS's own handlers
			comb_rp_queued = 1;
		}
		COMB_I_MASK |= 1u << irq;
	}
	else
	{
		COMB_I_MASK &= ~(1u << irq);
		comb_irq_bits &= ~(1u << irq);
		if (!comb_irq_bits && comb_rp_queued)
		{
			CombSysDeqIntRP(0, &comb_rp);
			comb_rp_queued = 0;
		}
	}
}

#endif
