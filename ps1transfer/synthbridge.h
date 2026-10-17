/*
* PS1 DEV CART Transfer tool: SPU synth bridge, see synthbridge.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _SYNTHBRIDGE_H_
#define _SYNTHBRIDGE_H_

#include <stdint.h>
#include <stdbool.h>

#define	SYNTH_FILENAME	"synth.ps-exe"

// Provided by main.c
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
void	*SystemGetFile(char *filename, int *sizeptr);
char	*GetToolPath(char *argv0, char *name);
extern bool		ps1_program_running;
extern uint32_t	usb_big_packet_size;
extern bool		usb_quiet;

// True if the synth runs on the PS1 (its mailbox answers)
bool	SynthRunning(void);
// Starts the synth if needed (the PS1 must be at the menu), then runs the commands read on the standard input
int		SynthBridge(char *argv0, bool start);

#endif
