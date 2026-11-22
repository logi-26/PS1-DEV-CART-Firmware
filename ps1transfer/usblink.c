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
#include <sched.h>
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

int		PC_InputState(void);	// main.c: the standard input taken by the program's "@input" (-1: no, 0: yes, 1: stop)

// Ctrl+C, or a line / end of file on the standard input when it is a pipe (GUI). Taken by the program's "@input":
// its lines are the program's, only the end of file of a pipe stops (the GUI writes "quit" then closes it).
static bool	StopRequested(void)
{
#ifdef _WIN32
	HANDLE	h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	avail = 0;
#else
	fd_set			fds;
	struct timeval	tv = {0, 0};
#endif
	int		input = PC_InputState();

	if (input >= 0)
	{
		if (input)
			stop_requested = 1;
		return (stop_requested != 0);
	}
#ifdef _WIN32
	if ((h != INVALID_HANDLE_VALUE) && (GetFileType(h) == FILE_TYPE_PIPE) &&
		(!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail))
		stop_requested = 1;
#else
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

// (the flag only: USB_ReadFrom's patient wait ends when a stop is asked, see usb_patient)
int	LinkStopFlag(void)
{
	return (stop_requested);
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

/****************************************/
// -vlink: the virtual link cable of Red Alert Retaliation (../ps1-linkplay/virtual): its driver, put in each game by
// ps1-linkplay/virtual/usb_link.py, passes the link traffic through rings in the kernel hook's cheat table space; carried
// here between the two consoles, one thread per cartridge, one exchange ('X') per turn: the computer's part written
// (its counters and line changes, after the bytes it gives, if any), the console's part read (its counters and the
// start of its send area) in one transfer: 95 bytes when nothing moves (347 with the rings before: each byte costs the
// console about 1 us, the game stopped when the exchange is answered in an interrupt, and the turn ~2.6 us).
// Layout (vlink.c VLinkBuf): magic, rx[128], the computer's counters (36), the console's (40), tx[128], stats.
// rx: bytes given only when the console has read all it had, ending where the area ends; tx: the console's bytes start
// at the area's beginning (tx_base) once all were taken. The streams carry records: [0, len lo, len hi, bytes] data, [1, lines] a line change, [2, n lo,
// n hi] credit (a read wants n bytes), [3, n lo, n hi] credit taken back; data and lines go into the other console's
// rings, credits change what it may send (see vlink.c).

#define	VL_BUF		0x8000D810
#define	VL_RX		128
#define	VL_TX		128
#define	VL_LN		16
#define	VL_MAGIC	0x4B4E4C56
#define	VL_PC		36				// rx_head, tx_tail, ln_head, credit_in, abort, ln_ring[16] (after rx[])
#define	VL_PC_ADRS	(VL_BUF + 4 + VL_RX)
#define	VL_ABORT_MS	1500			// A console waiting that long with nothing moving for it: its wait ended (abort)
#define	VL_WAIT_SEEN_MS	200
#define	VL_GONE_MS	100				// Then (the other side gone, until something comes from it): its next waits sooner,
									// as the real port with no partner (the game retries many times before it gives up)
#define	VL_PS1		40				// rx_tail, tx_head, ln_tail, my_lines, saved, peer_lines, peer_seen, credit_used,
									// waiting, tx_base (then tx[], stats[8])
// The console's part read with as much of its send area as needed, so that the answer ('G', the written packet's CRC,
// the bytes, their CRC) fills whole USB packets (62 bytes of data each for the FT245R): a last partial packet waits for
// the latency timer (1 ms). 59, 121 or 183 bytes (the last: 15 bytes after the area, its stats: not used)
#define	VL_USB_DATA	62
#define	VL_PS1_READ_MAX	(3 * VL_USB_DATA - 3)
#define	VL_STREAM	65536			// Power of 2: records on their way to the other console
#define	VL_FRAME	0x801B4EB8		// The game's frame counter (25 a second)
// Each game frame both consoles exchange a 6-byte header (BD DA len AF DE) twice, the second always the same as the
// first, then the payload: 6 one-way messages, each crossing this computer. The reader's next 6-byte read after a
// header is answered at once with a copy of it (its request still goes to the writer as usual), and the writer's
// second header, when it comes, is dropped (checked equal): nearly 4 crossings a frame instead of 6. Only on request:
// given in advance (BILL's first version), a copy nobody asked for (leaving a link game) shifted every message after
// it, and the game, reading garbage, wrote over its own interrupt callbacks.
#define	VL_COPY_MS	200				// A copy kept that long for the reader's request
// The payloads too (VLINK_TUNE "pcopy 1"): every message is sent twice, identical (header, header, payload, payload:
// traced 2026-11-13, 860 of 860 pairs equal, orders too). The reader's request for as many bytes as the message it was
// just given is answered with a copy, and the writer's repeat dropped when it comes (checked equal: dup_bad otherwise)
#define	VL_COPY_MAX	96
static volatile int		vl_pcopy = 1;	// (2026-11-13: 23.8 -> 24.9 fps, no mismatch)
#define	VL_IS_HEADER(_p_)	(((_p_)[0] == 0xBD) && ((_p_)[1] == 0xDA) && ((_p_)[4] == 0xAF) && ((_p_)[5] == 0xDE))

#define	VL_MARKS	8192			// Power of 2
static volatile uint32_t	vl_delay_us;

typedef struct
{
	uint8_t				buf[VL_STREAM];
	volatile uint32_t	head, tail;
	// Timing (VTIME lines): the first records put in while none were stamped, when they came from the console
	// Network delay simulated (VLINK_TUNE "delay_us N"): where the stream's head was when (marks written as records
	// go in); the other side only takes what went in delay_us ago or more (ok_pos)
	volatile uint32_t	mk_pos[VL_MARKS];
	volatile uint64_t	mk_us[VL_MARKS];
	volatile uint32_t	mk_head, mk_tail, ok_pos;
	volatile int		stamped;
	volatile uint32_t	stamp_pos;
	volatile uint64_t	stamp_us;
} VlStream;

// Timing, per console, reset each second (VTIME lines): exchanges served while it waited in the driver (its waiting
// flag up in the answer) or not (another driver call or an interrupt), each one's time; crossings: records taken from
// the other console until the exchange that gave them to this one ended
typedef struct
{
	uint32_t	n, max;
	uint64_t	sum;
} VlTime;

static void	VlTimeAdd(volatile VlTime *t, uint32_t us)
{
	t->n++;
	t->sum += us;
	if (us > t->max)
		t->max = us;
}

static uint64_t	VlUs(void);

// Trace (VLINK_TUNE "trace S": S seconds of events, then written to vlink_trace.txt in the current folder): each
// exchange and record with its time. Kinds: X exchange ended (a = receive bytes given in it, b = the console's waiting
// flag), D data written by the console (a = length), C a read's request (a = bytes), K a request given to the console
// (a = bytes), H a header copy given
#define	VL_TRACE_MAX	400000
typedef struct
{
	uint32_t	us;
	uint8_t		end, kind;
	uint16_t	a, b;
} VlEv;
static VlEv				vl_trace[VL_TRACE_MAX];
static volatile long	vl_trace_n;
static volatile int		vl_trace_on, vl_fake;
// Line changes held back from a console that isn't in link mode (its own DTR and RTS low: at its menus): the last one
// given once steady for VL_HOLD_MS, or at once when it raises a line (VLINK_TUNE "hold 0/1"). Short pulses are not
// shown to it then. BEN froze at its main menu each time BILL's game, left alone,
// finished timing out and its last line changes reached it: the driver shows every change at least once (the link
// page's one-frame RTS pulse), so BEN's menu may have seen a raised line and started linking there
static volatile int		vl_hold = 0;	// (off: it held back line changes in the network handshake; the freezes it was for were the relay giving up)
#define	VL_HOLD_MS	500
static volatile uint64_t	vl_trace_until, vl_trace_t0;

// The contents of the data records written by each console while tracing (vlink_trace.txt "M" lines: hex bytes)
#define	VL_MSG_MAX		60000
#define	VL_MSG_BYTES	96
typedef struct
{
	uint32_t	us;
	uint8_t		end;
	uint16_t	len;
	uint8_t		data[VL_MSG_BYTES];
} VlMsg;
static VlMsg			vl_msgs[VL_MSG_MAX];
static volatile long	vl_msg_n;

static void	VlTraceMsg(int end, const uint8_t *data, uint32_t len);

static void	VlTrace(int end, int kind, uint32_t a, uint32_t b)
{
	long	i;

	if (!vl_trace_on)
		return;
	i = __sync_fetch_and_add(&vl_trace_n, 1) % VL_TRACE_MAX;	// (a ring: the newest kept)
	vl_trace[i].us = (uint32_t)(VlUs() - vl_trace_t0);
	vl_trace[i].end = end;
	vl_trace[i].kind = kind;
	vl_trace[i].a = a;
	vl_trace[i].b = b;
}

typedef struct VlEnd_s
{
	void				*handle;
	char				*name;
	uint8_t				area[VL_RX + VL_PC], *pc;	// The receive area (its last rx_new bytes to be written) and the
	uint8_t				ps1[VL_PS1_READ_MAX];		// computer's counters; the console's counters and send area
	uint32_t			rx_new;			// Bytes put at the end of the receive area for the next exchange
	uint32_t			ps1_read;		// Bytes read of the console's part in the last exchange
	VlStream			*in, *out;		// Records for this console, from it
	uint32_t			data_left;		// Bytes of the data record being put in its receive ring
	volatile int		failed, reset;
	volatile uint32_t	turns, bytes_in, bytes_out, frame, aborts;
	volatile uint32_t	frame_ms;		// Last time its game frame counter was seen moving (in a game: payload copies)
	int					copy_hdr;		// The copy kept is a header's
	uint32_t			moved_ms;		// Last time something was carried for or from it (abort)
	int					gone;			// Its last wait ended by an abort, nothing from the other side since
	// Second headers (see VL_IS_HEADER): the other's last header (copy_pending: this console may ask for its copy;
	// copy_used: given, the other's next header is dropped), this console's records parsed as they go out
	int					out_state, out_need, out_credit;
	// Early credit for the responder's second header (VLINK_TUNE "fake 1"): each frame the responder reads the
	// initiator's first header, writes its own, reads the second (given the copy here at once), then writes its second
	// header, which is dropped (the other side gets a copy). That write waited for the other's request (credit), which
	// comes only once the other has the first header: ~8 ms a frame. So: when this console gets the copy having written
	// one header (the responder), it gets 6 bytes of credit at once (owed: taken from the other's next request), and the
	// other console drops its next header (drop_hdr: before its copy is asked for, h2_gone then; or after, copy_used)
	struct VlEnd_s		*peer;
	int					out_len, out_pos, hdrs_written;
	uint8_t				out_msg[VL_MSG_BYTES];		// (trace: the data record's bytes)
	uint8_t				out_hdr[6];
	uint32_t			hdr_ms, owed_ms, drop_ms;
	int32_t				owed;
	volatile int		drop_hdr;
	int					h2_gone;
	int					held_line;		// The other's last line change held back (-1 none): see vl_hold
	int					sym_on;			// The symmetric exchange's instructions written in its game (see VlSym)
	int					net;			// -vlinkhost / -vlinkjoin: the other console is on another computer
	uint32_t			fps;			// Its game frames in the last second (the other computer's: its status)
	// Double delivery during the symmetric exchange: every in-game message is sent twice, identical; each one is given
	// with its copy at once (the console's two reads served by one crossing) and the sender's repeat dropped: 2
	// network delays a frame instead of 4 (measured 15.7 frames a second at 10 ms one-way with 4). Started at a
	// frame's first header (after a payload), so as to stay on the pairs.
	int					dbl_on, dbl_wait, last_rx_len;
	uint32_t			dbl_len;
	uint8_t				dbl_msg[VL_COPY_MAX];
	uint32_t			held_ms;
	int					copy_pending, copy_used;
	uint32_t			copy_ms;
	uint8_t				in_hdr[VL_COPY_MAX];	// The last message given to it (a copy of it may be asked for)
	uint32_t			copy_len;
	volatile uint32_t	dup_ok, dup_bad;
	int					pc_dirty;		// Its part changed since the last exchange (to be written at once)
	uint64_t			wait_us;		// (the same in microseconds: pacing)
	uint32_t			wait_ms;		// Last time it was seen waiting (its flag is up only during half of its USB
										// services in a wait: "waiting" = seen in the last VL_WAIT_SEEN_MS)
	uint64_t			deliver_us;		// Timing: stamp of records given in the coming exchange (0 none)
	int					id;				// 0 the first console, 1 the other (trace)
	volatile VlTime		t_wait, t_other, t_cross;
	// Timing of its reads that waited (asked the other side for bytes): from their request taken here until the
	// exchange that gave the last of those bytes ended (the time the console sits in the driver for the other side)
	uint64_t			req_us;
	int32_t				req_left;
	int					req_done;
	volatile VlTime		t_read;
	// Net Play's automatic link play (see VlAuto): the run asked last, the step reached, when it started, last check
	int					stall_said;		// "the other console is busy" printed for this wait (see VlPatience)
	uint32_t			watch_val[8];	// (VlWatch: the game's words last read, VL_WATCH_MAX)
	uint8_t				watch_seen[8];
	unsigned int		auto_seen[2];	// (VL_AUTO_LISTS)
	int					auto_on, auto_step, auto_armed, auto_list;
	uint32_t			auto_ms, auto_check_ms, auto_until;
} VlEnd;

static uint32_t	VlMs(void)
{
#ifdef _WIN32
	return (GetTickCount());
#else
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000));
#endif
}

