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
	bool		tx_tail_changed, rx_head_changed;
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

// Rings read/written with the kernel hook's exchange ('X'), not D/U: one USB write that the PS1 never waits in for the
// computer (D/U wait for its acknowledgements, the program stopped meanwhile: a few milliseconds each, the program's
// frames slowed by each turn). A packet received wrong: transferred again.
#define	XFER_PACKET		2048		// (MAX_PACKET_SIZE: the exchange's packets)

static bool	Xfer(LinkEnd *e, bool write, uint32_t adrs, uint8_t *buf, uint32_t n)
{
	uint32_t	part, errors;
	int			tries;

	for (; n; adrs += part, buf += part, n -= part)
	{
		part = (n > XFER_PACKET) ? XFER_PACKET : n;
		for (tries = 0; ; tries++)
		{
			errors = link_crc_errors;
			if (!USB_Exchange(e->handle, adrs, write ? buf : NULL, write ? part : 0, write ? NULL : buf,
				write ? 0 : part))
				return (false);
			if (link_crc_errors == errors)
				break;
			if (tries == 2)
				return (false);
		}
	}
	return (true);
}

// Ring piece(s) from/to the computer buffer (the ring may wrap)
static bool	RingAccess(LinkEnd *e, bool write, uint32_t ring_adrs, uint32_t index, uint8_t *buf, uint32_t n)
{
	uint32_t	pos = index & (e->ring - 1), first = (n < e->ring - pos) ? n : e->ring - pos;

	if (!Xfer(e, write, ring_adrs + pos, buf, first))
		return (false);
	return ((n == first) || Xfer(e, write, ring_adrs, buf + first, n - first));
}

// State of the console (indexes, and the bytes to send for a tty driver)
static bool	Fetch(LinkEnd *e)
{
	Use(e);
	if (e->kind == END_TTY)
		return (USB_Process('D', e->ps1, e->tty_adrs + TTY_PC_PART, TTY_PS1_PART));
	return (Xfer(e, false, e->base, (uint8_t *)&e->h, offsetof(UsbLink, tx)));
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
	if (!RingAccess(e, false, LINK_TX(e), e->tx_tail, buf, n))
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
	if (!RingAccess(e, true, LINK_RX(e), e->rx_head, buf, n))
		return (false);
	e->rx_head += n;		// After the bytes (written by Commit)
	e->bytes += n;
	e->rx_head_changed = true;
	return (true);
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
	if ((e->kind == END_RINGS) && (e->tx_tail_changed || e->rx_head_changed))
	{
		uint32_t	both[2] = {e->tx_tail, e->rx_head};	// (tx_tail and rx_head follow each other: one transfer)

		e->tx_tail_changed = e->rx_head_changed = false;
		return (Xfer(e, true, LINK_FIELD(e, tx_tail), (uint8_t *)both, sizeof(both)));
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

typedef bool	(*ExchangeFn)(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata,
	uint32_t rsize);

// One turn of a console: the units for it put in its receive buffer, one exchange, the units it sent queued
static bool	TtyTurn(TtyThread *t, ExchangeFn exchange)
{
	LinkEnd		*e = t->e;
	uint8_t		buf[TTY_TX_SIZE], unit[TTY_TX_SIZE];
	uint16_t	units[TTY_UNITS];
	uint32_t	n, i, k, size;

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
	if (!exchange(e->handle, e->tty_adrs, e->pc, TTY_PC_PART, e->ps1, TTY_PS1_PART))
		return (false);
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
	t->turns++;
	return (true);
}

static void	TtyLoop(TtyThread *t)
{
	while (!stop_requested)
		if (!TtyTurn(t, USB_Exchange))
		{
			t->failed = 1;
			return;
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
			printf("%s -> %s %u B/s   %s -> %s %u B/s   %u/%u exchanges/s", a->name, b->name, b->bytes, b->name,
				a->name, a->bytes, ta.turns, tb.turns);
			if (link_crc_errors)
				printf("   %u CRC errors", link_crc_errors);
			printf("\n");
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
// Relay of a debug session ("-gdb -relay SERIAL2", gdbserver.c): the other console's turns in a thread (its own
// cartridge), the debugged one's taken by the GDB server between its own USB transfers (RelayTurn, with an exchange
// that keeps the stop notifications for it). Both programs with the USB link tty driver (-c link).

static LinkEnd		dbg_a, dbg_b;
static Queue		dbg_ab, dbg_ba;
static TtyThread	dbg_ta, dbg_tb;
static bool			dbg_on;
#ifdef _WIN32
static HANDLE		dbg_thread;
#else
static pthread_t	dbg_thread;
#endif

bool	RelayBegin(char *first_name, char *second_serial)
{
	bool	ok;

	dbg_a.handle = USB_GetHandle();
	dbg_a.name = first_name;
	dbg_b.name = second_serial;
	if (!(dbg_b.handle = USB_OpenOther(second_serial)))
	{
		printf("Can't open the cartridge %s for the USB link relay (see -l).\n", second_serial);
		return (false);
	}
	Use(&dbg_b);
	USB_DetectEscaped();
	ok = Start(&dbg_a) && Start(&dbg_b);
	Use(&dbg_a);					// (the debugged cartridge: the GDB server's)
	if (ok && ((dbg_a.kind != END_TTY) || (dbg_b.kind != END_TTY)))
	{
		printf("The relay of a debug session needs both programs with the USB link tty driver (-c link).\n");
		ok = false;
	}
	if (!ok)
	{
		USB_CloseOther(dbg_b.handle);
		return (false);
	}
	dbg_ta = (TtyThread){&dbg_a, &dbg_ba, &dbg_ab, 0, 0};
	dbg_tb = (TtyThread){&dbg_b, &dbg_ab, &dbg_ba, 0, 0};
	stop_requested = 0;
#ifdef _WIN32
	InitializeCriticalSection(&queue_lock);
	dbg_thread = CreateThread(NULL, 0, TtyThreadMain, &dbg_tb, 0, NULL);
#else
	pthread_create(&dbg_thread, NULL, TtyThreadMain, &dbg_tb);
#endif
	dbg_on = true;
	printf("USB link relay with %s, in the debug session\n", second_serial);
	return (true);
}

// The debugged console's turn (the program running or stopped: the monitor answers the exchange too)
bool	RelayTurn(void)
{
	if (!dbg_on)
		return (true);
	if (dbg_tb.failed)
	{
		printf("USB link relay: USB error (%s), stopped.\n", dbg_b.name);
		RelayEnd();
		return (false);
	}
	return (TtyTurn(&dbg_ta, USB_ExchangeServed));
}

void	RelayEnd(void)
{
	if (!dbg_on)
		return;
	dbg_on = false;
	stop_requested = 1;
#ifdef _WIN32
	WaitForSingleObject(dbg_thread, INFINITE);
#else
	pthread_join(dbg_thread, NULL);
#endif
	USB_CloseOther(dbg_b.handle);
}

/****************************************/
// -net: the USB link of a console connected to a TCP server (e.g. a game server on the computer)

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
