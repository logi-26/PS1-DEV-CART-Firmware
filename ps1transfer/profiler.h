/*
* PS1 DEV CART Transfer tool: profiler, see profiler.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_PROFILER_H_
#define _PS1TRANSFER_PROFILER_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
	int			rate;		// Samples per second
	int			counter;	// Root counter used (0-2)
	bool		json;		// "PROF {...}" lines (PS1 Dev Cart Manager) instead of the text summary
	char		*csv;		// One line per second in this file
	char		*symfile;	// Symbol file (-sym), otherwise the one of the last executable
	uint32_t	stack_top;	// Initial stack pointer of the program (executed with -e), 0: unknown
	char		*idle;		// -idle: more waiting functions, names (or addresses as shown) separated by commas
} ProfOptions;

// Provided by main.c
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
bool	USB_ReadData(uint8_t *data, uint32_t size);
bool	USB_ReadByteNoWait(uint8_t *c);
void	PC_FileServerCmd(int first);
void	AutoLoadSymbols(char *symfile);
extern bool		ps1_program_running;
extern uint32_t	usb_big_packet_size;
extern bool		usb_quiet;
extern int		symbol_count;

// Provided by usblink.c: Ctrl+C, or a line on the standard input when it is a pipe (GUI)
void	LinkCatchInterrupt(void);
bool	LinkStopRequested(void);

// True while profiling: the profiler packets arriving before the answer to a command are read by USB_Process
extern bool	prof_active;
// Reads the profiler packet that follows a Data Link Escape byte (type byte first), false on a USB error
bool	ProfReadPacket(void);
// The same when the type byte has already been read
bool	ProfReadPacketType(uint8_t type);
// Profiles the running program until stopped: returns the exit code
int		UsbProfile(ProfOptions *o);

#endif