#define	VL_IN_AVAIL(_e_)		((_e_)->in->head - (_e_)->in->tail)
#define	VL_IN_BYTE(_e_, _i_)	((_e_)->in->buf[((_e_)->in->tail + (_i_)) & (VL_STREAM - 1)])

// n bytes put at the end of the receive area after those already put this turn (from the stream when b is NULL)
static void	VlRxAdd(VlEnd *e, const uint8_t *b, uint32_t n)
{
	uint32_t	k;

	memmove(e->area + VL_RX - e->rx_new - n, e->area + VL_RX - e->rx_new, e->rx_new);
	for (k = 0; k < n; k++)
		e->area[VL_RX - n + k] = b ? b[k] : VL_IN_BYTE(e, k);
	e->rx_new += n;
}

// Where the other side may take the stream up to (all of it, or what went in delay_us ago: see mk_pos)
static uint32_t	VlDeliverable(VlStream *s)
{
	uint64_t	cut = VlUs() - vl_delay_us;

	if (!vl_delay_us)		// (kept at the end: never behind what was taken when a delay is set)
	{
		s->mk_tail = s->mk_head;
		return (s->ok_pos = s->head);
	}
	while ((s->mk_tail != s->mk_head) && (s->mk_us[s->mk_tail & (VL_MARKS - 1)] <= cut))
	{
		s->ok_pos = s->mk_pos[s->mk_tail & (VL_MARKS - 1)];
		s->mk_tail++;
	}
	return ((int32_t)(s->ok_pos - s->tail) > 0) ? s->ok_pos : s->tail;
}

