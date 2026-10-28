/*
* PS1 DEV CART Transfer tool: live link of the TIM viewer with the PS1 Dev Cart Manager, see timlive.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _TIMLIVE_H_
#define _TIMLIVE_H_

#include <stdint.h>
#include <stdbool.h>

// Provided by main.c
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
bool	USB_ReadByteNoWait(uint8_t *c);
bool	HotService(char cmd, uint8_t *data, uint32_t param, uint32_t size);
void	PC_FileServer(void);
void	USB_DetectPacketSize(void);
extern bool		ps1_program_running;
extern uint32_t	usb_big_packet_size;
extern bool		usb_quiet;

// The TIM viewer runs (just executed with -tim): commands read on the standard input until "quit". Returns the exit code.
int		TimLive(void);
// "-v auto": if the TIM viewer runs, the VRAM position of the screen on the TV in *x, *y and the viewer held on a
// complete frame (true: TimViewRelease after the screenshot)
bool	TimViewScreen(int *x, int *y);
void	TimViewRelease(void);
// Shared with sndlive.c: the program's text and file server requests shown/served, a line of the standard input (in
// *text, false if none yet; *eof at its end), a pause
void	LiveDrain(void);
bool	LiveNextLine(char **text, bool *eof);
void	LiveSleepMs(int ms);

// The sound tester runs (just executed with -snd): commands read on the standard input until "quit" (sndlive.c)
int		SndLive(void);
// The model viewer runs (just executed with -m): commands read on the standard input until "quit" (mdllive.c)
int		MdlLive(void);
// Memory read/written on request while a program runs (memlive.c)
int		MemLive(void);
// Remote pad (-pad): the buttons of the selected port ("off": let go), the port driven (0 or 1, memlive.c), its
// state as a "MEMPAD READY BUF1 BUF2 MONVER" line
bool	RemotePad(char *arg);
bool	RemotePadPort(char *arg);
int		RemotePadDevice(const char *cmd, const char *err);	// padtype, padaxes, padmove (-1: another command)
void	RemotePadState(void);

#endif
