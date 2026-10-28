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

// Provided by main.c
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
void	*USB_GetHandle(void);
void	USB_SetHandle(void *handle);
void	*USB_OpenOther(char *serial);		// Another cartridge, by serial number (NULL: can't open it)
void	USB_CloseOther(void *handle);
bool	USB_Exchange(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata, uint32_t rsize);
extern volatile uint32_t	link_crc_errors;	// Exchange packets received wrong (not used, or written again)
void	USB_DetectEscaped(void);			// Asks the selected cartridge if it has escaped answers (see main.c)
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
// Connects the USB link of the opened cartridge's program to a TCP server (host:port) until stopped: the exit code
int		UsbNet(char *name, char *host_port);

#endif