// The other console's records, as many as its receive area and line ring take
static void	VlIncoming(VlEnd *e)
{
	uint32_t	avail, n, k, lim, rx_head = U32(e->pc), ln_head = U32(e->pc + 8), credit_in = U32(e->pc + 12);
	int			sym;
	uint32_t	rx_head0 = rx_head, ln_head0 = ln_head, credit_in0 = credit_in;
	uint32_t	rx_tail = U32(e->ps1), ln_tail = U32(e->ps1 + 8), credit_used = U32(e->ps1 + 28);

	LOCK();
	lim = VlDeliverable(e->in);
	avail = lim - e->in->tail;
	UNLOCK();
	if ((e->held_line >= 0) && (VlMs() - e->held_ms > VL_HOLD_MS) && (ln_head - ln_tail < VL_LN))
	{
		e->pc[20 + (ln_head & (VL_LN - 1))] = e->held_line;
		ln_head++;
		printf("VLINK %s: the other's lines %u given (steady)\n", e->name, e->held_line);
		e->held_line = -1;
	}
	if (avail)
		e->gone = 0;
	while (avail)
	{
		if (e->data_left)
		{
			// (only when the console has read all it was given: rx_new bytes put this turn come on top)
			n = (rx_head - e->rx_new == rx_tail) ? VL_RX - e->rx_new : 0;
			if (n > e->data_left)
				n = e->data_left;
			if (n > avail)
				n = avail;
			if (!n)
				break;
			VlRxAdd(e, NULL, n);
			rx_head += n;
			e->data_left -= n;
			e->bytes_in += n;
			if (e->req_us && ((e->req_left -= n) <= 0))
				e->req_done = 1;
		}
		else if (VL_IN_BYTE(e, 0) == 0)
		{
			uint8_t	h[VL_COPY_MAX];
			int		is_hdr = 0;

			if (avail < 3)
				break;
			n = VL_IN_BYTE(e, 1) | (VL_IN_BYTE(e, 2) << 8);
			sym = e->sym_on && e->peer->sym_on;
			if (!sym)
				e->dbl_on = e->dbl_wait = 0;
			if ((n == 6) || ((vl_pcopy || sym) && n && (n <= VL_COPY_MAX)))	// (a message that may be copied: whole)
			{
				if (avail < 3 + n)
					break;
				for (k = 0; k < n; k++)
					h[k] = VL_IN_BYTE(e, 3 + k);
				is_hdr = (n == 6) && VL_IS_HEADER(h);
			}
			if (e->drop_hdr && (VlMs() - e->drop_ms >= VL_COPY_MS))
				e->drop_hdr = 0;
			if (sym && e->dbl_wait)		// (double delivery: the sender's repeat, given already)
			{
				e->dbl_wait = 0;
				if ((n == e->dbl_len) && !memcmp(h, e->dbl_msg, n))
				{
					e->dup_ok++;
					e->last_rx_len = n;
					VlTrace(e->id, 'P', n, 0);
					LOCK();
					e->in->tail += n + 3;
					avail = lim - e->in->tail;
					UNLOCK();
					continue;
				}
				e->dup_bad++;
				e->dbl_on = 0;
				printf("VLINK %s: a repeat differed from the message given twice (%u bytes): double delivery off\n",
					e->name, (unsigned int)n);
			}
			if (sym && !e->dbl_on && is_hdr && (e->last_rx_len > 6))	// (a frame's first header: on the pairs)
				e->dbl_on = 1;
			// Given twice at once, into an empty area: only messages that fit there twice (64 bytes: the receive
			// area is VL_RX). A longer one (a frame with orders: 84 bytes, 4 orders) is given as usual, once per
			// read, and so is its repeat (2026-11-14 13:04: it waited for room that never came, both consoles
			// waited for each other, the game was lost after 4 minutes)
			if (sym && e->dbl_on && n && (n <= VL_COPY_MAX) && (2 * n <= VL_RX))
			{
				if (!((rx_head - e->rx_new == rx_tail) && (e->rx_new + 2 * n <= VL_RX)))
					break;
				VlRxAdd(e, h, n);
				VlRxAdd(e, h, n);
				rx_head += 2 * n;
				e->bytes_in += 2 * n;
				if (e->req_us && ((e->req_left -= 2 * n) <= 0))
					e->req_done = 1;
				memcpy(e->dbl_msg, h, n);
				e->dbl_len = n;
				e->dbl_wait = 1;
				e->last_rx_len = n;
				VlTrace(e->id, 'H', n, 1);
				LOCK();
				e->in->tail += n + 3;
				avail = lim - e->in->tail;
				UNLOCK();
				continue;
			}
			e->last_rx_len = n;
			if (e->copy_used && (n == e->copy_len) && (is_hdr == e->copy_hdr))	// The repeat, its copy given: dropped
			{
				if (memcmp(h, e->in_hdr, n))
				{
					e->dup_bad++;
					if (!is_hdr && vl_pcopy)	// (a wrong guess: the copy given was stale; not again)
					{
						vl_pcopy = 0;
						printf("VLINK %s: a payload repeat differed from its copy: payload copies switched off\n",
							e->name);
					}
				}
				else
					e->dup_ok++;
				e->copy_used = 0;
				e->drop_hdr = 0;
				VlTrace(e->id, 'P', n, 0);
				n += 3;
			}
			else if (is_hdr && e->drop_hdr)	// (the second header, its copy to be given: see drop_hdr)
			{
				if (memcmp(h, e->in_hdr, 6))
					e->dup_bad++;
				else
					e->dup_ok++;
				e->h2_gone = 1;
				e->drop_hdr = 0;
				VlTrace(e->id, 'P', 6, 0);
				n = 9;
			}
			else
			{
				e->copy_used = 0;
				// It may ask for its copy: a header's, or in a game a payload's (both games' frame counters moving:
				// not at the setup screen, whose messages aren't all repeated: copies given there crashed BILL)
				// (none while the symmetric exchange is on: the writer's real repeat can come before the reader's
				// request, and both it and the copy were given: 6 bytes too many, BEN's game stopped)
				e->copy_pending = !e->sym_on && !e->peer->sym_on && (is_hdr || (vl_pcopy && n && (n <= VL_COPY_MAX) &&
					(VlMs() - e->frame_ms < 2500) && (VlMs() - e->peer->frame_ms < 2500)));
				e->copy_hdr = is_hdr;
				if (e->copy_pending)
				{
					memcpy(e->in_hdr, h, n);
					e->copy_len = n;
					e->copy_ms = VlMs();
				}
				e->data_left = n;
				n = 3;
			}
		}
		else if (VL_IN_BYTE(e, 0) == 1)
		{
			if (avail < 2)
				break;
			if (vl_hold && !(U32(e->ps1 + 12) & 3))		// (not in link mode: held, see vl_hold)
			{
				e->held_line = VL_IN_BYTE(e, 1);
				e->held_ms = VlMs();
				printf("VLINK %s: the other's lines %u held (its own 0)\n", e->name, e->held_line);
			}
			else
			{
				if (ln_head - ln_tail >= VL_LN)
					break;
				e->pc[20 + (ln_head & (VL_LN - 1))] = VL_IN_BYTE(e, 1);
				ln_head++;
				printf("VLINK %s: the other's lines %u given\n", e->name, VL_IN_BYTE(e, 1));
			}
			n = 2;
		}
		else if ((VL_IN_BYTE(e, 0) == 2) || (VL_IN_BYTE(e, 0) == 3))
		{
			if (avail < 3)
				break;
			k = VL_IN_BYTE(e, 1) | (VL_IN_BYTE(e, 2) << 8);
			if (VL_IN_BYTE(e, 0) == 2)
			{
				if (e->owed && (VlMs() - e->owed_ms > 1000))
					e->owed = 0;
				if (e->owed)		// (given early: see owed)
				{
					uint32_t	t = ((int32_t)k < e->owed) ? k : (uint32_t)e->owed;

					k -= t;
					e->owed -= t;
					VlTrace(e->id, 'S', t, 0);
				}
				credit_in += k;
				VlTrace(e->id, 'K', k, 0);
			}
			else
				credit_in = ((int32_t)(credit_in - k - credit_used) > 0) ? credit_in - k : credit_used;
			n = 3;
		}
		else
		{
			printf("%s: bad record from the other console (%u): stream dropped\n", e->name, VL_IN_BYTE(e, 0));
			n = avail;
		}
		LOCK();
		e->in->tail += n;
		avail = lim - e->in->tail;
		if (e->in->stamped && ((int32_t)(e->in->tail - e->in->stamp_pos) > 0))
		{
			if (!e->deliver_us)
				e->deliver_us = e->in->stamp_us;
			e->in->stamped = 0;
		}
		UNLOCK();
	}
	U32(e->pc) = rx_head;
	U32(e->pc + 8) = ln_head;
	U32(e->pc + 12) = credit_in;
	if ((rx_head != rx_head0) || (ln_head != ln_head0) || (credit_in != credit_in0))
		e->moved_ms = VlMs();
}

