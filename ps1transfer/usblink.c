/*
* PS1 DEV CART Transfer tool: USB link relay
*
* "ps1transfer -n SERIAL1 -relay SERIAL2" links two consoles through their cartridges and this computer,
* instead of a serial cable: what a console sends is copied to the other one, with the memory access of the kernel
* hook. Each console runs either:
* - a program executed with "-c link" (e.g. an unmodified Net Yaroze game using open("tty:"), like YarIO): the
*   kernel hook's tty driver uses the USB link instead of the serial port (kernel_hook/siotty.h, SioBuffers);
* - or a program with the USB link rings (../ps1-tools/linktest/usblink.h), e.g. the link tester started with "-link usb".
* When both consoles run the tty driver, each one is driven by its own thread with one "exchange" transfer per
* answer of its kernel hook (the computer's half of the buffers written, the PS1's half read): the lowest latency.
* It runs until stopped and prints the throughput every second. It stops at the end of a turn (never in the middle of a
* transfer, which would leave the kernel hook reading the next command as data) on Ctrl+C, or when a line is written
* to its input or the input is closed (the GUI's Stop).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET	sock_t;
#define	CloseSocket	closesocket
#else
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
typedef int		sock_t;
#define	INVALID_SOCKET	(-1)
#define	CloseSocket	close
#endif

#include "usblink.h"
#include "../ps1-tools/linktest/usblink.h"

// Rings: at the address published at LINK_BUFFERS_ADRS (psxcomb USB link), else at USBLINK_ADRS (link tester)
#define	LINK_FIELD(_e_, _f_)	((_e_)->base + offsetof(UsbLink, _f_))
#define	LINK_TX(_e_)			((_e_)->base + offsetof(UsbLink, tx))
#define	LINK_RX(_e_)			((_e_)->base + offsetof(UsbLink, tx) + (_e_)->ring)
#define	RELAY_CHUNK			4096		// Bytes copied at most per direction and turn

// Kernel hook params (0x8000DF70): is_enabled, tty_mode, ...; MonitorState.link_buffers (0x8000DF38): SioBuffers
#define	HOOK_PARAMS_ADRS	0x8000DF70
#define	LINK_BUFFERS_ADRS	0x8000DF38
#define	TTY_MODE_LINK		3

// tty driver buffers of the kernel hook (SioBuffers in kernel_hook/siotty.h): masked ring positions, and frame units
// (what the program wrote during one frame, given to the other program one unit per frame like a serial cable)
#define	TTY_RX_SIZE			256
#define	TTY_TX_SIZE			64
#define	TTY_UNITS			8
#define	TTY_PC_PART			(16 + TTY_UNITS * 2 + TTY_RX_SIZE)	// Written by the computer
#define	TTY_PS1_PART		(16 + TTY_UNITS * 2 + TTY_TX_SIZE)	// Written by the PS1
// Computer part: rx_head, tx_tail, rx_unit_head, tx_unit_tail, rx_units[], rx_buffer[]
#define	PC_RX_HEAD(_e_)			U32((_e_)->pc)
#define	PC_TX_TAIL(_e_)			U32((_e_)->pc + 4)
#define	PC_RX_UNIT_HEAD(_e_)	U32((_e_)->pc + 8)
#define	PC_TX_UNIT_TAIL(_e_)	U32((_e_)->pc + 12)
#define	PC_RX_UNIT(_e_, _i_)	(*(uint16_t *)((_e_)->pc + 16 + (_i_) * 2))
#define	PC_RX_BUF(_e_)			((_e_)->pc + 16 + TTY_UNITS * 2)
// PS1 part: rx_tail, tx_head, rx_unit_tail, tx_unit_head, tx_units[], tx_buffer[]
#define	PS1_RX_TAIL(_e_)		U32((_e_)->ps1)
#define	PS1_TX_HEAD(_e_)		U32((_e_)->ps1 + 4)
#define	PS1_RX_UNIT_TAIL(_e_)	U32((_e_)->ps1 + 8)
#define	PS1_TX_UNIT_HEAD(_e_)	U32((_e_)->ps1 + 12)
#define	PS1_TX_UNIT(_e_, _i_)	(*(uint16_t *)((_e_)->ps1 + 16 + (_i_) * 2))	// End position of a unit sent
#define	PS1_TX_BUF(_e_)			((_e_)->ps1 + 16 + TTY_UNITS * 2)

enum	{END_RINGS, END_TTY};

typedef struct
{
	void		*handle;
	char		*name;
	int			kind;
	uint32_t	bytes;				// Received this second
	// END_RINGS: free-running indexes, rings read/written with D/U
	UsbLink		h;
	uint32_t	base, ring;			// Address of the rings header, ring size
	uint32_t	tx_tail;			// Next byte to read from its send ring
	uint32_t	rx_head;			// Next byte to write in its receive ring
	bool		tx_tail_changed;
	// END_TTY: the computer's half kept here and written at once, the PS1's half read at once
	uint32_t	tty_adrs;
	uint8_t		pc[TTY_PC_PART];
	uint8_t		ps1[TTY_PS1_PART];
	bool		pc_changed;
} LinkEnd;

#define	U32(_p_)	(*(uint32_t *)(_p_))

static volatile int	stop_requested;
static bool			start_quiet;		// Start(): no message (-net retries while the program starts)

#ifdef _WIN32
static BOOL WINAPI	OnInterrupt(DWORD type)
{
	stop_requested = 1;
	return (TRUE);		// Ctrl+C / Ctrl+Break handled: the relay ends its turn
}
#else
static void	OnInterrupt(int sig)
{
	stop_requested = 1;
	signal(sig, OnInterrupt);
}
#endif

// Ctrl+C, or a line / end of file on the standard input when it is a pipe (GUI)
static bool	StopRequested(void)
{
#ifdef _WIN32
	HANDLE	h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	avail = 0;

	if ((h != INVALID_HANDLE_VALUE) && (GetFileType(h) == FILE_TYPE_PIPE) &&
		(!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail))
		stop_requested = 1;
#else
	fd_set			fds;
	struct timeval	tv = {0, 0};

	if (!isatty(0))
	{
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		if (select(1, &fds, NULL, NULL, &tv) > 0)
			stop_requested = 1;
	}
#endif
	return (stop_requested != 0);
}

// Profiler (profiler.c): the same stop request
void	LinkCatchInterrupt(void)
{
#ifdef _WIN32
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
}

bool	LinkStopRequested(void)
{
	return (StopRequested());
}

static void	Use(LinkEnd *e)
{
	USB_SetHandle(e->handle);
}

// Ring piece(s) from/to the computer buffer (the ring may wrap)
static bool	RingAccess(char cmd, uint32_t ring_adrs, uint32_t ring, uint32_t index, uint8_t *buf, uint32_t n)
{
	uint32_t	pos = index & (ring - 1), first = (n < ring - pos) ? n : ring - pos;

	if (!USB_Process(cmd, buf, ring_adrs + pos, first))
		return (false);
	return ((n == first) || USB_Process(cmd, buf + first, ring_adrs, n - first));
}

// State of the console (indexes, and the bytes to send for a tty driver)
static bool	Fetch(LinkEnd *e)
{
	Use(e);
	if (e->kind == END_TTY)
		return (USB_Process('D', e->ps1, e->tty_adrs + TTY_PC_PART, TTY_PS1_PART));
	return (USB_Process('D', (uint8_t *)&e->h, e->base, offsetof(UsbLink, tx)));
}

// Bytes the console sent (tty driver: the units closed by a vertical blank)
static uint32_t	Outgoing(LinkEnd *e)
{
	uint32_t	n = 0, u, pos;

	if (e->kind == END_TTY)
	{
		for (u = PC_TX_UNIT_TAIL(e), pos = PC_TX_TAIL(e); u != (PS1_TX_UNIT_HEAD(e) & (TTY_UNITS - 1));
			u = (u + 1) & (TTY_UNITS - 1))
		{
			n += (PS1_TX_UNIT(e, u) - pos) & (TTY_TX_SIZE - 1);
			pos = PS1_TX_UNIT(e, u) & (TTY_TX_SIZE - 1);
		}
		return (n);
	}
	return (e->h.tx_head - e->tx_tail);
}

// Bytes the console can receive (tty driver: as one unit)
static uint32_t	Space(LinkEnd *e)
{
	if (e->kind == END_TTY)
	{
		if (((PC_RX_UNIT_HEAD(e) + 1) & (TTY_UNITS - 1)) == (PS1_RX_UNIT_TAIL(e) & (TTY_UNITS - 1)))
			return (0);
		return ((PS1_RX_TAIL(e) - PC_RX_HEAD(e) - 1) & (TTY_RX_SIZE - 1));
	}
	return (e->ring - (e->rx_head - e->h.rx_tail));
}

// tty driver: the closed units (n bytes, as given by Outgoing), their sizes in units[] if not NULL
static uint32_t	TakeUnits(LinkEnd *e, uint8_t *buf, uint16_t *units)
{
	uint32_t	n = 0, i, size, u, tail = PC_TX_TAIL(e), total = 0;

	for (u = PC_TX_UNIT_TAIL(e); u != (PS1_TX_UNIT_HEAD(e) & (TTY_UNITS - 1)); u = (u + 1) & (TTY_UNITS - 1))
	{
		size = (PS1_TX_UNIT(e, u) - tail) & (TTY_TX_SIZE - 1);
		// The units' bytes are in a TTY_TX_SIZE ring, so they always fit in buf (TTY_TX_SIZE bytes) unless the
		// console's buffers are corrupted (e.g. by its program): that unit is dropped
		if (total + size > TTY_TX_SIZE)
		{
			tail = PS1_TX_UNIT(e, u) & (TTY_TX_SIZE - 1);
			continue;
		}
		total += size;
		for (i = 0; i < size; i++, tail = (tail + 1) & (TTY_TX_SIZE - 1))
			buf[i] = PS1_TX_BUF(e)[tail];
		buf += size;
		if (units)
			units[n++] = size;
	}
	PC_TX_TAIL(e) = tail;
	PC_TX_UNIT_TAIL(e) = u;
	e->pc_changed = true;
	return (n);
}

static bool	Take(LinkEnd *e, uint8_t *buf, uint32_t n)
{
	if (e->kind == END_TTY)
	{
		TakeUnits(e, buf, NULL);
		return (true);
	}
	Use(e);
	if (!RingAccess('D', LINK_TX(e), e->ring, e->tx_tail, buf, n))
		return (false);
	e->tx_tail += n;
	e->tx_tail_changed = true;
	return (true);
}

static bool	Put(LinkEnd *e, uint8_t *buf, uint32_t n)
{
	uint32_t	i, head;

	if (e->kind == END_TTY)	// One unit (given to the program in one frame)
	{
		for (i = 0, head = PC_RX_HEAD(e); i < n; i++, head = (head + 1) & (TTY_RX_SIZE - 1))
			PC_RX_BUF(e)[head] = buf[i];
		PC_RX_HEAD(e) = head;
		PC_RX_UNIT(e, PC_RX_UNIT_HEAD(e)) = n;
		PC_RX_UNIT_HEAD(e) = (PC_RX_UNIT_HEAD(e) + 1) & (TTY_UNITS - 1);
		e->pc_changed = true;
		e->bytes += n;
		return (true);
	}
	Use(e);
	if (!RingAccess('U', LINK_RX(e), e->ring, e->rx_head, buf, n))
		return (false);
	e->rx_head += n;		// After the bytes
	e->bytes += n;
	return (USB_Process('U', (uint8_t *)&e->rx_head, LINK_FIELD(e, rx_head), 4));
}

// Indexes (and received bytes for a tty driver) written to the console
static bool	Commit(LinkEnd *e)
{
	Use(e);
	if ((e->kind == END_TTY) && e->pc_changed)
	{
		// One transfer: the program sees the bytes and the indexes at once (the kernel hook runs it in an exception)
		e->pc_changed = false;
		return (USB_Process('U', e->pc, e->tty_adrs, TTY_PC_PART));
	}
	if ((e->kind == END_RINGS) && e->tx_tail_changed)
	{
		e->tx_tail_changed = false;
		return (USB_Process('U', (uint8_t *)&e->tx_tail, LINK_FIELD(e, tx_tail), 4));
	}
	return (true);
}

static bool	Relay(LinkEnd *src, LinkEnd *dst)
{
	static uint8_t	buf[RELAY_CHUNK];
	uint32_t		n = Outgoing(src), space = Space(dst);

	if ((src->kind == END_TTY) && (n > space))	// tty driver: whole units only
		return (true);
	if (n > space)
		n = space;
	if (n > RELAY_CHUNK)
		n = RELAY_CHUNK;
	return (!n || (Take(src, buf, n) && Put(dst, buf, n)));
}

// What the console runs, and its current state (bytes it sent before the relay started are dropped)
static bool	Start(LinkEnd *e)
{
	uint8_t		params[4];
	uint32_t	tty_adrs = 0;

	Use(e);
	if (!USB_Process('D', params, HOOK_PARAMS_ADRS, sizeof(params)) ||
		!USB_Process('D', (uint8_t *)&tty_adrs, LINK_BUFFERS_ADRS, 4))
	{
		printf("%s: USB error.\n", e->name);
		return (false);
	}
	if (params[0] && ((params[1] & 0x7F) == TTY_MODE_LINK) && (tty_adrs >= 0x80000000) && (tty_adrs < 0x80010000))
	{
		e->kind = END_TTY;
		e->tty_adrs = tty_adrs;
		if (!USB_Process('D', e->pc, tty_adrs, TTY_PC_PART) || !Fetch(e))
			return (false);
		// Bytes sent before are dropped (units are given by their end position: the next one starts here)
		PC_TX_UNIT_TAIL(e) = PS1_TX_UNIT_HEAD(e) & (TTY_UNITS - 1);
		PC_TX_TAIL(e) = PS1_TX_HEAD(e) & (TTY_TX_SIZE - 1);
		e->pc_changed = true;
		printf("%s: program with the USB link tty driver (-c link)\n", e->name);
		return (Commit(e));
	}
	e->kind = END_RINGS;
	e->base = USBLINK_ADRS;
	if ((tty_adrs >= 0x80010000) && (tty_adrs < 0x80200000))	// Rings published by the program (psxcomb USB link)
	{
		e->base = tty_adrs;
		if (!Fetch(e))
			return (false);
		if (e->h.magic != USBLINK_MAGIC)
			e->base = USBLINK_ADRS;
	}
	if (!Fetch(e))
		return (false);
	e->ring = e->h.ring_size ? e->h.ring_size : USBLINK_RING;
	if ((e->h.magic != USBLINK_MAGIC) || (e->h.version != USBLINK_VERSION) || (e->ring & (e->ring - 1)) ||
		(e->ring > 65536))
	{
		if (!start_quiet)
			printf("%s doesn't run a program with the USB link (start it with \"-c link\", the link tester with "
				"\"-link usb\", or a program using psxcomb's USB link).\n", e->name);
		return (false);
	}
	e->tx_tail = e->h.tx_head;
	e->rx_head = e->h.rx_head;
	e->tx_tail_changed = true;
	printf("%s: program with the USB link rings at 0x%08X (%u bytes)\n", e->name, (unsigned int)e->base,
		(unsigned int)e->ring);
	return (Commit(e));
}

/****************************************/
// Both consoles with the tty driver: one thread per console, bytes passed through queues

