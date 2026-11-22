/*
* PS1 DEV CART Transfer tool: USB link relay, see usblink.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_USBLINK_H_
#define _PS1TRANSFER_USBLINK_H_

#include <stdint.h>
#include <stdbool.h>

// Provided by the other files of ps1transfer (ps1transfer.h)
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
void	*USB_GetHandle(void);
void	USB_SetHandle(void *handle);
void	*USB_OpenOther(char *serial);		// Another cartridge, by serial number (NULL: can't open it)
void	USB_CloseOther(void *handle);
bool	USB_Exchange(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize);
// The same in two halves (-vlink: the next exchange sent before this one's answer is read)
bool	USB_ExchangeSend(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint32_t rsize);
bool	USB_ExchangeRecv(void *handle, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize);
int		LinkStopFlag(void);
extern volatile int		usb_patient;		// -vlink: reads wait for a silent cartridge (not 5 s)
extern volatile uint32_t	link_crc_errors;	// Exchange packets received wrong (not used, or written again)
void	USB_DetectEscaped(void);			// Asks the selected cartridge if it has escaped answers (see usbio.c)
extern bool		ps1_program_running;
extern uint32_t	usb_big_packet_size;
extern bool		usb_quiet;

// Relays the USB link rings between the opened cartridge and another one until an error: returns the exit code
int		UsbRelay(char *first_name, char *second_serial);
// Relay of a debug session (-gdb -relay SERIAL2): started with the program stopped at its entry point, the debugged
// cartridge's turns taken by the GDB server (RelayTurn: false after a USB error, the relay stopped)
bool	USB_ExchangeServed(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize);
bool	RelayBegin(char *first_name, char *second_serial);
bool	RelayTurn(void);
void	RelayEnd(void);
// Red Alert Retaliation's virtual link cable (ps1-linkplay/virtual): its rings carried between the opened cartridge and
// another one until stopped (0), a driver gone (2: a console reset), an error (1)
int		VlinkRelay(char *first_name, char *second_serial);
int		VlinkNet(char *name, char *where, int host);	// -vlinkhost PORT / -vlinkjoin HOST:PORT (Net Play)
int		XServe(char *name, int port);	// -xserve PORT: the kernel hook's exchange for a program here (pad play)
// Connects the USB link of the opened cartridge's program to a TCP server (host:port) until stopped: the exit code
int		UsbNet(char *name, char *host_port);

#endif