// This console's records as they go out: a read of 6 bytes asked for (credit) just after the other's header came: the
// copy of that header given at once (the request still goes to the other console: its second header is dropped)
static void	VlParseOut(VlEnd *e, uint8_t b)
{
	uint32_t	rx_head = U32(e->pc), rx_tail = U32(e->ps1);

	switch (e->out_state)
	{
		case 0:		// Record type
			if (b == 0)
				e->out_state = 1;
			else if (b == 2)
				e->out_state = 10;
			else if (b == 1)
				e->out_state = 13;
			else
			{
				e->out_state = 9;
				e->out_need = 2;
			}
			break;
		case 13:	// Its own lines: the other's held back given now if it raises one (see vl_hold)
			e->out_state = 0;
			printf("VLINK %s: sets its lines %u\n", e->name, b);
			if ((b & 3) && (e->held_line >= 0) && (U32(e->pc + 8) - U32(e->ps1 + 8) < VL_LN))
			{
				e->pc[20 + (U32(e->pc + 8) & (VL_LN - 1))] = e->held_line;
				U32(e->pc + 8) += 1;
				e->pc_dirty = 1;
				printf("VLINK %s: the other's lines %u given (held)\n", e->name, e->held_line);
				e->held_line = -1;
			}
			break;
		case 1:		// Data length low
			e->out_need = b;
			e->out_state = 2;
			break;
		case 2:		// Data length high
			e->out_need |= b << 8;
			VlTrace(e->id, 'D', e->out_need, 0);
			e->out_len = e->out_need;
			e->out_pos = 0;
			e->out_state = e->out_need ? 12 : 0;
			break;
		case 10:	// Credit low
			e->out_credit = b;
			e->out_state = 11;
			break;
		case 11:	// Credit high: the request
			e->out_credit |= b << 8;
			e->out_state = 0;
			VlTrace(e->id, 'C', e->out_credit, 0);
			if (e->out_credit)
			{
				if (!e->req_us)
				{
					e->req_us = VlUs();
					e->req_left = 0;
				}
				e->req_left += e->out_credit;
			}
			if (e->copy_pending && (e->out_credit == e->copy_len) && (VlMs() - e->copy_ms < VL_COPY_MS) &&
				(rx_head - e->rx_new == rx_tail) && (e->rx_new + e->copy_len <= VL_RX))
			{
				VlRxAdd(e, e->in_hdr, e->copy_len);
				U32(e->pc) = rx_head + e->copy_len;
				e->bytes_in += e->copy_len;
				if (e->h2_gone)		// (its second header already dropped)
					e->copy_used = e->h2_gone = 0;
				else
					e->copy_used = 1;
				VlTrace(e->id, 'H', e->copy_len, 0);
				if ((e->copy_len == 6) && vl_fake && (e->hdrs_written == 1) && (VlMs() - e->hdr_ms < VL_COPY_MS))
				{
					U32(e->pc + 12) += 6;
					e->owed += 6;
					e->owed_ms = VlMs();
					e->peer->drop_ms = VlMs();
					e->peer->drop_hdr = 1;
					VlTrace(e->id, 'F', 6, 0);
				}
				e->pc_dirty = 1;
				if (e->req_us && ((e->req_left -= e->copy_len) <= 0))
					e->req_done = 1;
			}
			e->copy_pending = 0;
			break;
		case 12:	// Data bytes (headers written counted)
			if (e->out_pos < 6)
				e->out_hdr[e->out_pos] = b;
			if (e->out_pos < VL_MSG_BYTES)
				e->out_msg[e->out_pos] = b;
			e->out_pos++;
			if (!--e->out_need)
			{
				e->out_state = 0;
				VlTraceMsg(e->id, e->out_msg, e->out_len);
				if ((e->out_len == 6) && VL_IS_HEADER(e->out_hdr))
				{
					e->hdrs_written++;
					e->hdr_ms = VlMs();
				}
				else
					e->hdrs_written = 0;
			}
			break;
		default:	// Skipped bytes
			if (!--e->out_need)
				e->out_state = 0;
			break;
	}
}

// The console's new records (its send ring) to the other console
static void	VlOutgoing(VlEnd *e)
{
	uint32_t	tx_tail = U32(e->pc + 4), tx_head = U32(e->ps1 + 4), tx_base = U32(e->ps1 + 36), n, room, k, at;

	n = tx_head - tx_tail;
	at = tx_tail - tx_base;
	if ((n > VL_TX) || (at > VL_TX))
	{
		printf("%s: its send area reads wrong (%u bytes at %u)\n", e->name, (unsigned int)n, (unsigned int)at);
		e->failed = 1;
		return;
	}
	if (VL_PS1 + at + n > e->ps1_read)	// (only what was read: the rest next turn)
		n = (e->ps1_read > VL_PS1 + at) ? e->ps1_read - VL_PS1 - at : 0;
	LOCK();
	room = VL_STREAM - (e->out->head - e->out->tail);
	UNLOCK();
	if (n > room)
		n = room;
	for (k = 0; k < n; k++)
	{
		e->out->buf[(e->out->head + k) & (VL_STREAM - 1)] = e->ps1[VL_PS1 + at + k];
		VlParseOut(e, e->ps1[VL_PS1 + at + k]);
	}
	LOCK();
	if (n)
	{
		if (e->out->mk_head - e->out->mk_tail < VL_MARKS)
		{
			e->out->mk_pos[e->out->mk_head & (VL_MARKS - 1)] = e->out->head + n;
			e->out->mk_us[e->out->mk_head & (VL_MARKS - 1)] = VlUs();
			e->out->mk_head++;
		}
		else		// (full: the last mark moved on)
			e->out->mk_pos[(e->out->mk_head - 1) & (VL_MARKS - 1)] = e->out->head + n;
	}
	if (n && !e->out->stamped)
	{
		e->out->stamp_pos = e->out->head;
		e->out->stamp_us = VlUs();
		e->out->stamped = 1;
	}
	e->out->head += n;
	UNLOCK();
	U32(e->pc + 4) = tx_tail + n;
	e->bytes_out += n;
	if (n)
		e->pc_dirty = 1;
	if (n)
		e->moved_ms = VlMs();
}

static bool	VlRead(VlEnd *e, uint32_t adrs, uint8_t *buf, uint32_t n)
{
	uint32_t	errors = link_crc_errors;
	int			tries;

	for (tries = 0; tries < 5; tries++)
	{
		if (!USB_Exchange(e->handle, adrs, NULL, 0, buf, n))
			return (false);
		if (link_crc_errors == errors)
			return (true);
		errors = link_crc_errors;
	}
	return (false);
}

// Turns paced by what the console is doing. Each exchange costs it time: when it isn't waiting in the driver, it is
// answered in an interrupt, the game stopped meanwhile (about 350 bytes copied). So: one turn after another while it
// waits for the other side (seen waiting in the last VL_BUSY_MS: answered in the driver's wait, the game waiting
// anyway) or when something is to be given to it; otherwise one every VL_IDLE_US. (One always on its way, -vlink's
// first try, made it slower: 18 frames a second instead of 21-22.)
// (live settings: VLINK_TUNE "busy_us N", "idle_us N"; microseconds: GetTickCount only moves every ~15.6 ms)
static volatile uint32_t	vl_busy_us = 100000, vl_idle_us = 2000;	// (measured 2026-11-13: 20000/5000 23.6 fps, these 24.3)
// A console that hasn't waited in the driver for vl_quiet_us (loading a map, menus, videos): one exchange every
// vl_sleep_us only (unless something is to be given to it). Each one stops its game in an interrupt: at one every 2 ms
// BEN's map loading took ~30 s, the other console's 30 s countdown nearly or really running out ("connection lost").
// (VLINK_TUNE "sleep_us N", "quiet_us N")
static volatile uint32_t	vl_quiet_us = 1000000, vl_sleep_us = 2000;	// (quiet off: 100000 blocked the setup screen's first messages)	// (off: 20000 broke the link page handshake, and BEN's slow start was its disc reads, not this)

static uint64_t	VlUs(void)
{
#ifdef _WIN32
	LARGE_INTEGER	f, c;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return ((uint64_t)(c.QuadPart / (f.QuadPart / 1000000)));
#else
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000);
#endif
}

// Both consoles away from any link (their own lines low, no wait in the driver for vl_quiet_us: menus, videos,
// loading): exchanges every vl_sleep_us only. Either raising a line (the link page, the game's start handshake)
// brings both back to full speed at once (checking the waits alone broke the link page's handshake)
static int	VlQuiet(VlEnd *e)
{
	uint64_t	now = VlUs();

	return (!(U32(e->ps1 + 12) & 3) && !(U32(e->peer->ps1 + 12) & 3) && (now - e->wait_us > vl_quiet_us) &&
		(now - e->peer->wait_us > vl_quiet_us));
}

// The symmetric exchange (vlink.c vlink_free_write), only during a game (VLINK_TUNE "sym 1"): the game's exchange
// function also carries the setup screen's last step, which deadlocked with both consoles writing first. So its two
// instructions (VLINK_SYM_WORDS: "adrs:orig:new,..." from usb_link.py) are written into both games while both
// consoles write in-game headers (none at the setup screen), and the originals back as soon as either stops. A frame
// with one console switched and the other not can't deadlock (the switched one's write goes out unasked).
#define	VL_SYM_WORDS	4
static unsigned int		vl_join_last, vl_leave_last;	// (lobby commands in VLINK_TUNE: acted on when changed)
static volatile int		vl_lobby, vl_paired;		// -vlinklobby: connected to the lobby server, paired there
static bool	VlNetSend(int type, const uint8_t *data, uint32_t n);
static volatile int		vl_sym = 1;	// (on when the instructions are given; "sym 0" in VLINK_TUNE turns it off)
static int				vl_sym_n = -1;
static uint32_t			vl_sym_adrs[VL_SYM_WORDS], vl_sym_word[2][VL_SYM_WORDS];