#define	QUEUE_SIZE	4096	// Power of 2

#define	QUEUE_UNITS	256		// Power of 2

typedef struct
{
	uint8_t				buf[QUEUE_SIZE];
	volatile uint32_t	head, tail;		// Free-running
	uint16_t			units[QUEUE_UNITS];
	volatile uint32_t	uhead, utail;	// Free-running
} Queue;

typedef struct
{
	LinkEnd			*e;
	Queue			*in, *out;			// Bytes for this console, bytes from it
	volatile int	failed;
	volatile uint32_t	turns;
	void			(*notify)(void);	// Called after units were put in the out queue (-netlink: sent at once)
} TtyThread;

#ifdef _WIN32
static CRITICAL_SECTION	queue_lock;
#define	LOCK()		EnterCriticalSection(&queue_lock)
#define	UNLOCK()	LeaveCriticalSection(&queue_lock)
#else
static pthread_mutex_t	queue_lock = PTHREAD_MUTEX_INITIALIZER;
#define	LOCK()		pthread_mutex_lock(&queue_lock)
#define	UNLOCK()	pthread_mutex_unlock(&queue_lock)
#endif

static void	TtyLoop(TtyThread *t)
{
	LinkEnd		*e = t->e;
	uint8_t		buf[TTY_TX_SIZE], unit[TTY_TX_SIZE];
	uint16_t	units[TTY_UNITS];
	uint32_t	n, i, k, size;

	while (!stop_requested)
	{
		// Units from the other console, as many as the receive buffer can take (known from the last exchange)
		LOCK();
		while (t->in->utail != t->in->uhead)
		{
			size = t->in->units[t->in->utail & (QUEUE_UNITS - 1)];
			if ((size > sizeof(unit)) || (size > Space(e)))
				break;
			for (i = 0; i < size; i++)
				unit[i] = t->in->buf[(t->in->tail + i) & (QUEUE_SIZE - 1)];
			Put(e, unit, size);
			t->in->tail += size;
			t->in->utail++;
		}
		UNLOCK();
		if (!USB_Exchange(e->handle, e->tty_adrs, e->pc, TTY_PC_PART, e->ps1, TTY_PS1_PART))
		{
			t->failed = 1;
			return;
		}
		// Units sent by this console (its tx_tail goes with the next exchange)
		n = TakeUnits(e, buf, units);
		LOCK();
		for (k = 0, i = 0; k < n; i += units[k++])
		{
			if ((t->out->head - t->out->tail + units[k] > QUEUE_SIZE) || (t->out->uhead - t->out->utail >= QUEUE_UNITS))
				break;	// The other console doesn't read: dropped
			for (size = 0; size < units[k]; size++)
				t->out->buf[t->out->head++ & (QUEUE_SIZE - 1)] = buf[i + size];
			t->out->units[t->out->uhead++ & (QUEUE_UNITS - 1)] = units[k];
		}
		UNLOCK();
		if (n && t->notify)
			t->notify();
		t->turns++;
	}
}

