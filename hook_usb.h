/*
* PS1 DEV CART tools: faster answers to the computer while a tool runs
*
* The kernel hook answers ps1transfer (memory access while a program runs) only at some interrupts, about once per
* frame. A tool that waits for the next frame can call the hook's USB service itself in the meantime, with the
* interrupts off like in the hook (kernel hook params: pointer to it at 0x8000DF74, PS1 DEV CART firmware).
* No extra interrupt is needed (a 1kHz timer interrupt made the BIOS memory card driver time out with PSn00bSDK).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _HOOK_USB_H_
#define _HOOK_USB_H_

#include <stdint.h>

#define	HOOK_USB_PROCESS	(*((int (**)(uint8_t, uint32_t, uint8_t *, uint32_t, uint32_t *))0x8000DF74))
#define	HOOK_USB_STATUS		(*((volatile uint8_t *)0x1F060000))
#define	HOOK_USB_READY		(1 << 2)	// Active low

// Serves the computer once (a command waiting in the USB FIFO)
static void	HookUsbPoll(void)
{
	EnterCriticalSection();
	if (HOOK_USB_PROCESS && !(HOOK_USB_STATUS & HOOK_USB_READY))
		HOOK_USB_PROCESS(0, 0, NULL, 0, NULL);
	ExitCriticalSection();
}

// Waits for the next frame (like VSync(0)) while serving the computer
static void	HookUsbWaitFrame(void)
{
	int	v = VSync(-1);

	while (VSync(-1) == v)
		HookUsbPoll();
}

#endif