static bool	VlPeerThere(VlEnd *e);
#define	VL_STALL_MS		30000		// (see VlPatience)

// The console's link game over: its game running without the link (its frame counter, read once a second, seen
// moving 1.5 s after its last in-game header, as at its menus), or no in-game header for VL_STALL_MS
static bool	VlGameLeft(VlEnd *c, uint32_t now)
{
	return ((now - c->hdr_ms > 1000) && ((int32_t)(c->frame_ms - c->hdr_ms) > 1500)) || (now - c->hdr_ms > VL_STALL_MS);
}

static void	VlSym(VlEnd *e)
{
	uint32_t	now = VlMs();
	int			want, i;
	uint8_t		w[4];

	if (vl_sym_n < 0)		// (the words, once)
	{
		char	*p = getenv("VLINK_SYM_WORDS");
		unsigned int	a, o, n;

		vl_sym_n = 0;
		while (p && (vl_sym_n < VL_SYM_WORDS) && (sscanf(p, "%x:%x:%x", &a, &o, &n) == 3))
		{
			vl_sym_adrs[vl_sym_n] = a;
			vl_sym_word[0][vl_sym_n] = o;
			vl_sym_word[1][vl_sym_n++] = n;
			p = strchr(p, ',');
			p = p ? p + 1 : NULL;
		}
	}
	// On when both consoles write in-game headers. Off only when the game ended: a console running on without the
	// link (its frame counter seen moving over a second after its last header: it left the game), no headers for
	// VL_STALL_MS, or the other side gone. Not when a console only stops for a moment (its frame counter still too):
	// the instructions were rewritten in the middle of a frame there, and the game left the match (2026-11-14 12:46,
	// after 15 minutes: BILL's frame stalled ~1 s, the exchange switched off/on/off, BILL's game quit)
	if (!e->sym_on)
		want = vl_sym && vl_sym_n && (now - e->hdr_ms < 1000) && (now - e->peer->hdr_ms < 1000);
	else
		want = vl_sym && VlPeerThere(e) && !VlGameLeft(e, now) && !VlGameLeft(e->peer, now);
	if (want == e->sym_on)
		return;
	for (i = 0; i < vl_sym_n; i++)
	{
		U32(w) = vl_sym_word[want][i];
		if (!USB_Exchange(e->handle, vl_sym_adrs[i], w, 4, NULL, 0))
			return;
	}
	e->sym_on = want;
	if (want)
		e->copy_pending = 0;		// (no copies while it is on: see copy_pending)
	printf("VLINK %s: symmetric exchange %s\n", e->name, want ? "on" : "off");
}

// Net Play's automatic link play: the Manager starts the game's link play on the console (no button pressed there),
// and the host's game with the settings chosen in the Manager. Two lists of steps from the game's patch package, each
// run on each console of this computer when asked in VLINK_TUNE (acted on when N changes):
//   "auto N V0 ... V15"      VLINK_AUTO_STEPS (-vlinkauto): the link play started (Red Alert: main menu -> LINK GAME
//                            -> country and color -> START SETUP: the console waits on the link page, its RTS up:
//                            "VLINK OFFER");
//   "hostauto N V0 ... V15"  VLINK_HOST_STEPS (-vlinkhostauto): the host's part once paired (Red Alert: the setup
//                            screen showing, the game settings written into its items, a short wait, START).
// The values: the game's options chosen in the Manager, in the order of its patch package (up to 16; missing: 0).
// Steps separated by ',', each a kind letter then its words separated by '/', ADDR=VALUE in hex (VALUE "$0" to "$15":
// the values given):
//   w  wait until all its words hold (read together);
//   a  arm: written (written back to 0 if the run fails: a driver's one-shot request, e.g. "choose LINK GAME");
//   s  write;
//   d  wait: "dMS" (hex milliseconds).
// A run always starts at the first step (not at a later "w" already holding: the game leaves a page's values in memory
// after it, Red Alert's link page "showing" at its main menu, and a second run wrote START into a page not there), and
// fails after VL_AUTO_MS (Red Alert's start: the game not at its main menu: the attract movie, a game running...).
#define	VL_AUTO_STEPS	16
#define	VL_AUTO_WORDS	12
#define	VL_AUTO_MS		60000
#define	VL_AUTO_VARS	16
#define	VL_AUTO_LISTS	3			// 0 the start, 1 the host's part, 2 kept (VlKeep: each second, not asked)
#define	VL_AUTO_RUNS	2			// (the lists run when asked)
typedef struct
{
	char		kind;
	int			n;
	uint32_t	adrs[VL_AUTO_WORDS], val[VL_AUTO_WORDS];
	signed char	var[VL_AUTO_WORDS];		// the value used ($0-$15), -1 the value written in the step
} VlAutoStep;
static VlAutoStep			vl_auto[VL_AUTO_LISTS][VL_AUTO_STEPS];
static int					vl_auto_n[VL_AUTO_LISTS] = {-1, -1, -1};
static volatile unsigned int	vl_auto_req[VL_AUTO_LISTS];
static volatile uint32_t	vl_auto_var[VL_AUTO_LISTS][VL_AUTO_VARS];
static const char			*vl_auto_env[VL_AUTO_LISTS] = {"VLINK_AUTO_STEPS", "VLINK_HOST_STEPS", "VLINK_KEEP_STEPS"};
static const char			*vl_auto_what[VL_AUTO_LISTS] = {"AUTO", "HOSTAUTO", "KEEP"};

static void	VlAutoParse(int list)
{
	char		*p = getenv(vl_auto_env[list]), *q;
	VlAutoStep	*st;
	uint32_t	a, v;
	int			*n = &vl_auto_n[list];

	*n = 0;
	while (p && *p && (*n < VL_AUTO_STEPS))
	{
		st = &vl_auto[list][*n];
		memset(st, 0, sizeof(*st));
		st->kind = *p++;
		if (!strchr("wasd", st->kind))
			break;
		if (st->kind == 'd')		// (a wait: its milliseconds)
		{
			v = strtoul(p, &q, 16);
			if (q == p)
				break;
			st->val[0] = v;
			st->n = 1;
			p = q;
		}
		while ((st->kind != 'd') && (st->n < VL_AUTO_WORDS))
		{
			a = strtoul(p, &q, 16);
			if ((q == p) || (*q != '='))
				break;
			p = q + 1;
			st->adrs[st->n] = a;
			st->var[st->n] = -1;
			if (p[0] == '$')
			{
				v = strtoul(p + 1, &q, 10);
				if ((q == p + 1) || (v >= VL_AUTO_VARS))
					break;
				st->var[st->n] = (signed char)v;
				p = q;
			}
			else
			{
				v = strtoul(p, &q, 16);
				if (q == p)
					break;
				st->val[st->n] = v;
				p = q;
			}
			st->n++;
			if (*p != '/')
				break;
			p++;
		}
		if (!st->n)
			break;
		(*n)++;
		if (*p != ',')
			break;
		p++;
	}
}

static uint32_t	VlAutoValue(int list, const VlAutoStep *st, int i)
{
	return (st->var[i] >= 0) ? vl_auto_var[list][(int)st->var[i]] : st->val[i];
}

// 1: all the words of a "w" step hold, 0 not, -1 USB error
static int	VlAutoHolds(VlEnd *e, int list, const VlAutoStep *st)
{
	uint8_t	w[4];
	int		i;

	for (i = 0; i < st->n; i++)
	{
		if (!VlRead(e, st->adrs[i], w, 4))
			return (-1);
		if (U32(w) != VlAutoValue(list, st, i))
			return (0);
	}
	return (1);
}

static bool	VlAutoWrite(VlEnd *e, int list, const VlAutoStep *st, bool zero)
{
	uint8_t	w[4];
	int		i;

	for (i = 0; i < st->n; i++)
	{
		U32(w) = zero ? 0 : VlAutoValue(list, st, i);
		if (!USB_Exchange(e->handle, st->adrs[i], w, 4, NULL, 0))
			return (false);
	}
	return (true);
}

