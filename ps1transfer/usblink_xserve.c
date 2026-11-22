/*
* PS1 DEV CART Transfer tool: -xserve, the kernel hook's exchange served to a program on this computer (pad play) (from usblink.c)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "usblink_int.h"

// -xserve PORT: the kernel hook's exchange ('X') served to one program on this computer (pad play's relay,
// ps1-linkplay/padlink/padrelay.py --bridge 127.0.0.1:PORT). Requests: 'X', address (4 bytes), write size (2),
// read size (2), the bytes to write (all little-endian); answer: 1 (done) or 0 (the USB exchange failed), then the
// bytes read (written first at the address, then read right after them, as the kernel hook does). Only on 127.0.0.1
// (it writes the console's memory). Ends when the program disconnects or Ctrl+C.
int	XServe(char *name, int port)
{
	struct sockaddr_in	addr;
	sock_t				ls, c;
	void				*handle;
	uint8_t				h[9], *w = NULL, *r = NULL, ok;
	uint32_t			adrs, wsize, rsize, served = 0;
	int					one = 1, ret = 1;
#ifdef _WIN32
	WSADATA				wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	setvbuf(stdout, NULL, _IONBF, 0);
	handle = USB_GetHandle();
	ps1_program_running = true;		// Only the kernel hook answers
	usb_big_packet_size = 0;
	usb_quiet = true;
	USB_DetectEscaped();
	usb_patient = 1;
	if ((ls = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		return (1);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if ((bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) || (listen(ls, 1) != 0))
	{
		printf("XSERVE can't use port %d (another program on it?)\n", port);
		CloseSocket(ls);
		return (1);
	}
	printf("XSERVE %s ready on 127.0.0.1:%d\n", name, port);
	while (!stop_requested && !NetReadable(ls, 200))
		;
	if (stop_requested || ((c = accept(ls, NULL, NULL)) == INVALID_SOCKET))
	{
		CloseSocket(ls);
		return (stop_requested ? 0 : 1);
	}
	CloseSocket(ls);
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	w = malloc(0x10000);
	r = malloc(0x10000 + 1);
	printf("XSERVE connected\n");
	while (!stop_requested)
	{
		if (!NetReadable(c, 200))
			continue;
		if (!VlNetRecvAll(c, h, 9) || (h[0] != 'X'))
		{
			ret = 0;		// (the program went: done)
			break;
		}
		adrs = h[1] | (h[2] << 8) | (h[3] << 16) | ((uint32_t)h[4] << 24);
		wsize = h[5] | (h[6] << 8);
		rsize = h[7] | (h[8] << 8);
		if (wsize && !VlNetRecvAll(c, w, wsize))
			break;
		ok = USB_Exchange(handle, adrs, wsize ? w : NULL, wsize, rsize ? r + 1 : NULL, rsize) ? 1 : 0;
		if (!ok)
			memset(r + 1, 0, rsize);
		r[0] = ok;
		if (!NetSendAll(c, r, 1 + rsize))
			break;
		served++;
	}
	printf("XSERVE ends after %u exchanges\n", served);
	CloseSocket(c);
	free(w);
	free(r);
	return (ret);
}