#ifdef _WIN32
static DWORD WINAPI	TtyThreadMain(LPVOID arg)
{
	TtyLoop((TtyThread *)arg);
	return (0);
}
#else
static void	*TtyThreadMain(void *arg)
{
	TtyLoop((TtyThread *)arg);
	return (NULL);
}
#endif

static int	TtyRelay(LinkEnd *a, LinkEnd *b)
{
	static Queue	ab, ba;
	TtyThread		ta = {a, &ba, &ab, 0, 0}, tb = {b, &ab, &ba, 0, 0};
	time_t			second = time(NULL);
#ifdef _WIN32
	HANDLE			ha, hb;

	InitializeCriticalSection(&queue_lock);
	ha = CreateThread(NULL, 0, TtyThreadMain, &ta, 0, NULL);
	hb = CreateThread(NULL, 0, TtyThreadMain, &tb, 0, NULL);
#else
	pthread_t		ha, hb;

	pthread_create(&ha, NULL, TtyThreadMain, &ta);
	pthread_create(&hb, NULL, TtyThreadMain, &tb);
#endif
	printf("Relaying between %s and %s, one thread each (Ctrl+C to stop)...\n", a->name, b->name);
	while (!StopRequested() && !ta.failed && !tb.failed)
	{
#ifdef _WIN32
		Sleep(20);
#else
		usleep(20000);
#endif
		if (time(NULL) != second)
		{
			printf("%s -> %s %u B/s   %s -> %s %u B/s   %u/%u exchanges/s\n", a->name, b->name, b->bytes, b->name,
				a->name, a->bytes, ta.turns, tb.turns);
			a->bytes = b->bytes = ta.turns = tb.turns = 0;
			second = time(NULL);
		}
	}
	stop_requested = 1;
#ifdef _WIN32
	WaitForSingleObject(ha, INFINITE);
	WaitForSingleObject(hb, INFINITE);
#else
	pthread_join(ha, NULL);
	pthread_join(hb, NULL);
#endif
	if (ta.failed || tb.failed)
	{
		printf("USB error (%s).\n", ta.failed ? a->name : b->name);
		return (1);
	}
	printf("Relay stopped.\n");
	return (0);
}

/****************************************/
// -net: the USB link of a console connected to a TCP server (e.g. a game server, or the audio streamer's server)

#define	NET_CHUNK	4096		// Bytes moved at most per direction and turn

static void	NetSleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

static sock_t	NetConnect(const char *host_port)
{
	char				host[256], *colon;
	struct hostent		*he;
	struct sockaddr_in	addr;
	sock_t				s;
	int					port, one = 1;
#ifdef _WIN32
	WSADATA				wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	strncpy(host, host_port, sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;
	if (!(colon = strrchr(host, ':')) || ((port = atoi(colon + 1)) <= 0) || (port > 65535))
	{
		printf("-net needs host:port (e.g. 127.0.0.1:7777).\n");
		return (INVALID_SOCKET);
	}
	*colon = 0;
	if (!(he = gethostbyname(host)))
	{
		printf("Unknown host %s.\n", host);
		return (INVALID_SOCKET);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		return (INVALID_SOCKET);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		printf("Can't connect to %s (is the server running?).\n", host_port);
		CloseSocket(s);
		return (INVALID_SOCKET);
	}
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));	// Small packets sent at once
	return (s);
}

// Bytes waiting on the socket (wait: milliseconds to wait for them)
static bool	NetReadable(sock_t s, int wait)
{
	fd_set			fds;
	struct timeval	tv = {0, wait * 1000};

	FD_ZERO(&fds);
	FD_SET(s, &fds);
	return (select((int)s + 1, &fds, NULL, NULL, &tv) > 0);
}

static bool	NetSendAll(sock_t s, const uint8_t *buf, uint32_t n)
{
	int	r;

	while (n)
	{
		if ((r = send(s, (const char *)buf, n, 0)) <= 0)
			return (false);
		buf += r;
		n -= r;
	}
	return (true);
}