static void	VlAuto(VlEnd *e)
{
	uint32_t	now = VlMs();
	int			h, list = e->auto_list;
	const char	*what;

	for (h = 0; h < VL_AUTO_RUNS; h++)
	{
		if (vl_auto_n[h] < 0)
			VlAutoParse(h);
		if (e->auto_seen[h] != vl_auto_req[h])	// A new run asked (it replaces a run going on)
		{
			e->auto_seen[h] = vl_auto_req[h];
			if (!vl_auto_n[h])
			{
				printf("VLINK %s %s: not available for this game\n", vl_auto_what[h], e->name);
				continue;
			}
			e->auto_list = list = h;
			e->auto_step = 0;
			e->auto_on = 1;
			e->auto_armed = -1;
			e->auto_ms = now;
			e->auto_check_ms = 0;
			e->auto_until = 0;
			printf("VLINK %s %s: started (%d steps, values %u %u %u %u ...)\n", vl_auto_what[h], e->name, vl_auto_n[h],
				vl_auto_var[h][0], vl_auto_var[h][1], vl_auto_var[h][2], vl_auto_var[h][3]);
		}
	}
	what = vl_auto_what[list];
	if (!e->auto_on || (now - e->auto_check_ms < 50))
		return;
	e->auto_check_ms = now;
	while (e->auto_step < vl_auto_n[list])
	{
		const VlAutoStep	*st = &vl_auto[list][e->auto_step];

		if (st->kind == 'w')
		{
			if ((h = VlAutoHolds(e, list, st)) < 0)
				return;					// (USB: tried again next time)
			if (!h)
				break;
		}
		else if (st->kind == 'd')
		{
			if (!e->auto_until)
				e->auto_until = now + st->val[0];
			if ((int32_t)(now - e->auto_until) < 0)
				break;
			e->auto_until = 0;
		}
		else
		{
			if (!VlAutoWrite(e, list, st, false))
				return;
			if (st->kind == 'a')
				e->auto_armed = e->auto_step;
		}
		e->auto_step++;
		printf("VLINK %s %s: step %d of %d\n", what, e->name, e->auto_step, vl_auto_n[list]);
	}
	if (e->auto_step >= vl_auto_n[list])
	{
		e->auto_on = 0;
		printf("VLINK %s %s: done\n", what, e->name);
	}
	else if (now - e->auto_ms > VL_AUTO_MS)
	{
		if (e->auto_armed >= 0)
			VlAutoWrite(e, list, &vl_auto[list][e->auto_armed], true);
		e->auto_on = 0;
		printf("VLINK %s %s: failed at step %d of %d (%s)\n", what, e->name, e->auto_step + 1, vl_auto_n[list],
			list ? "the setup screen not shown?" : "the game not at its main menu?");
	}
}

// Kept in the game while the link runs (VLINK_KEEP_STEPS, -vlinkkeep: the same steps, "w" and "s" only): each second
// from the first step, until a "w" doesn't hold; its writes made then. For code the game loads again from its disc
// (Red Alert's menus: the main menu's idle timer, which leaves for the intro movie, made a nop each time the menus
// are loaded; the "w" checks the menu's code is there, unchanged)
static void	VlKeep(VlEnd *e)
{
	int		i, h;

	if (vl_auto_n[2] < 0)
		VlAutoParse(2);
	for (i = 0; i < vl_auto_n[2]; i++)
	{
		const VlAutoStep	*st = &vl_auto[2][i];

		if (st->kind == 'w')
		{
			if ((h = VlAutoHolds(e, 2, st)) <= 0)
				return;
		}
		else if (st->kind == 's')
		{
			if (!VlAutoWrite(e, 2, st, false))
				return;
			printf("VLINK %s: kept (step %d)\n", e->name, i + 1);
		}
	}
}

// The driver's settings from the file VLINK_TUNE names, written when changed
static void	VlTune(VlEnd *e)
{
	static char	details[1024];		// (the lobby's details last sent: "details DISC<TAB>SETTINGS")
	char		*path = getenv("VLINK_TUNE"), line[1024];
	unsigned int	v;
	int			i, list;
	FILE		*f;

	if (!path || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof(line), f))
		if ((sscanf(line, "trace %u", &v) == 1) && !e->id && v && !vl_trace_t0)
		{
			vl_trace_t0 = VlUs();
			vl_trace_until = vl_trace_t0 + (uint64_t)v * 1000000;
			vl_trace_on = 1;
			printf("VLINK trace: %u s\n", v);
		}
		else if ((sscanf(line, "stop %u", &v) == 1) && v)	// A clean stop: each thread's exchange finished first
		{													// (killed during one, half a write was left in memory)
			if (!stop_requested)
				printf("VLINK stop asked\n");
			stop_requested = 1;
		}
		else if ((sscanf(line, "join %u", &v) == 1) && !e->id && vl_lobby && (v != vl_join_last))
		{
			char	id[16];

			vl_join_last = v;
			if (v)
			{
				snprintf(id, sizeof(id), "%u", v);
				VlNetSend('J', (const uint8_t *)id, (uint32_t)strlen(id));
				printf("VLINK joining game %u\n", v);
			}
		}
		else if ((sscanf(line, "leave %u", &v) == 1) && !e->id && vl_lobby && (v != vl_leave_last))
		{
			vl_leave_last = v;
			if (v)
			{
				VlNetSend('Q', NULL, 0);
				vl_paired = 0;
				printf("VLINK left the game\n");
			}
		}
		else if (((list = !strncmp(line, "hostauto ", 9) ? 1 : !strncmp(line, "auto ", 5) ? 0 : -1) >= 0) && !e->id)
		{
			char	*p = line + (list ? 9 : 5), *q;

			v = strtoul(p, &q, 10);
			if ((q != p) && (v != vl_auto_req[list]))
			{
				for (i = 0, p = q; i < VL_AUTO_VARS; i++, p = q)	// (the values first: each thread starts its
					vl_auto_var[list][i] = strtoul(p, &q, 10);		// run on the number)
				vl_auto_req[list] = v;
			}
		}
		else if (!strncmp(line, "details ", 8) && !e->id && vl_lobby)	// The lobby's details changed (a host's new
		{																// settings): sent when changed
			line[strcspn(line, "\r\n")] = 0;
			if (strcmp(line + 8, details))
			{
				snprintf(details, sizeof(details), "%s", line + 8);
				VlNetSend('D', (const uint8_t *)details, (uint32_t)strlen(details));
			}
		}
		else if ((sscanf(line, "sym %u", &v) == 1) && !e->id && ((int)v != vl_sym))
		{
			vl_sym = v;
			printf("VLINK sym %u\n", v);
		}
		else if ((sscanf(line, "busy_us %u", &v) == 1) && !e->id && (v != vl_busy_us))
		{
			vl_busy_us = v;
			printf("VLINK busy_us %u\n", v);
		}
		else if ((sscanf(line, "sleep_us %u", &v) == 1) && !e->id && (v != vl_sleep_us))
		{
			vl_sleep_us = v;
			printf("VLINK sleep_us %u\n", v);
		}
		else if ((sscanf(line, "quiet_us %u", &v) == 1) && !e->id && (v != vl_quiet_us))
		{
			vl_quiet_us = v;
			printf("VLINK quiet_us %u\n", v);
		}
		else if ((sscanf(line, "idle_us %u", &v) == 1) && !e->id && (v != vl_idle_us))
		{
			vl_idle_us = v;
			printf("VLINK idle_us %u\n", v);
		}
		else if ((sscanf(line, "hold %u", &v) == 1) && !e->id && ((int)v != vl_hold))
		{
			vl_hold = v;
			printf("VLINK hold %u\n", v);
		}
		else if ((sscanf(line, "delay_us %u", &v) == 1) && !e->id && (v != vl_delay_us))
		{
			vl_delay_us = v;
			printf("VLINK delay_us %u (each way)\n", v);
		}
		else if ((sscanf(line, "pcopy %u", &v) == 1) && !e->id && ((int)v != vl_pcopy))
		{
			vl_pcopy = v;
			printf("VLINK pcopy %u\n", v);
		}
		else if ((sscanf(line, "fake %u", &v) == 1) && !e->id && ((int)v != vl_fake))
		{
			vl_fake = v;
			printf("VLINK fake %u\n", v);
		}
	fclose(f);
}

static void	VlNetTick(VlEnd *e);

// Words of the game watched once a second, each change printed ("VLINK BEN: watch main menu 0 -> 1000"): where the
// game is (its menus), to follow a failed start in the log. VLINK_WATCH (-vlinkwatch, from the game's patch
// package): "ADDR=name,ADDR=name,..." (hex addresses, up to VL_WATCH_MAX)
#define	VL_WATCH_MAX	8
static int		vl_watch_n = -1;
static uint32_t	vl_watch_adrs[VL_WATCH_MAX];
static char		vl_watch_name[VL_WATCH_MAX][24];

