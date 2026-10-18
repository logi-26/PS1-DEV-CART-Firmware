/*
* PS1 DEV CART psxdevcart: the cartridge's features for programs whose library doesn't call the BIOS
* ReturnFromException itself, such as PsyQo programs (PsyQ, PSn00bSDK and Net Yaroze programs don't need it).
*
* The PS1 DEV CART kernel hook runs when the program's interrupt handlers end with the BIOS ReturnFromException.
* PsyQo lets the BIOS handle the interrupts (events), so the hook would never run: no printf on the computer, live
* memory access, screenshots, hot reload, profiler, debugger, crash report or USB link. DevCartInit() adds a BIOS
* interrupt handler that runs the hook at each interrupt, then lets the BIOS go on as usual.
*
* Use (PsyQo): call DevCartInit() once the application is prepared, e.g. at the start of createScene(). Don't call
* psyqo::Kernel::takeOverKernel() (it removes the BIOS kernel and the cartridge's kernel hook with it), and don't
* load the program into the kernel's RAM (TLOAD_ADDR=0x80001000).
* Plain C, no SDK library: compile psxdevcart.c with your program. Started without the cartridge (an emulator, a CD),
* DevCartInit() does nothing and returns 0.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PSXDEVCART_H_
#define _PSXDEVCART_H_

#if defined(__cplusplus)
extern "C" {
#endif

// Runs the cartridge's kernel hook at each interrupt: 1, or 0 if the PS1 DEV CART firmware isn't there
int		DevCartInit(void);

// Removes it (e.g. before the program resets the BIOS interrupt handlers again)
void	DevCartExit(void);

// 1 if the program was started by the PS1 DEV CART firmware (its kernel hook is installed)
int		DevCartPresent(void);

#if defined(__cplusplus)
}
#endif

#endif