int	UsbNet(char *name, char *host_port)
{
	static LinkEnd	a;
	static uint8_t	buf[NET_CHUNK];
	sock_t			s;
	time_t			second;
	uint32_t		up = 0, down = 0, n;
	int				i, r, ret = 1;
	bool			moved;

	setvbuf(stdout, NULL, _IONBF, 0);
	a.handle = USB_GetHandle();
	a.name = name;
	ps1_program_running = true;		// Only the kernel hook answers
	usb_big_packet_size = 0;
	usb_quiet = true;
	// The program may still be starting (e.g. just executed with -e): its rings are waited for 5 seconds
	start_quiet = true;
	for (i = 0; (i < 10) && !Start(&a); i++)
		NetSleepMs(500);
	start_quiet = false;
	if ((i == 10) && !Start(&a))
		return (1);
	if ((s = NetConnect(host_port)) == INVALID_SOCKET)
		return (1);
#ifdef _WIN32
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	printf("Connected %s to %s (Ctrl+C to stop)...\n", a.name, host_port);
	for (second = time(NULL); !StopRequested(); )
	{
		if (!Fetch(&a))
		{
			printf("USB error.\n");
			goto out;
		}
		if ((a.kind == END_RINGS) && (a.h.magic != USBLINK_MAGIC))
		{
			printf("The program stopped the USB link.\n");
			goto out;
		}
		moved = false;
		// Console -> server
		n = Outgoing(&a);
		if ((n > NET_CHUNK) && (a.kind == END_RINGS))	// (tty driver: all its units at once, a few hundred bytes)
			n = NET_CHUNK;
		if (n)
		{
			if (!Take(&a, buf, n) || !NetSendAll(s, buf, n))
			{
				printf("Connection to the server lost.\n");
				goto out;
			}
			up += n;
			moved = true;
		}
		// Server -> console, as much as it can take
		n = Space(&a);
		if (n > NET_CHUNK)
			n = NET_CHUNK;
		if (n && NetReadable(s, moved ? 0 : 1))
		{
			if ((r = recv(s, (char *)buf, n, 0)) <= 0)
			{
				printf("The server closed the connection.\n");
				ret = 0;
				goto out;
			}
			if (!Put(&a, buf, r))
			{
				printf("USB error.\n");
				goto out;
			}
			down += r;
		}
		else if (!n && !moved)
			NetSleepMs(1);
		if (!Commit(&a))
		{
			printf("USB error.\n");
			goto out;
		}
		if (time(NULL) != second)
		{
			printf("%s -> server %u B/s   server -> %s %u B/s\n", a.name, up, a.name, down);
			up = down = 0;
			second = time(NULL);
		}
	}
	printf("Bridge stopped.\n");
	ret = 0;

out:
	CloseSocket(s);
	return (ret);
}

int	UsbRelay(char *first_name, char *second_serial)
{
	static LinkEnd	a, b;
	time_t			second;
	uint32_t		turns = 0;
	int				ret = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	a.handle = USB_GetHandle();
	a.name = first_name;
	b.name = second_serial;
	if (!(b.handle = USB_OpenOther(second_serial)))
	{
		printf("Can't open the cartridge %s (see -l).\n", second_serial);
		return (1);
	}
	ps1_program_running = true;		// Only the kernel hooks answer (2KB packets)
	usb_big_packet_size = 0;
	usb_quiet = true;
	// (both asked first: up to 200 ms each with older firmwares, the first console's program would fill its send
	// buffer meanwhile, and a write that doesn't fit is cut)
	Use(&a);
	USB_DetectEscaped();
	Use(&b);
	USB_DetectEscaped();
	if (!Start(&a) || !Start(&b))
		goto out;
#ifdef _WIN32
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	if ((a.kind == END_TTY) && (b.kind == END_TTY))
	{
		ret = TtyRelay(&a, &b);
		goto out;
	}
	printf("Relaying between %s and %s (Ctrl+C to stop)...\n", a.name, b.name);
#ifdef _WIN32
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	for (second = time(NULL); !StopRequested(); turns++)
	{
		if (!Fetch(&a) || !Fetch(&b) || !Relay(&a, &b) || !Relay(&b, &a) || !Commit(&a) || !Commit(&b))
		{
			printf("USB error.\n");
			goto out;
		}
		if (((a.kind == END_RINGS) && (a.h.magic != USBLINK_MAGIC)) || ((b.kind == END_RINGS) && (b.h.magic != USBLINK_MAGIC)))
		{
			printf("A console stopped the USB link.\n");
			goto out;
		}
		if (time(NULL) != second)
		{
			printf("%s -> %s %u B/s   %s -> %s %u B/s   %u turns/s\n", a.name, b.name, b.bytes, b.name, a.name, a.bytes,
				turns);
			a.bytes = b.bytes = turns = 0;
			second = time(NULL);
		}
	}
	printf("Relay stopped.\n");
	ret = 0;

out:
	USB_SetHandle(a.handle);
	USB_CloseOther(b.handle);
	return (ret);
}

/****************************************/
// -netlink: the USB link of this console linked to the console of another computer through the network (netplay)
//
// Both computers run "ps1transfer -netlink": the host waits (":port"), the guest connects ("host:port"). Each console
// runs a program with the USB link: a Net Yaroze game with the tty driver ("-c link") or a program using psxcomb's
// USB link. The units (tty driver: what the program wrote in one write(), given to the other program one per frame
// like a cable; rings: the bytes taken in one turn) are numbered and sent over UDP:
// - every packet carries all the units the other computer hasn't acknowledged yet: a lost packet is covered by the
//   next one, without waiting for a timeout (a TCP stream would hold everything back for a round trip);
// - the units are sent as soon as the console gives them (the console's thread sends them), and put in the
//   console's queue as soon as they arrive (the receiving thread), the console's thread writing them at its next
//   exchange with the kernel hook;
// - acknowledges and time stamps (round trip time) go with the units, or alone after units were received.
// - between two guests of a Netplay lobby, both connect to the host's relay, which tells each one the other's address
//   as it sees it (NL_PEER): they try to link directly through their routers (UDP hole punching), the relay is used
//   until a packet comes directly, and again if the direct packets stop;
// - a dropout of up to 30 seconds is waited for (units kept and sent again), then the link is given up.
// -netdelay ms[,jitter[,loss%]] delays (and drops) the packets sent, to test as if the other computer was far.
// -netkey (given by the Netplay lobby for each game, see gui/netplay.py): every packet ends with a tag, the first 8
// bytes of HMAC-SHA256(key, packet), and packets without the right tag are dropped: another computer can't link in,
// send units or stop the link (packets version 2; without a key, version 1 as before).

#define	NL_MAGIC		0x4C4E5350	// "PSNL"
#define	NL_VERSION		1
#define	NL_VERSION_KEYED	2		// With the tag (-netkey)
#define	NL_KEY_SIZE		16			// 128-bit key, 32 hex digits
#define	NL_TAG_SIZE		8
#define	NL_HELLO		1
#define	NL_DATA			2
#define	NL_BYE			3
#define	NL_PEER			4			// From the lobby's relay: the other guest's address (IPv4, port, network order)
#define	NL_MAX_PACKET	1200		// UDP payload (under the usual 1500-byte MTU)
#define	NL_STORE		1024		// Units kept until acknowledged (power of 2): 17 s at 60 a second
#define	NL_UNIT_MAX		1024		// Bytes of a unit (rings: bytes taken in one turn)
#define	NL_RESEND_MS	10			// Units not acknowledged sent again after the round trip time + this
#define	NL_KEEPALIVE_MS	250
#define	NL_QUIET_MS		3000		// Nothing from the other computer: shown as waiting
#define	NL_TIMEOUT_MS	30000		// Nothing from the other computer for this time: link lost
#define	NL_PUNCH_MS		6000		// Direct link tried for this time (hello every NL_PUNCH_EVERY ms)
#define	NL_PUNCH_EVERY	50
#define	NL_DIRECT_LOST	1500		// Direct packets stopped this long: through the relay again
#define	NL_DELAYED		512			// -netdelay: packets waiting (power of 2)