static void	VlWatch(VlEnd *e)
{
	uint8_t	w[4];
	int		i;

	if (vl_watch_n < 0)
	{
		char	*p = getenv("VLINK_WATCH"), *q;

		vl_watch_n = 0;
		while (p && *p && (vl_watch_n < VL_WATCH_MAX))
		{
			vl_watch_adrs[vl_watch_n] = strtoul(p, &q, 16);
			if ((q == p) || (*q != '='))
				break;
			p = q + 1;
			for (i = 0; *p && (*p != ',') && (i < 23); )
				vl_watch_name[vl_watch_n][i++] = *p++;
			vl_watch_name[vl_watch_n++][i] = 0;
			while (*p && (*p != ','))
				p++;
			if (*p == ',')
				p++;
		}
	}
	for (i = 0; (i < vl_watch_n) && (i < VL_WATCH_MAX); i++)
		if (VlRead(e, vl_watch_adrs[i], w, 4) && (!e->watch_seen[i] || (U32(w) != e->watch_val[i])))
		{
			if (e->watch_seen[i])
				printf("VLINK %s: watch %s %X -> %X\n", e->name, vl_watch_name[i], e->watch_val[i], U32(w));
			e->watch_val[i] = U32(w);
			e->watch_seen[i] = 1;
		}
}

// How long a console may wait with nothing coming before its wait is ended (as the cable's timeout). VL_ABORT_MS,
// unless the other console is only busy: its game frozen (its frame counter still since this wait began) while not
// waiting in the driver itself, its computer and USB still there. Then VL_STALL_MS: the other game comes back and the
// match goes on (2026-11-14: BEN's game stopped 2.2 s between a frame's two headers, probably its slow laser reading
// the disc; BILL's wait was ended after 1.5 s: "connection lost" for both after 5 minutes of play). A console that
// left the game runs on (its menu): its frame counter moves, so the wait still ends in about 2-3 s.
// (VL_STALL_MS: defined before VlSym)
static bool	VlNetThere(void);
static bool	VlPeerThere(VlEnd *e)
{
	if (e->net)
		return VlNetThere();
	return !e->peer->failed && !e->peer->reset;
}

static uint32_t	VlPatience(VlEnd *e, uint32_t now)
{
	VlEnd	*p = e->peer;

	if (!VlPeerThere(e) ||
		(VlUs() - p->wait_us < (uint64_t)VL_WAIT_SEEN_MS * 1000) ||	// (both waiting: nothing will come)
		((int32_t)(p->frame_ms - e->moved_ms) > 1000))				// (the other's game going on: it left)
	{
		e->stall_said = 0;
		return (VL_ABORT_MS);
	}
	if (!e->stall_said && (now - e->moved_ms > VL_ABORT_MS))
	{
		e->stall_said = 1;
		printf("VLINK %s: the other console is busy (its game stopped), waiting for it\n", e->name);
	}
	return (VL_STALL_MS);
}

static void	VlLoop(VlEnd *e)
{
	uint8_t		w[4], before[VL_PC];
	uint32_t	now;
	uint64_t	last_us = 0, now_us;
	uint32_t	need;
	time_t		second = time(NULL);

	while (!stop_requested && !e->failed)
	{
		if (e->net)
			VlNetTick(e);
		memcpy(before, e->pc, VL_PC);
		VlIncoming(e);
		if (!memcmp(before, e->pc, VL_PC) && !e->pc_dirty && (VlUs() - e->wait_us > vl_busy_us) &&
			(VlUs() - last_us < (VlQuiet(e) ? vl_sleep_us : vl_idle_us)))
		{
#ifdef _WIN32
			Sleep(0);
#else
			sched_yield();
#endif
			continue;
		}
		last_us = VlUs();
		e->pc_dirty = 0;
		// Its send area read as far as the bytes waiting last turn (and some more), in whole USB packets
		need = VL_PS1 + U32(e->ps1 + 4) - U32(e->ps1 + 36) + 16;
		for (e->ps1_read = VL_USB_DATA - 3; (e->ps1_read < need) && (e->ps1_read < VL_PS1_READ_MAX); )
			e->ps1_read += VL_USB_DATA;
		if (!USB_Exchange(e->handle, VL_PC_ADRS - e->rx_new, e->pc - e->rx_new, e->rx_new + VL_PC, e->ps1,
			e->ps1_read))
		{
			e->failed = 1;
			return;
		}
		e->rx_new = 0;
		now_us = VlUs();
		VlTrace(e->id, 'X', U32(e->pc) - U32(before), U32(e->ps1 + 32));
		VlTimeAdd(U32(e->ps1 + 32) ? &e->t_wait : &e->t_other, (uint32_t)(now_us - last_us));
		if (e->deliver_us)
		{
			VlTimeAdd(&e->t_cross, (uint32_t)(now_us - e->deliver_us));
			e->deliver_us = 0;
		}
		if (e->req_done)
		{
			VlTimeAdd(&e->t_read, (uint32_t)(now_us - e->req_us));
			e->req_us = 0;
			e->req_done = 0;
		}
		VlOutgoing(e);
		VlSym(e);
		VlAuto(e);
		if (e->net)
			VlNetTick(e);
		e->turns++;
		// Waiting for the other side (its waiting flag, set while it serves this exchange from a wait) with nothing
		// moving for VL_ABORT_MS: its wait ended, as a timeout of the real port (the other console gone)
		now = VlMs();
		if (U32(e->ps1 + 32))
		{
			if (now - e->wait_ms > VL_WAIT_SEEN_MS)		// A new wait: its time counted from now
				e->moved_ms = now;
			e->wait_ms = now;
			e->wait_us = now_us;
		}
		if (e->stall_said && (now - e->moved_ms < VL_ABORT_MS))
		{
			e->stall_said = 0;
			printf("VLINK %s: the other console is back\n", e->name);
		}
		if ((now - e->wait_ms <= VL_WAIT_SEEN_MS) &&
			(now - e->moved_ms > (e->gone ? VL_GONE_MS : VlPatience(e, now))))
		{
			U32(e->pc + 16)++;
			e->pc_dirty = 1;
			e->copy_pending = e->copy_used = 0;
			e->owed = e->h2_gone = e->drop_hdr = 0;
			e->req_us = 0;
			e->req_done = 0;
			e->aborts++;
			e->gone = 1;
			e->moved_ms = VlMs();
		}
		if (time(NULL) != second)		// Once a second: the driver still there, the game's frame counter
		{
			second = time(NULL);
			if (!VlRead(e, VL_BUF, w, 4) || (U32(w) != VL_MAGIC))
			{
				e->reset = 1;
				return;
			}
			if (VlRead(e, VL_FRAME, w, 4))
			{
				if (U32(w) != e->frame)
					e->frame_ms = VlMs();
				e->frame = U32(w);
			}
			VlWatch(e);
			VlKeep(e);
			VlTune(e);
		}
	}
}

#ifdef _WIN32
static DWORD WINAPI	VlThreadMain(LPVOID arg)
{
	VlLoop((VlEnd *)arg);
	return (0);
}
#else
static void	*VlThreadMain(void *arg)
{
	VlLoop((VlEnd *)arg);
	return (NULL);
}
#endif

// The rings' state read first (carried on as the consoles left it: the driver may be in use, e.g. on the link page),
// and each console's lines told to the other
static bool	VlStart(VlEnd *e)
{
	uint8_t		w[4];

	if (!VlRead(e, VL_BUF, w, 4) || (U32(w) != VL_MAGIC))
	{
		printf("%s: the virtual link driver isn't installed (ps1-linkplay/virtual/usb_link.py installs it).\n",
			e->name);
		return (false);
	}
	e->pc = e->area + VL_RX;
	if (!VlRead(e, VL_PC_ADRS, e->pc, VL_PC) || !VlRead(e, VL_PC_ADRS + VL_PC, e->ps1, VL_PS1))
	{
		printf("%s: USB error.\n", e->name);
		return (false);
	}
	e->moved_ms = VlMs();
	e->out->buf[e->out->head & (VL_STREAM - 1)] = 1;
	e->out->buf[(e->out->head + 1) & (VL_STREAM - 1)] = U32(e->ps1 + 12) & 3;
	e->out->head += 2;
	return (true);
}

static void	VlTraceMsg(int end, const uint8_t *data, uint32_t len)
{
	long	i;

	if (!vl_trace_on)
		return;
	i = __sync_fetch_and_add(&vl_msg_n, 1) % VL_MSG_MAX;		// (a ring: the newest kept)
	vl_msgs[i].us = (uint32_t)(VlUs() - vl_trace_t0);
	vl_msgs[i].end = end;
	vl_msgs[i].len = len;
	memcpy(vl_msgs[i].data, data, (len < VL_MSG_BYTES) ? len : VL_MSG_BYTES);
}

