/*
* PS1 DEV CART Transfer tool: -vlinkhost / -vlinkjoin, the virtual link cable between two computers (Net Play) (from usblink.c)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "usblink_int.h"

static volatile sock_t	vl_net = INVALID_SOCKET;
static volatile int		vl_net_gone;		// The other computer's connection closed

// The other computer's half connected (and paired, in the lobby): see VlPatience
bool	VlNetThere(void)
{
	return (vl_net != INVALID_SOCKET) && !vl_net_gone && (!vl_lobby || vl_paired);
}
static volatile int		vl_net_fresh;		// Just connected: the console's lines sent first, at a record boundary
// -vlinklobby HOST:PORT: connected to the Net Play lobby server (ps1-linkplay/lobby/netplay_server.py), which pairs this
// half with another one and then forwards the relay frames between them ('L' hello, 'O' offer, 'W' withdraw, 'J' join,
// 'Q' leave; from it: 'G' the open games, 'P' paired, 'X' the other player left, 'E' error)
static char				vl_paired_name[80];

bool	VlNetRecvAll(sock_t s, uint8_t *buf, uint32_t n)
{
	int	r;

	while (n)
	{
		if ((r = recv(s, (char *)buf, n, 0)) <= 0)
			return (false);
		buf += r;
		n -= r;
	}
	return (true);
}

static bool	VlNetSendOn(sock_t s, int type, const uint8_t *data, uint32_t n)
{
	uint8_t	h[3] = {(uint8_t)type, (uint8_t)n, (uint8_t)(n >> 8)};

	return (NetSendAll(s, h, 3) && (!n || NetSendAll(s, data, n)));
}

bool	VlNetSend(int type, const uint8_t *data, uint32_t n)
{
	bool	ok;

	if (vl_net_gone || (vl_net == INVALID_SOCKET))
		return (false);
	LOCK();		// (whole frames: the main thread and the console's thread both send)
	ok = VlNetSendOn(vl_net, type, data, n);
	UNLOCK();
	return (ok);
}

// This console's records to the other computer (the stream's only reader in this mode), and its status every
// VL_NET_STATUS_MS: called by its thread after each exchange and while it idles (the only sender once connected)
void	VlNetTick(VlEnd *e)
{
	static uint8_t	buf[VL_STREAM];
	static uint64_t	status_us;
	uint32_t		n, k, now = VlMs();
	uint8_t			st[20];

	LOCK();
	n = e->out->head - e->out->tail;
	UNLOCK();
	if ((vl_net == INVALID_SOCKET) || vl_net_gone || vl_net_fresh || (vl_lobby && !vl_paired))
	{
		if (e->out_state)		// (only at a record boundary: the other side must start on one)
			return;
		LOCK();
		e->out->tail += n;
		UNLOCK();
		if (!vl_net_fresh || (vl_net == INVALID_SOCKET) || (vl_lobby && !vl_paired))
			return;
		buf[0] = 1;				// Just connected: the console's lines first (it may be waiting with RTS up)
		buf[1] = U32(e->ps1 + 12) & 3;
		vl_net_fresh = 0;
		VlNetSend('R', buf, 2);
		return;
	}
	if (n > 60000)
		n = 60000;
	if (n)
	{
		for (k = 0; k < n; k++)
			buf[k] = e->out->buf[(e->out->tail + k) & (VL_STREAM - 1)];
		if (!VlNetSend('R', buf, n))
			vl_net_gone = 1;
		LOCK();
		e->out->tail += n;
		UNLOCK();
		e->peer->bytes_in += n;
	}
	if (VlUs() - status_us >= VL_NET_STATUS_MS * 1000)
	{
		status_us = VlUs();
		U32(st) = (now - e->hdr_ms < 60000) ? now - e->hdr_ms : 60000;			// Last in-game header, ms ago
		U32(st + 4) = (now - e->frame_ms < 60000) ? now - e->frame_ms : 60000;	// Frame counter last moving
		U32(st + 8) = (VlUs() - e->wait_us < 60000000) ? (uint32_t)((VlUs() - e->wait_us) / 1000) : 60000;
		st[12] = e->sym_on;
		st[13] = U32(e->ps1 + 12) & 3;		// Its own lines
		st[14] = (uint8_t)e->fps;
		st[15] = 0;
		VlNetSend('S', st, 16);
	}
}

// The other computer's frames: its console's records into this console's stream, its status into the VlEnd that
// stands for it
#ifdef _WIN32
static DWORD WINAPI	VlNetRecvMain(LPVOID arg)
#else
static void	*VlNetRecvMain(void *arg)
#endif
{
	static uint8_t	buf[65536];
	VlEnd			*r = (VlEnd *)arg;
	uint8_t			h[3];
	uint32_t		n, room, k, now;

	while (!stop_requested && VlNetRecvAll(vl_net, h, 3))
	{
		n = h[1] | (h[2] << 8);
		if (n && !VlNetRecvAll(vl_net, buf, n))
			break;
		if ((h[0] == 'G') || (h[0] == 'P') || (h[0] == 'X') || (h[0] == 'E'))
		{
			buf[n] = 0;
			if (h[0] == 'G')		// The open games: one line for the Manager ("id name|id name|...")
			{
				for (k = 0; k < n; k++)
					buf[k] = (buf[k] == '\t') ? ' ' : (buf[k] == '\n') ? '|' : buf[k];
				printf("VLINK GAMES %s\n", (char *)buf);
			}
			else if (h[0] == 'P')
			{
				snprintf(vl_paired_name, sizeof(vl_paired_name), "%s", (char *)buf);
				r->name = vl_paired_name;
				r->hdr_ms = r->frame_ms = VlMs() - 60000;
				r->sym_on = 0;
				vl_net_fresh = 1;
				vl_paired = 1;
				printf("VLINK paired with %s\n", vl_paired_name);
			}
			else if (h[0] == 'X')
			{
				vl_paired = 0;
				r->sym_on = 0;
				U32(r->ps1 + 12) = 0;
				printf("VLINK the other player left (%s)\n", (char *)buf);
			}
			else
				printf("VLINK server: %s\n", (char *)buf);
			continue;
		}
		if (vl_lobby && !vl_paired)
			continue;
		if (h[0] == 'R')
		{
			for (;;)		// (room in the stream: the console takes it as it reads)
			{
				LOCK();
				room = VL_STREAM - (r->out->head - r->out->tail);
				UNLOCK();
				if ((room >= n) || stop_requested)
					break;
				NetSleepMs(1);
			}
			for (k = 0; k < n; k++)
				r->out->buf[(r->out->head + k) & (VL_STREAM - 1)] = buf[k];
			LOCK();
			r->out->head += n;
			UNLOCK();
			r->bytes_out += n;
		}
		else if (h[0] == 'Y')		// The lobby server's ping: sent back (its lists show each host's round trip)
			VlNetSend('Y', buf, n);
		else if (h[0] == 'I')		// Its id in the lobby (the Manager knows its own offer in the list by it)
		{
			buf[n] = 0;
			printf("VLINK lobby id %s\n", (char *)buf);
		}
		else if ((h[0] == 'S') && (n >= 16))
		{
			now = VlMs();
			r->hdr_ms = now - U32(buf);
			r->frame_ms = now - U32(buf + 4);
			r->wait_us = VlUs() - (uint64_t)U32(buf + 8) * 1000;
			r->sym_on = buf[12];
			U32(r->ps1 + 12) = buf[13];
			r->fps = buf[14];
		}
	}
	vl_net_gone = 1;
#ifdef _WIN32
	return (0);
#else
	return (NULL);
#endif
}

// The host's listening socket (connections taken by the main loop)
static sock_t	VlNetListenOpen(int port)
{
	struct sockaddr_in	addr;
	sock_t				s;
	int					one = 1;
#ifdef _WIN32
	WSADATA				wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		return (INVALID_SOCKET);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if ((bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) || (listen(s, 1) != 0))
	{
		printf("VLINK can't use port %d (another program on it?)\n", port);
		CloseSocket(s);
		return (INVALID_SOCKET);
	}
	printf("VLINK waiting for the other player on port %d\n", port);
	return (s);
}

// Hello both ways on a new connection: the protocol, the console names. The other's name in other.
static bool	VlNetHello(sock_t s, const char *name, char *other, int size)
{
	uint8_t		hello[80];
	uint32_t	n;
	int			one = 1;

	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	n = (uint32_t)snprintf((char *)hello, sizeof(hello), "%s %s", VL_NET_VERSION, name);
	if (!VlNetSendOn(s, 'H', hello, n) || !NetReadable(s, 10000) || !VlNetRecvAll(s, hello, 3) ||
		((n = hello[1] | (hello[2] << 8)) >= sizeof(hello) - 1) || (hello[0] != 'H') || !VlNetRecvAll(s, hello, n))
	{
		printf("VLINK no answer from the other computer (not Net Play?)\n");
		return (false);
	}
	hello[n] = 0;
	if (strncmp((char *)hello, VL_NET_VERSION " ", strlen(VL_NET_VERSION) + 1))
	{
		printf("VLINK the other computer runs another version of Net Play (%s)\n", (char *)hello);
		return (false);
	}
	snprintf(other, size, "%s", (char *)hello + strlen(VL_NET_VERSION) + 1);
	return (true);
}

// Returns 0 when stopped, 2 when the console's driver went (reset), 1 for an error
int	VlinkNet(char *name, char *where, int host)
{
	static VlStream	up, down;
	static VlEnd	a, r;
	static char		other[80];
	struct sockaddr_in	addr;
	uint32_t		frame_a = 0, rts_ms = 0, now;
	time_t			second;
	sock_t			ls = INVALID_SOCKET, c;
	int				ret = 1, offered = 0, rts = 0, started_recv = 0, n_hello;
	bool			gone_said = false;
#ifdef _WIN32
	int				len;
	HANDLE			ha, hr = NULL;
#else
	socklen_t		len;
	pthread_t		ha, hr;
#endif

	setvbuf(stdout, NULL, _IONBF, 0);
	a.handle = USB_GetHandle();
	a.name = name;
	r.name = "other";
	a.in = &down;
	a.out = &up;
	r.in = &up;
	r.out = &down;
	ps1_program_running = true;		// Only the kernel hooks answer
	usb_big_packet_size = 0;
	usb_quiet = true;
	USB_DetectEscaped();
	r.id = 1;
	a.held_line = r.held_line = -1;
	usb_patient = 1;
	a.peer = &r;
	r.peer = &a;
	a.net = 1;
#ifdef _WIN32
	InitializeCriticalSection(&queue_lock);
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	if (host == 1)		// (2: the lobby, below)
	{
		if ((ls = VlNetListenOpen(atoi(where))) == INVALID_SOCKET)
			return (1);
	}
	else if (host == 2)		// The lobby server
	{
		// Hello: version, player, game title (exactly these three: a lobby server before the details refused more,
		// 2026-11-14), then 'D' the details for the lobby's list: disc, the host's game settings as text (ignored by a
		// server without them)
		char	*game = getenv("VLINK_GAME"), *player = getenv("VLINK_NAME"), *disc = getenv("VLINK_DISC"),
				*info = getenv("VLINK_INFO");
		uint8_t	hello[1400];

		printf("VLINK connecting to the lobby %s\n", where);
		if ((c = NetConnect(where)) == INVALID_SOCKET)
			return (stop_requested ? 0 : 1);
		n_hello = snprintf((char *)hello, sizeof(hello), "%s\t%s\t%s", VL_NET_VERSION, player ? player : name,
			game ? game : "unknown");
		if (!VlNetSendOn(c, 'L', hello, (uint32_t)n_hello))
		{
			CloseSocket(c);
			return (1);
		}
		n_hello = snprintf((char *)hello, sizeof(hello), "%s\t%s", disc ? disc : "", info ? info : "");
		if (n_hello >= (int)sizeof(hello))
			n_hello = sizeof(hello) - 1;
		if ((disc || info) && !VlNetSendOn(c, 'D', hello, (uint32_t)n_hello))
		{
			CloseSocket(c);
			return (1);
		}
		vl_lobby = 1;
		vl_net = c;
		printf("VLINK in the lobby as %s\n", player ? player : name);
	}
	else
	{
		printf("VLINK connecting to %s\n", where);
		if ((c = NetConnect(where)) == INVALID_SOCKET)
			return (stop_requested ? 0 : 1);
		if (!VlNetHello(c, name, other, sizeof(other)))
		{
			CloseSocket(c);
			return (1);
		}
		r.name = other;
		vl_net_fresh = 1;
		vl_net = c;
	}
	if (!VlStart(&a))
		goto out;
#ifdef _WIN32
	ha = CreateThread(NULL, 0, VlThreadMain, &a, 0, NULL);
	if (vl_net != INVALID_SOCKET)
		hr = CreateThread(NULL, 0, VlNetRecvMain, &r, 0, NULL);
#else
	pthread_create(&ha, NULL, VlThreadMain, &a);
	if (vl_net != INVALID_SOCKET)
		pthread_create(&hr, NULL, VlNetRecvMain, &r);
#endif
	started_recv = (vl_net != INVALID_SOCKET);
	if (started_recv)
		printf("VLINK carrying between %s and %s (the other computer)\n", a.name, r.name);
	else
		printf("VLINK carrying %s, waiting for the other player\n", a.name);
	for (second = time(NULL); !StopRequested() && !a.failed && !a.reset; )
	{
		NetSleepMs(50);
		// Host: the other player's connection
		if ((ls != INVALID_SOCKET) && NetReadable(ls, 0))
		{
			len = sizeof(addr);
			c = accept(ls, (struct sockaddr *)&addr, &len);
			if ((c != INVALID_SOCKET) && VlNetHello(c, name, other, sizeof(other)))
			{
				CloseSocket(ls);		// (one game per run)
				ls = INVALID_SOCKET;
				r.name = other;
				vl_net_fresh = 1;
				vl_net = c;
				printf("VLINK the other player joined from %s\n", inet_ntoa(addr.sin_addr));
				printf("VLINK carrying between %s and %s (the other computer)\n", a.name, r.name);
#ifdef _WIN32
				hr = CreateThread(NULL, 0, VlNetRecvMain, &r, 0, NULL);
#else
				pthread_create(&hr, NULL, VlNetRecvMain, &r);
#endif
				started_recv = 1;
				if (offered)
				{
					printf("VLINK WITHDRAW (joined)\n");
					offered = 0;
				}
			}
			else if (c != INVALID_SOCKET)
				CloseSocket(c);
		}
		// Nobody connected: the console holding RTS = waiting on its link page (a game offered)
		now = VlMs();
		if ((vl_net == INVALID_SOCKET) || vl_net_gone || (vl_lobby && !vl_paired))
		{
			if ((U32(a.ps1 + 12) & 2) != (uint32_t)rts)
			{
				rts = U32(a.ps1 + 12) & 2;
				rts_ms = now;
			}
			if (!offered && rts && (now - rts_ms >= VL_OFFER_MS))
			{
				printf("VLINK OFFER %s waiting for a player\n", a.name);
				offered = 1;
				if (vl_lobby)
					VlNetSend('O', NULL, 0);
			}
			else if (offered && !rts && (now - rts_ms >= VL_OFFER_MS))
			{
				printf("VLINK WITHDRAW %s stopped waiting\n", a.name);
				offered = 0;
				if (vl_lobby)
					VlNetSend('W', NULL, 0);
			}
		}
		if (vl_lobby && vl_paired && offered)
			offered = 0;		// (paired: the offer taken)
		if (vl_net_gone && !gone_said)
		{
			printf("VLINK the other player left (connection closed)\n");
			gone_said = true;
		}
		if (time(NULL) != second)
		{
			a.fps = frame_a ? a.frame - frame_a : 0;
			printf("VLINK %s %u turns/s, %u B in, %u B out, %u frames/s   %s %u turns/s, %u B in, %u B out, "
				"%u frames/s%s\n", a.name, (unsigned int)a.turns, (unsigned int)a.bytes_in,
				(unsigned int)a.bytes_out, (unsigned int)a.fps, r.name, 0, (unsigned int)r.bytes_in,
				(unsigned int)r.bytes_out, (unsigned int)r.fps, a.aborts ? "   (waits ended: the other side gone)" : "");
			a.aborts = 0;
			VlTimePrint(&a);
			{	// (a stop line in the settings file: the console's thread may be waiting for it, not reading it)
				char	*path = getenv("VLINK_TUNE"), line[80];
				unsigned int	v;
				FILE	*f;

				if (path && (f = fopen(path, "r")))
				{
					while (fgets(line, sizeof(line), f))
						if ((sscanf(line, "stop %u", &v) == 1) && v && !stop_requested)
						{
							printf("VLINK stop asked\n");
							stop_requested = 1;
						}
					fclose(f);
				}
			}
			if (vl_trace_on && (VlUs() > vl_trace_until))
				VlTraceWrite();
			frame_a = a.frame;
			a.turns = a.bytes_in = a.bytes_out = r.bytes_in = r.bytes_out = 0;
			second = time(NULL);
		}
	}
	stop_requested = 1;
	if (vl_trace_on)		// (a trace going on: written as far as it went)
		VlTraceWrite();
#ifdef _WIN32
	WaitForSingleObject(ha, INFINITE);
#else
	pthread_join(ha, NULL);
#endif
	if (vl_net != INVALID_SOCKET)
	{
#ifdef _WIN32
		shutdown(vl_net, SD_BOTH);
#else
		shutdown(vl_net, SHUT_RDWR);
#endif
		CloseSocket(vl_net);
		vl_net = INVALID_SOCKET;
	}
	if (started_recv)
	{
#ifdef _WIN32
		WaitForSingleObject(hr, 2000);
#else
		pthread_join(hr, NULL);
#endif
	}
	if (ls != INVALID_SOCKET)
		CloseSocket(ls);
	if (a.reset)
	{
		printf("VLINK %s: the driver is gone (console reset?)\n", a.name);
		return (2);
	}
	if (a.failed)
	{
		printf("VLINK %s: USB error\n", a.name);
		return (1);
	}
	printf("VLINK stopped\n");
	return (0);

out:
	if (vl_net != INVALID_SOCKET)
		CloseSocket(vl_net);
	vl_net = INVALID_SOCKET;
	if (ls != INVALID_SOCKET)
		CloseSocket(ls);
	return (ret);
}

/****************************************/