typedef struct
{
	uint32_t	magic;
	uint8_t		type, kind, version, flags;
	uint32_t	session;
	uint32_t	ack;				// Next unit expected from the other computer
	uint32_t	first;				// Number of the first unit in the packet
	uint16_t	count, reserved;
	uint32_t	time, echo, echo_age;	// Sender's clock (ms), last time received from the other one and its age
} NlHeader;

typedef struct
{
	uint16_t	len;
	uint8_t		data[NL_UNIT_MAX];
} NlUnit;

typedef struct
{
	double		due;
	int			len;
	uint8_t		data[NL_MAX_PACKET];
} NlDelayed;

static struct
{
	sock_t				s;
	struct sockaddr_in	peer;
	volatile bool		connected;
	uint32_t			session;
	int					kind;
	NlUnit				store[NL_STORE];		// Units sent, kept until acknowledged
	uint32_t			send_next, acked;		// Free-running unit numbers
	uint32_t			recv_next;				// Next unit expected
	uint32_t			peer_time;				// Last time stamp received, and when (local clock)
	double				peer_time_at, last_send, last_recv;
	volatile double		rtt, rtt_min;
	// Statistics of the current second
	volatile uint32_t	units_out, units_in, packets_out, packets_in, resent;
	// -netdelay
	int					delay, jitter, loss;
	NlDelayed			delayed[NL_DELAYED];
	volatile uint32_t	dhead, dtail;
	Queue				*in, *out;				// Units for the console, units from it
	// Guest: the address connected to (host, or the lobby's relay), the other guest's direct address (NL_PEER)
	struct sockaddr_in	relay, direct;
	bool				guest, have_direct, direct_ok, quiet;
	double				punch_start, last_punch, last_direct;
} nl;

#ifdef _WIN32
static CRITICAL_SECTION	nl_lock;
#define	NL_LOCK()	EnterCriticalSection(&nl_lock)
#define	NL_UNLOCK()	LeaveCriticalSection(&nl_lock)
#else
static pthread_mutex_t	nl_lock = PTHREAD_MUTEX_INITIALIZER;
#define	NL_LOCK()	pthread_mutex_lock(&nl_lock)
#define	NL_UNLOCK()	pthread_mutex_unlock(&nl_lock)
#endif

/****************************************/
// Link key: SHA-256 (FIPS 180-4) and HMAC (RFC 2104), small and fast enough for a few hundred packets a second

static uint8_t	nl_key[NL_KEY_SIZE];
static bool		nl_keyed;

typedef struct
{
	uint32_t	h[8];
	uint8_t		buf[64];
	uint32_t	n;			// Bytes in buf
	uint64_t	len;		// Bytes hashed
} Sha256;

