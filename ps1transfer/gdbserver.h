/*
* PS1 DEV CART Transfer tool: GDB remote debugging server, see gdbserver.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _GDBSERVER_H_
#define _GDBSERVER_H_

#include <stdint.h>
#include <stdbool.h>

// Provided by main.c
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
bool	USB_ReadByteNoWait(uint8_t *c);		// A byte from the PS1 if one is waiting
bool	USB_WriteBytes(uint8_t *data, uint32_t size);
void	PC_FileServer(void);				// After a Data Link Escape byte: PCDRV request or stop notification
extern bool		mon_stop_pending;			// Stop notification received by PC_FileServer
extern uint32_t	mon_stop_regs, mon_stop_info;

// Program loaded with a break instruction at its entry point (original instruction given), executed in a debug
// session. Waits for the stop at the entry point, then serves GDB on the port until it detaches or kills.
// relay_serial: another cartridge, the USB link relay with it in the session (link play, both programs with -c link)
int		GdbServer(int port, uint32_t entry_pc, uint32_t entry_insn, char *relay_serial);
// The same, without GDB: frames of the program captured on request (PS1 Dev Cart Manager), see the end of gdbserver.c
int		FrameLive(uint32_t entry_pc, uint32_t entry_insn);

#endif