// The trace to vlink_trace.txt: "us console kind a b" per line, then "us console M len hex" per message written
static void	VlTraceWrite(void)
{
	FILE	*f;
	long	i, k, n, j, m, first;

	vl_trace_on = 0;
	// (rings: the newest VL_TRACE_MAX events and VL_MSG_MAX messages, oldest first)
	n = (vl_trace_n < VL_TRACE_MAX) ? vl_trace_n : VL_TRACE_MAX;
	if (!(f = fopen("vlink_trace.txt", "w")))
		return;
	first = vl_trace_n - n;
	for (k = 0; k < n; k++)
	{
		i = (first + k) % VL_TRACE_MAX;
		fprintf(f, "%u %d %c %u %u\n", (unsigned int)vl_trace[i].us, vl_trace[i].end, vl_trace[i].kind,
			vl_trace[i].a, vl_trace[i].b);
	}
	m = (vl_msg_n < VL_MSG_MAX) ? vl_msg_n : VL_MSG_MAX;
	first = vl_msg_n - m;
	for (k = 0; k < m; k++)
	{
		i = (first + k) % VL_MSG_MAX;
		fprintf(f, "%u %d M %u ", (unsigned int)vl_msgs[i].us, vl_msgs[i].end, vl_msgs[i].len);
		for (j = 0; (j < vl_msgs[i].len) && (j < VL_MSG_BYTES); j++)
			fprintf(f, "%02X", vl_msgs[i].data[j]);
		fprintf(f, "\n");
	}
	fclose(f);
	printf("VLINK trace written: the last %ld of %ld events\n", n, (long)vl_trace_n);
}

// The last second's timing (microseconds: mean/max xcount), then reset
static void	VlTimePrint(VlEnd *e)
{
	volatile VlTime	*t[4] = {&e->t_wait, &e->t_other, &e->t_cross, &e->t_read};
	char			s[4][40];
	unsigned int	read_ms = (unsigned int)(e->t_read.sum / 1000);
	int				i;

	for (i = 0; i < 4; i++)
	{
		snprintf(s[i], sizeof(s[i]), "%u/%u x%u", (unsigned int)(t[i]->n ? t[i]->sum / t[i]->n : 0),
			(unsigned int)t[i]->max, (unsigned int)t[i]->n);
		t[i]->n = t[i]->max = 0;
		t[i]->sum = 0;
	}
	printf("VTIME %s us: turns waiting %s, turns other %s, crossings %s, reads waiting %s = %u ms\n", e->name, s[0],
		s[1], s[2], s[3], read_ms);
}

// Returns 0 when stopped, 2 when a console's driver went (reset: install it again), 1 for an error
int	VlinkRelay(char *first_name, char *second_serial)
{
	static VlStream	ab, ba;
	static VlEnd	a, b;
	uint32_t		frame_a = 0, frame_b = 0;
	time_t			second;
	int				ret = 1;
#ifdef _WIN32
	HANDLE			ha, hb;
#else
	pthread_t		ha, hb;
#endif

	setvbuf(stdout, NULL, _IONBF, 0);
	a.handle = USB_GetHandle();
	a.name = first_name;
	b.name = second_serial;
	a.in = &ba;
	a.out = &ab;
	b.in = &ab;
	b.out = &ba;
	if (!(b.handle = USB_OpenOther(second_serial)))
	{
		printf("Can't open the cartridge %s (see -l).\n", second_serial);
		return (1);
	}
	ps1_program_running = true;		// Only the kernel hooks answer
	usb_big_packet_size = 0;
	usb_quiet = true;
	USB_SetHandle(b.handle);
	USB_DetectEscaped();
	USB_SetHandle(a.handle);
	USB_DetectEscaped();
	b.id = 1;
	a.held_line = b.held_line = -1;
	usb_patient = 1;
	a.peer = &b;
	b.peer = &a;
	if (!VlStart(&a) || !VlStart(&b))
		goto out;
#ifdef _WIN32
	InitializeCriticalSection(&queue_lock);
	SetConsoleCtrlHandler(OnInterrupt, TRUE);
	ha = CreateThread(NULL, 0, VlThreadMain, &a, 0, NULL);
	hb = CreateThread(NULL, 0, VlThreadMain, &b, 0, NULL);
#else
	signal(SIGINT, OnInterrupt);
	pthread_create(&ha, NULL, VlThreadMain, &a);
	pthread_create(&hb, NULL, VlThreadMain, &b);
#endif
	printf("VLINK carrying between %s and %s (Ctrl+C to stop)\n", a.name, b.name);
	for (second = time(NULL); !StopRequested() && !a.failed && !b.failed && !a.reset && !b.reset; )
	{
#ifdef _WIN32
		Sleep(50);
#else
		usleep(50000);
#endif
		if (time(NULL) != second)
		{
			printf("VLINK %s %u turns/s, %u B in, %u B out, %u frames/s   %s %u turns/s, %u B in, %u B out, "
				"%u frames/s%s%s\n", a.name, (unsigned int)a.turns, (unsigned int)a.bytes_in,
				(unsigned int)a.bytes_out, (unsigned int)(frame_a ? a.frame - frame_a : 0), b.name,
				(unsigned int)b.turns, (unsigned int)b.bytes_in, (unsigned int)b.bytes_out,
				(unsigned int)(frame_b ? b.frame - frame_b : 0), link_crc_errors ? "   (CRC errors)" : "",
				(a.aborts || b.aborts) ? "   (waits ended: the other side gone)" : "");
			a.aborts = b.aborts = 0;
			if (a.dup_bad || b.dup_bad)
				printf("VLINK second headers different from the first: %u %u (given the first's copy)\n",
					(unsigned int)a.dup_bad, (unsigned int)b.dup_bad);
			VlTimePrint(&a);
			VlTimePrint(&b);
			{	// (the stop line seen here too: both threads may be waiting for silent consoles, not reading it)
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
			frame_b = b.frame;
			a.turns = b.turns = a.bytes_in = a.bytes_out = b.bytes_in = b.bytes_out = 0;
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
	if (a.reset || b.reset)
	{
		printf("VLINK %s: the driver is gone (console reset?)\n", a.reset ? a.name : b.name);
		ret = 2;
	}
	else if (a.failed || b.failed)
		printf("VLINK %s: USB error\n", a.failed ? a.name : b.name);
	else
	{
		printf("VLINK stopped\n");
		ret = 0;
	}

out:
	USB_SetHandle(a.handle);
	USB_CloseOther(b.handle);
	return (ret);
}

/****************************************/
// -vlinkhost PORT / -vlinkjoin HOST:PORT: the virtual link cable between two computers, each with its own cartridge
// (Net Play): this computer's half carries its console as -vlink does, the other console is represented by a VlEnd
// kept up to date from the other half's status messages (in a game, symmetric exchange on, lines, frame counter), so
// that all of -vlink's logic (copies, double delivery, the symmetric switch, line holding, waits ended) runs as it is.
// TCP frames: type ('H' hello, 'R' records, 'S' status), length (2 bytes), data.
// The host carries its console from the start, while it waits for the other player: what the console sends is
// dropped until then (at record boundaries), and a console holding RTS for VL_OFFER_MS with nobody connected is
// waiting on its link page ("VLINK OFFER": a game offered, for the Manager's lobby; "VLINK WITHDRAW" when it stops).
#define	VL_NET_VERSION	"VLNK 1"
#define	VL_NET_STATUS_MS	100
#define	VL_OFFER_MS		300				// (longer than the link handshake's one-frame RTS pulse)

static volatile sock_t	vl_net = INVALID_SOCKET;
static volatile int		vl_net_gone;		// The other computer's connection closed

// The other computer's half connected (and paired, in the lobby): see VlPatience
static bool	VlNetThere(void)
{
	return (vl_net != INVALID_SOCKET) && !vl_net_gone && (!vl_lobby || vl_paired);
}
static volatile int		vl_net_fresh;		// Just connected: the console's lines sent first, at a record boundary
// -vlinklobby HOST:PORT: connected to the Net Play lobby server (ps1-linkplay/lobby/netplay_server.py), which pairs this
// half with another one and then forwards the relay frames between them ('L' hello, 'O' offer, 'W' withdraw, 'J' join,
// 'Q' leave; from it: 'G' the open games, 'P' paired, 'X' the other player left, 'E' error)
static char				vl_paired_name[80];

static bool	VlNetRecvAll(sock_t s, uint8_t *buf, uint32_t n)
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

static bool	VlNetSend(int type, const uint8_t *data, uint32_t n)
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
static void	VlNetTick(VlEnd *e)
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