static const uint32_t	sha256_k[64] =
{
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define	SHA_ROR(_x_, _n_)	(((_x_) >> (_n_)) | ((_x_) << (32 - (_n_))))

static void	Sha256Block(Sha256 *s, const uint8_t *p)
{
	uint32_t	w[64], v[8], t1, t2;
	int			i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
	for (; i < 64; i++)
		w[i] = w[i - 16] + (SHA_ROR(w[i - 15], 7) ^ SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
			(SHA_ROR(w[i - 2], 17) ^ SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
	memcpy(v, s->h, sizeof(v));
	for (i = 0; i < 64; i++)
	{
		t1 = v[7] + (SHA_ROR(v[4], 6) ^ SHA_ROR(v[4], 11) ^ SHA_ROR(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
			sha256_k[i] + w[i];
		t2 = (SHA_ROR(v[0], 2) ^ SHA_ROR(v[0], 13) ^ SHA_ROR(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		memmove(v + 1, v, 7 * sizeof(uint32_t));
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		s->h[i] += v[i];
}

static void	Sha256Init(Sha256 *s)
{
	static const uint32_t	iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
										0x1f83d9ab, 0x5be0cd19};

	memcpy(s->h, iv, sizeof(iv));
	s->n = 0;
	s->len = 0;
}

static void	Sha256Update(Sha256 *s, const uint8_t *p, uint32_t n)
{
	while (n--)
	{
		s->buf[s->n++] = *p++;
		s->len++;
		if (s->n == 64)
		{
			Sha256Block(s, s->buf);
			s->n = 0;
		}
	}
}

static void	Sha256Final(Sha256 *s, uint8_t out[32])
{
	uint64_t	bits = s->len * 8;
	uint8_t		b = 0x80;
	int			i;

	Sha256Update(s, &b, 1);
	b = 0;
	while (s->n != 56)
		Sha256Update(s, &b, 1);
	for (i = 7; i >= 0; i--)
	{
		b = (uint8_t)(bits >> (i * 8));
		Sha256Update(s, &b, 1);
	}
	for (i = 0; i < 8; i++)
	{
		out[4 * i] = s->h[i] >> 24;
		out[4 * i + 1] = s->h[i] >> 16;
		out[4 * i + 2] = s->h[i] >> 8;
		out[4 * i + 3] = s->h[i];
	}
}

// Tag of a packet: the first NL_TAG_SIZE bytes of HMAC-SHA256(nl_key, data)
static void	NlTag(const uint8_t *data, uint32_t len, uint8_t *tag)
{
	Sha256	s;
	uint8_t	pad[64], inner[32], full[32];
	int		i;

	for (i = 0; i < 64; i++)
		pad[i] = ((i < NL_KEY_SIZE) ? nl_key[i] : 0) ^ 0x36;
	Sha256Init(&s);
	Sha256Update(&s, pad, 64);
	Sha256Update(&s, data, len);
	Sha256Final(&s, inner);
	for (i = 0; i < 64; i++)
		pad[i] = ((i < NL_KEY_SIZE) ? nl_key[i] : 0) ^ 0x5C;
	Sha256Init(&s);
	Sha256Update(&s, pad, 64);
	Sha256Update(&s, inner, 32);
	Sha256Final(&s, full);
	memcpy(tag, full, NL_TAG_SIZE);
}

// The packet's tag is right (compared in constant time); len: without the tag
static bool	NlTagOk(const uint8_t *data, uint32_t len)
{
	uint8_t	tag[NL_TAG_SIZE], diff = 0;
	int		i;

	NlTag(data, len, tag);
	for (i = 0; i < NL_TAG_SIZE; i++)
		diff |= tag[i] ^ data[len + i];
	return (diff == 0);
}

// -netkey: 32 hex digits
static bool	NlSetKey(const char *hex)
{
	int	i, hi, lo;

	nl_keyed = false;
	if (!hex)
		return (true);
	if (strlen(hex) != 2 * NL_KEY_SIZE)
		return (false);
	for (i = 0; i < NL_KEY_SIZE; i++)
	{
		hi = isxdigit((unsigned char)hex[2 * i]) ? (isdigit((unsigned char)hex[2 * i]) ? hex[2 * i] - '0' : (tolower(hex[2 * i]) - 'a' + 10)) : -1;
		lo = isxdigit((unsigned char)hex[2 * i + 1]) ? (isdigit((unsigned char)hex[2 * i + 1]) ? hex[2 * i + 1] - '0' : (tolower(hex[2 * i + 1]) - 'a' + 10)) : -1;
		if ((hi < 0) || (lo < 0))
			return (false);
		nl_key[i] = (uint8_t)((hi << 4) | lo);
	}
	nl_keyed = true;
	return (true);
}

/****************************************/

// Milliseconds, precise (GetTickCount moves by 15.6 ms)
static double	NlNow(void)
{
#ifdef _WIN32
	static LARGE_INTEGER	freq;
	LARGE_INTEGER			t;

	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t);
	return (t.QuadPart * 1000.0 / freq.QuadPart);
#else
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6);
#endif
}

// 1 ms timer resolution for the waits (Windows: 15.6 ms by default), winmm loaded at run time
static void	NlFineTimer(void)
{
#ifdef _WIN32
	HMODULE	winmm = LoadLibraryA("winmm.dll");
	UINT	(WINAPI *begin)(UINT) = winmm ? (UINT (WINAPI *)(UINT))GetProcAddress(winmm, "timeBeginPeriod") : NULL;

	if (begin)
		begin(1);
#endif
}

static void	NlRawSend(const uint8_t *buf, int len)
{
	sendto(nl.s, (const char *)buf, len, 0, (struct sockaddr *)&nl.peer, sizeof(nl.peer));
}

// A packet to the other computer (through the delay line with -netdelay)
static void	NlSendPacket(const uint8_t *buf, int len)
{
	NlDelayed	*d;

	nl.packets_out++;
	if (!nl.delay && !nl.jitter && !nl.loss)
	{
		NlRawSend(buf, len);
		return;
	}
	if (nl.loss && ((rand() % 100) < nl.loss))
		return;
	if (nl.dhead - nl.dtail >= NL_DELAYED)
		return;
	d = &nl.delayed[nl.dhead & (NL_DELAYED - 1)];
	d->due = NlNow() + nl.delay + (nl.jitter ? rand() % (nl.jitter + 1) : 0);
	if ((nl.dhead != nl.dtail) && (d->due < nl.delayed[(nl.dhead - 1) & (NL_DELAYED - 1)].due))
		d->due = nl.delayed[(nl.dhead - 1) & (NL_DELAYED - 1)].due;	// (in order, as on a link)
	d->len = len;
	memcpy(d->data, buf, len);
	nl.dhead++;
}

// Data packet: the units not acknowledged (the oldest first, as many as fit), the acknowledge, the time stamps.
// Called with nl_lock held.
static int	NlBuild(int type, uint8_t *buf)
{
	NlHeader	*h = (NlHeader *)buf;
	uint32_t	u, len = sizeof(NlHeader);
	double		now = NlNow();

	memset(h, 0, sizeof(*h));
	h->magic = NL_MAGIC;
	h->type = type;
	h->kind = nl.kind;
	h->version = nl_keyed ? NL_VERSION_KEYED : NL_VERSION;
	h->session = nl.session;
	h->ack = nl.recv_next;
	h->first = nl.acked;
	h->time = (uint32_t)(uint64_t)now;	// (32-bit milliseconds, wraps after 49 days)
	h->echo = nl.peer_time;
	h->echo_age = nl.peer_time_at ? (uint32_t)(now - nl.peer_time_at) : 0;
	if (type == NL_DATA)
		for (u = nl.acked; u != nl.send_next; u++)
		{
			NlUnit	*unit = &nl.store[u & (NL_STORE - 1)];

			if (len + 2 + unit->len > NL_MAX_PACKET - NL_TAG_SIZE)
				break;
			buf[len] = unit->len;
			buf[len + 1] = unit->len >> 8;
			memcpy(buf + len + 2, unit->data, unit->len);
			len += 2 + unit->len;
			h->count++;
		}
	if (nl_keyed)	// (buf has NL_MAX_PACKET bytes)
	{
		NlTag(buf, len, buf + len);
		len += NL_TAG_SIZE;
	}
	return (len);
}

static void	NlSendData(int type)
{
	uint8_t	buf[NL_MAX_PACKET];
	int		len = NlBuild(type, buf);

	nl.last_send = NlNow();
	NlSendPacket(buf, len);
}

static bool	NlSame(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return ((a->sin_addr.s_addr == b->sin_addr.s_addr) && (a->sin_port == b->sin_port));
}

// Units given by the console (its thread, after an exchange): numbered, kept, sent at once
static void	NlFromConsole(void)
{
	Queue	*q = nl.out;
	bool	any = false;

	LOCK();
	NL_LOCK();
	while ((q->utail != q->uhead) && (nl.send_next - nl.acked < NL_STORE))
	{
		NlUnit		*unit = &nl.store[nl.send_next & (NL_STORE - 1)];
		uint32_t	size = q->units[q->utail & (QUEUE_UNITS - 1)], i;

		for (i = 0; i < size; i++)
			unit->data[i] = q->buf[(q->tail + i) & (QUEUE_SIZE - 1)];
		unit->len = size;
		q->tail += size;
		q->utail++;
		nl.send_next++;
		nl.units_out++;
		any = true;
	}
	if (nl.connected && any)
		NlSendData(NL_DATA);
	NL_UNLOCK();
	UNLOCK();
}

// A packet dropped: without the game's key, or with a key while this side has none (shown once)
static void	NlRejected(int version)
{
	static bool	shown;

	if (shown)
		return;
	shown = true;
	if (nl_keyed && (version == NL_VERSION))
		printf("Packets without the game's key ignored (the other computer needs -netkey too).\n");
	else if (!nl_keyed && (version == NL_VERSION_KEYED))
		printf("Packets with a link key ignored (this computer needs the same -netkey).\n");
	else if (nl_keyed)
		printf("Packets with a wrong key ignored (not from the other player of this game).\n");
}

// A packet from the other computer
static void	NlReceive(uint8_t *buf, int len, struct sockaddr_in *from)
{
	NlHeader	*h = (NlHeader *)buf;
	uint32_t	u, pos = sizeof(NlHeader), n, i, k;
	bool		got = false;
	double		now = NlNow();

	if ((len < (int)sizeof(NlHeader)) || (h->magic != NL_MAGIC))
		return;
	if (nl_keyed)	// Only packets with the game's key (the tag is not counted in len after)
	{
		if ((h->version != NL_VERSION_KEYED) || (len < (int)sizeof(NlHeader) + NL_TAG_SIZE) ||
			!NlTagOk(buf, len - NL_TAG_SIZE))
		{
			NlRejected(h->version);
			return;
		}
		len -= NL_TAG_SIZE;
	}
	else if (h->version != NL_VERSION)
	{
		NlRejected(h->version);
		return;
	}
	NL_LOCK();
	if (h->type == NL_PEER)		// The relay gives the other guest's address: a direct link is tried
	{
		if (nl.guest && NlSame(from, &nl.relay) && (len >= (int)sizeof(NlHeader) + 6) && !nl.have_direct)
		{
			memset(&nl.direct, 0, sizeof(nl.direct));
			nl.direct.sin_family = AF_INET;
			memcpy(&nl.direct.sin_addr, buf + sizeof(NlHeader), 4);
			memcpy(&nl.direct.sin_port, buf + sizeof(NlHeader) + 4, 2);
			nl.have_direct = true;
			nl.punch_start = now;
			printf("Trying a direct link with %s:%d\n", inet_ntoa(nl.direct.sin_addr), ntohs(nl.direct.sin_port));
		}
		goto out;
	}
	if (!nl.connected)
	{
		if ((h->type != NL_HELLO) && (h->type != NL_DATA))
			goto out;
		if (h->kind != nl.kind)
		{
			printf("The other console runs %s, this one %s: both need the same kind of link.\n",
				(h->kind == END_TTY) ? "a Net Yaroze tty link program (-c link)" : "a psxcomb USB link program",
				(nl.kind == END_TTY) ? "a Net Yaroze tty link program (-c link)" : "a psxcomb USB link program");
			stop_requested = 1;
			goto out;
		}
		nl.peer = *from;
		nl.session = h->session;
		nl.connected = true;
		printf("Linked with %s:%d\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
		NlSendData(NL_HELLO);
	}
	else if (!NlSame(from, &nl.peer) && !(nl.guest && NlSame(from, &nl.relay)) &&
		!(nl.have_direct && NlSame(from, &nl.direct)))
		goto out;		// Another computer
	else if (h->session != nl.session)
	{
		// Both sides connecting (through the lobby's relay: both said hello at the same time): the lower session
		// number is kept by both. Otherwise an old session.
		if ((h->type != NL_HELLO) || (h->session > nl.session))
			goto out;
		nl.session = h->session;
		NlSendData(NL_HELLO);
	}
	if (h->type == NL_BYE)
	{
		printf("The other computer stopped the link.\n");
		stop_requested = 1;
		goto out;
	}
	if (nl.have_direct && NlSame(from, &nl.direct))	// Direct packets: the relay isn't needed any more
	{
		nl.last_direct = now;
		if (!nl.direct_ok)
		{
			nl.direct_ok = true;
			nl.peer = nl.direct;
			printf("Direct link with %s:%d (the host's relay isn't used any more)\n", inet_ntoa(from->sin_addr),
				ntohs(from->sin_port));
		}
	}
	if (nl.quiet)
	{
		nl.quiet = false;
		printf("The other computer is back.\n");
	}
	nl.packets_in++;
	nl.last_recv = now;
	nl.peer_time = h->time;
	nl.peer_time_at = now;
	if (h->echo)
	{
		// (the clocks are 32-bit milliseconds: the difference wraps with them; an old or bad value is ignored)
		double	rtt = (int32_t)((uint32_t)(uint64_t)now - h->echo - h->echo_age);

		if ((rtt >= 0) && (rtt < 10000))
		{
			nl.rtt = nl.rtt ? nl.rtt * 0.8 + rtt * 0.2 : rtt;
			if (!nl.rtt_min || (rtt < nl.rtt_min))
				nl.rtt_min = rtt;
		}
	}
	if ((int32_t)(h->ack - nl.acked) > 0 && (int32_t)(h->ack - nl.send_next) <= 0)
		nl.acked = h->ack;
	// Units: the ones not received yet go to the console's queue, in order
	LOCK();
	for (u = h->first, i = 0; i < h->count; i++, u++)
	{
		if (pos + 2 > (uint32_t)len)
			break;
		n = buf[pos] | (buf[pos + 1] << 8);
		if ((pos + 2 + n > (uint32_t)len) || (n > NL_UNIT_MAX))
			break;
		if (u == nl.recv_next)
		{
			Queue	*q = nl.in;

			if ((q->head - q->tail + n > QUEUE_SIZE) || (q->uhead - q->utail >= QUEUE_UNITS))
				break;	// The console doesn't read: kept by the other computer until it does
			for (k = 0; k < n; k++)
				q->buf[q->head++ & (QUEUE_SIZE - 1)] = buf[pos + 2 + k];
			q->units[q->uhead++ & (QUEUE_UNITS - 1)] = n;
			nl.recv_next++;
			nl.units_in++;
			got = true;
		}
		pos += 2 + n;
	}
	UNLOCK();
	if (got)
		NlSendData(NL_DATA);	// Acknowledge at once (with this console's units not acknowledged, if any)
out:
	NL_UNLOCK();
}

// psxcomb USB link rings: the bytes of each turn are a unit
static void	RingsLoop(TtyThread *t)
{
	LinkEnd		*e = t->e;
	static uint8_t	buf[NL_UNIT_MAX];
	uint32_t	n, i, size;

	while (!stop_requested)
	{
		if (!Fetch(e))
		{
			t->failed = 1;
			return;
		}
		if (e->h.magic != USBLINK_MAGIC)
		{
			printf("The console stopped the USB link.\n");
			t->failed = 1;
			return;
		}
		LOCK();
		while (t->in->utail != t->in->uhead)
		{
			size = t->in->units[t->in->utail & (QUEUE_UNITS - 1)];
			if (size > Space(e))
				break;
			for (i = 0; i < size; i++)
				buf[i] = t->in->buf[(t->in->tail + i) & (QUEUE_SIZE - 1)];
			UNLOCK();
			if (!Put(e, buf, size))
			{
				t->failed = 1;
				return;
			}
			LOCK();
			t->in->tail += size;
			t->in->utail++;
		}
		UNLOCK();
		if ((n = Outgoing(e)))
		{
			if (n > NL_UNIT_MAX)
				n = NL_UNIT_MAX;
			if (!Take(e, buf, n))
			{
				t->failed = 1;
				return;
			}
			LOCK();
			if ((t->out->head - t->out->tail + n <= QUEUE_SIZE) && (t->out->uhead - t->out->utail < QUEUE_UNITS))
			{
				for (i = 0; i < n; i++)
					t->out->buf[t->out->head++ & (QUEUE_SIZE - 1)] = buf[i];
				t->out->units[t->out->uhead++ & (QUEUE_UNITS - 1)] = n;
			}
			UNLOCK();
			if (t->notify)
				t->notify();
		}
		if (!Commit(e))
		{
			t->failed = 1;
			return;
		}
		t->turns++;
	}
}

#ifdef _WIN32
static DWORD WINAPI	RingsThreadMain(LPVOID arg)
{
	RingsLoop((TtyThread *)arg);
	return (0);
}
#else
static void	*RingsThreadMain(void *arg)
{
	RingsLoop((TtyThread *)arg);
	return (NULL);
}
#endif

// Receiving thread: packets from the other computer, resends, keepalives, the delay line of -netdelay
static volatile int	nl_net_done;

static void	NlNetLoop(void)
{
	uint8_t				buf[2048];
	struct sockaddr_in	from;
	int					len;
	double				now;

	while (!stop_requested)
	{
#ifdef _WIN32
		int			flen = sizeof(from);
#else
		socklen_t	flen = sizeof(from);
#endif

		if (NetReadable(nl.s, 1))
		{
			len = recvfrom(nl.s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
			if (len > 0)
				NlReceive(buf, len, &from);
		}
		now = NlNow();
		NL_LOCK();
		while ((nl.dtail != nl.dhead) && (nl.delayed[nl.dtail & (NL_DELAYED - 1)].due <= now))
		{
			NlRawSend(nl.delayed[nl.dtail & (NL_DELAYED - 1)].data, nl.delayed[nl.dtail & (NL_DELAYED - 1)].len);
			nl.dtail++;
		}
		// Direct link tried: hellos to the other guest's address (they open our router to its packets)
		if (nl.have_direct && !nl.direct_ok && (now - nl.punch_start < NL_PUNCH_MS) &&
			(now - nl.last_punch >= NL_PUNCH_EVERY))
		{
			uint8_t	pbuf[NL_MAX_PACKET];
			int		plen = NlBuild(NL_HELLO, pbuf);

			sendto(nl.s, (const char *)pbuf, plen, 0, (struct sockaddr *)&nl.direct, sizeof(nl.direct));
			nl.last_punch = now;
		}
		if (nl.direct_ok && (now - nl.last_direct > NL_DIRECT_LOST))
		{
			nl.direct_ok = false;
			nl.peer = nl.relay;
			nl.punch_start = now;		// (tried again)
			printf("Direct link lost: through the host's relay again\n");
		}
		if (nl.connected)
		{
			if ((nl.acked != nl.send_next) && (now - nl.last_send >= NL_RESEND_MS + nl.rtt))	// (acknowledge not back yet)
			{
				nl.resent++;
				NlSendData(NL_DATA);
			}
			else if (now - nl.last_send >= NL_KEEPALIVE_MS)
				NlSendData(NL_DATA);
			if (!nl.quiet && (now - nl.last_recv > NL_QUIET_MS))
			{
				nl.quiet = true;
				printf("Nothing from the other computer for %d seconds: waiting for it (up to %d seconds)...\n",
					NL_QUIET_MS / 1000, NL_TIMEOUT_MS / 1000);
			}
			if (now - nl.last_recv > NL_TIMEOUT_MS)
			{
				printf("Nothing from the other computer for %d seconds: link lost.\n", NL_TIMEOUT_MS / 1000);
				stop_requested = 1;
			}
		}
		NL_UNLOCK();
	}
	nl_net_done = 1;
}

#ifdef _WIN32
static DWORD WINAPI	NlNetThreadMain(LPVOID arg)
{
	(void)arg;
	NlNetLoop();
	return (0);
}
#else
static void	*NlNetThreadMain(void *arg)
{
	(void)arg;
	NlNetLoop();
	return (NULL);
}
#endif

// UDP socket: bound to the port (host), or to any port and the host's address known (guest)
static bool	NlOpen(const char *peer, bool *host)
{
	char				name[256], *colon;
	struct sockaddr_in	addr;
	struct hostent		*he;
	int					port;
#ifdef _WIN32
	WSADATA				wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	strncpy(name, peer, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if (!(colon = strrchr(name, ':')) || ((port = atoi(colon + 1)) <= 0) || (port > 65535))
	{
		printf("-netlink needs :port (host) or host:port (guest), e.g. -netlink :7010 or -netlink 1.2.3.4:7010\n");
		return (false);
	}
	*colon = 0;
	*host = !name[0];
	if ((nl.s = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
		return (false);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(*host ? port : 0);
	if (bind(nl.s, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		printf("Can't use the UDP port %d (used by another program?).\n", port);
		return (false);
	}
	if (!*host)
	{
		if (!(he = gethostbyname(name)))
		{
			printf("Unknown host %s.\n", name);
			return (false);
		}
		memset(&nl.peer, 0, sizeof(nl.peer));
		nl.peer.sin_family = AF_INET;
		nl.peer.sin_port = htons(port);
		memcpy(&nl.peer.sin_addr, he->h_addr_list[0], 4);
	}
	return (true);
}

int	UsbNetLink(char *name, char *peer, const char *key, int delay, int jitter, int loss)
{
	static LinkEnd	a;
	static Queue	in, out;
	TtyThread		t = {&a, &in, &out, 0, 0, NlFromConsole};
	bool			host;
	double			hello = 0, second;
	int				i, ret = 1;
#ifdef _WIN32
	HANDLE			ht, hn;
#else
	pthread_t		ht, hn;
#endif

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!NlSetKey(key))
	{
		printf("-netkey needs the 32 hex digits of the game's link key.\n");
		return (1);
	}
	a.handle = USB_GetHandle();
	a.name = name;
	ps1_program_running = true;		// Only the kernel hook answers
	usb_big_packet_size = 0;
	usb_quiet = true;
	start_quiet = true;
	for (i = 0; (i < 10) && !Start(&a); i++)	// (the program may still be starting)
		NetSleepMs(500);
	start_quiet = false;
	if ((i == 10) && !Start(&a))
		return (1);
	memset(&nl, 0, sizeof(nl));
	nl.kind = a.kind;
	nl.in = &in;
	nl.out = &out;
	nl.delay = delay;
	nl.jitter = jitter;
	nl.loss = loss;
	srand((unsigned int)time(NULL) ^ (unsigned int)(NlNow() * 1000));
	nl.session = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)NlNow();
	if (!NlOpen(peer, &host))
		return (1);
	nl.guest = !host;
	nl.relay = nl.peer;		// (a guest's first address: the host, or the lobby's relay)
	NlFineTimer();
#ifdef _WIN32
	InitializeCriticalSection(&queue_lock);
	InitializeCriticalSection(&nl_lock);
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
#else
	signal(SIGINT, OnInterrupt);
#endif
	if (host)
		printf("Waiting for the other computer on UDP port %s (Ctrl+C to stop)...\n", strrchr(peer, ':') + 1);
	else
		printf("Connecting to %s (Ctrl+C to stop)...\n", peer);
	if (delay || jitter || loss)
		printf("Test: packets sent delayed by %d ms (+0-%d ms), %d%% dropped\n", delay, jitter, loss);
	nl_net_done = 0;
#ifdef _WIN32
	hn = CreateThread(NULL, 0, NlNetThreadMain, NULL, 0, NULL);
#else
	pthread_create(&hn, NULL, NlNetThreadMain, NULL);
#endif
	// Until linked: the guest says hello every 250 ms (the console's bytes are kept by its driver meanwhile)
	while (!nl.connected && !StopRequested())
	{
		if (!host && (NlNow() - hello > 250))
		{
			NL_LOCK();
			NlSendData(NL_HELLO);
			NL_UNLOCK();
			hello = NlNow();
		}
		NetSleepMs(10);
	}
	if (!nl.connected)
		goto stop;
	// Bytes sent by the console while waiting are dropped (as a cable not plugged yet)
	if (!Start(&a))
		goto stop;
#ifdef _WIN32
	ht = CreateThread(NULL, 0, (a.kind == END_TTY) ? TtyThreadMain : RingsThreadMain, &t, 0, NULL);
#else
	pthread_create(&ht, NULL, (a.kind == END_TTY) ? TtyThreadMain : RingsThreadMain, &t);
#endif
	for (second = NlNow(); !StopRequested() && !t.failed; )
	{
		NetSleepMs(20);
		if (NlNow() - second >= 1000)
		{
			printf("netlink: rtt %.1f ms (min %.1f)  units %u out %u in  packets %u out %u in  resent %u  "
				"%u exchanges/s\n", nl.rtt, nl.rtt_min, nl.units_out, nl.units_in, nl.packets_out, nl.packets_in,
				nl.resent, t.turns);
			nl.units_out = nl.units_in = nl.packets_out = nl.packets_in = nl.resent = t.turns = 0;
			second = NlNow();
		}
	}
	stop_requested = 1;
#ifdef _WIN32
	WaitForSingleObject(ht, INFINITE);
#else
	pthread_join(ht, NULL);
#endif
	ret = t.failed ? 1 : 0;
	if (t.failed)
		printf("USB error.\n");

stop:
	stop_requested = 1;
#ifdef _WIN32
	WaitForSingleObject(hn, INFINITE);
#else
	pthread_join(hn, NULL);
#endif
	if (nl.connected)		// (sent at once, not through the delay line of -netdelay)
	{
		nl.delay = nl.jitter = nl.loss = 0;
		for (i = 0; i < 3; i++)
			NlSendData(NL_BYE);
	}
	CloseSocket(nl.s);
	printf("Link stopped.\n");
	return (ret);
}
