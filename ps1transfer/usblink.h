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
void	USB_DetectEscaped(void);			// Asks the selected cartridge if it has escaped answers (see main.c)
extern bool		ps1_program_running;
extern uint32_t	usb_big_packet_size;
extern bool		usb_quiet;

// Relays the USB link rings between the opened cartridge and another one until an error: returns the exit code
int		UsbRelay(char *first_name, char *second_serial);
// Connects the USB link of the opened cartridge's program to a TCP server (host:port) until stopped: the exit code
int		UsbNet(char *name, char *host_port);
// Links the USB link of the opened cartridge's program to the console of another computer (netplay), until stopped:
// peer ":port" waits for it (host), "host:port" connects to it; delay/jitter/loss: test conditions (packets sent)
// key: the game's link key from the Netplay lobby (32 hex digits, packets tagged), NULL: none
int		UsbNetLink(char *name, char *peer, const char *key, int delay, int jitter, int loss);

#endif
