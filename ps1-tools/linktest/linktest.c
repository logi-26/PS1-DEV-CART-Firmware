/*
* PS1 DEV CART link cable tester
* Executed on both consoles by "ps1transfer -n <serial> -link [yaroze|usb]"
*
* Modes, chosen by ps1transfer (the kernel hook's tty mode tells which one):
* - libcomb (default, "-link"): like a game using the link cable library: AddCOMB(), open("sio:") with O_NOWAIT,
*   asynchronous read/write ended by the SIO events. Built with PSn00bSDK, it uses psxcomb (../../psxcomb), our
*   library compatible with libcomb, which gives the same signals on the cable as the PlayStation library. Reads are of the size sent each frame (same
*   settings on both consoles); a write still waiting for the other console (flow control) is shown as "blocked" and
*   the frame's packets are not sent. A write without progress for COMB_TIMEOUT frames is cancelled with the read
*   and both restarted ("resync").
*   The PsyQ build (linktest_psyq.ps-exe, "make psyq", for comparisons) uses the original library: it sends one byte
*   per handshake without timeout and only sees the acknowledge at its next transmit interrupt, so the tester enables
*   that interrupt again while a write waits (CombKick).
* - USB ("-link usb", tty mode USB): no serial cable, the same libcomb calls with CombSetLink(COMB_LINK_USB): the
*   consoles are linked through their cartridges and the computer ("ps1transfer -n SERIAL1 -relay SERIAL2").
* - Net Yaroze ("-link yaroze"): like a Net Yaroze game (e.g. YarIO), with the serial port "tty" driver installed
*   by ps1transfer (like "-c sio"): open("tty:"), the SIO ioctls, non-blocking read, write.
* Each console sends numbered packets every frame:
*	0xA5, length, sequence (16-bit), echo (16-bit: last sequence received from the other console), test pattern, checksum
* Shown: link state, CTS/DSR lines, packets sent/received/lost, corrupted packets, throughput, round trip time.
* Nothing is printed with printf: the "tty" is the serial port, the text would be sent to the other console.
*
*
* Controls:	Up/Down: packets per frame, Left/Right: packet size, L1/R1: baud rate (same on both consoles),
*			Cross: pause sending, Start: reset statistics
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <string.h>
#ifdef PSN00BSDK
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
// BIOS "tty" driver ioctls (as PsyQ's sys/ioctl.h)
#define	FIOCNBLOCK	(('f' << 8) | 1)	// Non-blocking read
#define	TIOCRAW		(('t' << 8) | 1)	// No XON/XOFF
#define	TIOCFLUSH	(('t' << 8) | 2)	// Flush the input buffer
#define	TIOCBAUD	(('t' << 8) | 4)	// Baud rate
#define	TIOCLEN		(('t' << 8) | 8)	// Stop bits << 16 | character length
#define	TIOCPARITY	(('t' << 8) | 9)	// Parity: 0 none, 1 even, 3 odd
#define	TIOERRRST	(('t' << 8) | 11)	// Error reset
#include "psxcomb/psxcomb.h"	// Our link cable library, libcomb compatible (serial and USB link)
#define	HAVE_COMB	1
#define	HAVE_USB	1
#else
#include <sys/types.h>
#include <libetc.h>
#include <libgte.h>
#include <libgpu.h>
#include <libapi.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <libcomb.h>
#define	HAVE_COMB	1				// PsyQ build: the original library (no USB link)
#endif
#include "devcart_ui.h"
// Text of the current panel (streams opened in main)
static int	fnt_cur, fnt_status, fnt_stats, fnt_rtt, fnt_hint;
#define	PRINT(...)	FntPrint(fnt_cur, __VA_ARGS__)


#define	SCREEN_W		320
#define	SCREEN_H		240
#define	SYNC			0xA5
#define	HEADER_SIZE		6		// sync, length, sequence, echo
#define	MAX_PACKET		255		// Length in one byte
#define	RX_BUFFER		8192
#define	LINK_TIMEOUT	60		// Frames without packet: link lost
#define	SIO1_STAT		*((volatile uint16_t *)0x1F801054)
#define	HOOK_TTY_MODE	(*((volatile uint8_t *)0x8000DF71) & 0x7F)	// Kernel hook params: tty mode of the execute
#define	TTY_MODE_USB	0		// printf to USB (USB link mode)
#define	TTY_MODE_SIO	2		// "tty" driver on the serial port installed (Net Yaroze mode)
#define	TTY_MODE_LINK	3		// The same "tty" driver on the USB link (Net Yaroze mode through the computer)
#define	SIO1_CTRL		*((volatile uint16_t *)0x1F80105A)
#define	SIO_TXIEN		0x400
#define	COMB_TIMEOUT	15		// Frames without write progress: resync

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			side;

static const long	bauds[] = {9600, 19200, 38400, 57600, 115200, 230400};
static const int	sizes[] = {8, 16, 32, 64, 128, 255};
#define	SIZES			6

// Packets per frame and size index (pad), global so that they can also be set from the computer for tests
int				per_frame = 1, size_idx = 0;
static int			psyq;				// 1: libcomb ("sio:"), 0: Net Yaroze ("tty:")
static int			usb;				// 1: USB link through the computer (libcomb, CombSetLink)
static int			comb;				// libcomb used (serial or USB link)
static long			tty = -1;
#ifdef HAVE_COMB
static long			comb_read = -1, comb_write = -1, ev_read, ev_write;
static int			write_busy, write_packets, read_size;
static uint8_t		comb_rx[MAX_PACKET * 8], comb_tx[MAX_PACKET * 8];
#endif
static uint32_t		blocked, resyncs;
#ifdef HAVE_COMB
static long			write_left;			// Bytes left of the write in progress, and frame of its last progress
static uint32_t		write_progress_frame;
#endif
static uint8_t		rx[RX_BUFFER];
static int			rx_len;
static uint16_t		send_frame[256];	// Frame when each sequence (low byte) was sent, for the round trip time

static uint32_t		frame, last_rx_frame, tx_packets, rx_packets, lost, corrupted, tx_bytes, rx_bytes;
static uint32_t		rtt_sum, rtt_count, rtt_min, rtt_max, rtt_last;
static uint16_t		tx_seq, rx_seq_expected, last_echo, peer_seq;
static int			rx_started;
static uint32_t		rate_frame, rate_rx_bytes, rate_tx_bytes, rx_rate, tx_rate;

static void	ResetStats(void)
{
	tx_packets = rx_packets = lost = corrupted = tx_bytes = rx_bytes = blocked = resyncs = 0;
	rtt_sum = rtt_count = rtt_max = rtt_last = 0;
	rtt_min = 0xFFFFFFFF;
	rx_started = 0;
	rate_frame = frame;
	rate_rx_bytes = rate_tx_bytes = rx_rate = tx_rate = 0;
}

// Same setup as YarIO: raw, baud rate, 8 bits 1 stop bit, no parity, non-blocking, flush, error reset
static void	OpenSerial(long baud)
{
	if (tty < 0)
		tty = open("tty:", 2);
	if (tty < 0)
		return;
	ioctl(tty, TIOCRAW, 1);
	ioctl(tty, TIOCBAUD, baud);
	ioctl(tty, TIOCLEN, (1 << 16) | 3);
	ioctl(tty, TIOCPARITY, 0);
	ioctl(tty, FIOCNBLOCK, 1);
	ioctl(tty, TIOCFLUSH, 1);
	ioctl(tty, TIOERRRST, 1);
	rx_len = 0;
}

#ifdef HAVE_COMB
// PsyQ: libcomb, the SIO events tell when an asynchronous read/write has ended
static void	OpenComb(long baud, int size)
{
	if (comb_read < 0)
	{
		AddCOMB();
		EnterCriticalSection();
		ev_read = OpenEvent(HwSIO, EvSpIOER, EvMdNOINTR, NULL);
		ev_write = OpenEvent(HwSIO, EvSpIOEW, EvMdNOINTR, NULL);
		EnableEvent(ev_read);
		EnableEvent(ev_write);
		ExitCriticalSection();
		comb_read = open("sio:", O_RDONLY | O_NOWAIT);
		comb_write = open("sio:", O_WRONLY | O_NOWAIT);
	}
	else
	{
		CombCancelRead();
		CombCancelWrite();
	}
	if ((comb_read < 0) || (comb_write < 0))
		return;
	CombSetBPS(baud);
	CombSetControl(COMB_BIT_DTR | COMB_BIT_RTS);
	CombResetError();
	TestEvent(ev_read);
	TestEvent(ev_write);
	write_busy = 0;
	rx_len = 0;
	read_size = size;
	read(comb_read, comb_rx, read_size);
}
#endif

static uint8_t	Checksum(uint8_t *p, int n)
{
	uint8_t	sum = 0;

	while (n--)
		sum += *p++;
	return (sum);
}

static void	BuildPacket(uint8_t *p, int size)
{
	int		i;

	p[0] = SYNC;
	p[1] = size;
	p[2] = tx_seq;
	p[3] = tx_seq >> 8;
	p[4] = peer_seq;
	p[5] = peer_seq >> 8;
	for (i = HEADER_SIZE; i < size - 1; i++)	// Test pattern depending on the sequence
		p[i] = tx_seq + i * 7;
	p[size - 1] = Checksum(p, size - 1);
	send_frame[tx_seq & 255] = frame;
}

static void	SendPacket(int size)
{
	uint8_t	p[MAX_PACKET];

	BuildPacket(p, size);
	if (write(tty, p, size) == size)
	{
		tx_packets++;
		tx_bytes += size;
		tx_seq++;
	}
}

#ifdef HAVE_COMB
// PsyQ: the packets of the frame in one asynchronous write, counted once it has ended
// The libcomb interrupt handler checks the handshake again (transmit interrupt). Only while libcomb has a write in
// progress (checked with the interrupts off): otherwise the transmit interrupt would never be disabled again
static void	CombKick(void)
{
#ifndef PSN00BSDK		// (psxcomb checks the acknowledge itself)
	EnterCriticalSection();
	if (CombAsyncRequest(0))
		SIO1_CTRL |= SIO_TXIEN;
	ExitCriticalSection();
#endif
}

static void	SendComb(int size, int count)
{
	int	i;

	if (write_busy && TestEvent(ev_write))
	{
		write_busy = 0;
		tx_packets += write_packets;
		tx_bytes += write_packets * size;
	}
	if (write_busy)
	{
		blocked++;		// The other console doesn't read (flow control): this frame's packets are not sent
		if (CombBytesToWrite() != write_left)
		{
			write_left = CombBytesToWrite();
			write_progress_frame = frame;
		}
		// (USB link: a blocked write only means the relay is busy, nothing can be lost: no resync)
		if (usb || (frame - write_progress_frame < COMB_TIMEOUT))
		{
			CombKick();
			return;
		}
		// No progress (a byte was lost): read and write restarted, the partial packets are dropped by the parser
		CombCancelWrite();
		CombCancelRead();
		TestEvent(ev_read);
		TestEvent(ev_write);
		read(comb_read, comb_rx, read_size);
		write_busy = 0;
		resyncs++;
	}
	for (i = 0; i < count; i++)
	{
		BuildPacket(comb_tx + i * size, size);
		tx_seq++;
	}
	write_packets = count;
	write_busy = 1;
	write_left = size * count;
	write_progress_frame = frame;
	write(comb_write, comb_tx, size * count);
}
#endif

static void	ParsePackets(void);

// Packets found in the received bytes: sync, valid length, checksum. Bad data is skipped byte by byte.
static void	ParsePackets(void)
{
	int		pos = 0, size;
	uint16_t	seq, echo;

	while (rx_len - pos >= 8)
	{
		size = rx[pos + 1];
		if ((rx[pos] != SYNC) || (size < 8) || (size > MAX_PACKET))
		{
			pos++;
			corrupted += rx_started;	// Garbage before the first packet is not counted
			continue;
		}
		if (rx_len - pos < size)
			break;
		if (Checksum(rx + pos, size - 1) != rx[pos + size - 1])
		{
			pos++;
			corrupted++;
			continue;
		}
		seq = rx[pos + 2] | (rx[pos + 3] << 8);
		echo = rx[pos + 4] | (rx[pos + 5] << 8);
		if (rx_started && (seq != rx_seq_expected) && ((uint16_t)(seq - rx_seq_expected) < 1000))
			lost += (uint16_t)(seq - rx_seq_expected);	// Gap in the sequence (not counted: old/duplicate packets)
		rx_started = 1;
		rx_seq_expected = seq + 1;
		peer_seq = seq;
		rx_packets++;
		last_rx_frame = frame;

		// Round trip: frames since our packet "echo" was sent
		if (echo != last_echo)
		{
			last_echo = echo;
			rtt_last = (uint16_t)((uint16_t)frame - send_frame[echo & 255]);
			if (rtt_last < 60)
			{
				rtt_sum += rtt_last;
				rtt_count++;
				if (rtt_last < rtt_min)	rtt_min = rtt_last;
				if (rtt_last > rtt_max)	rtt_max = rtt_last;
			}
		}
		pos += size;
	}
	if (pos)
	{
		memmove(rx, rx + pos, rx_len - pos);
		rx_len -= pos;
	}
}

#ifdef HAVE_COMB
static void	ReceiveComb(void)
{
	int	n = 0;

	// Read ended: its bytes are kept, the next one is started. A few reads per call only: with the USB link, the
	// next read can end at once (bytes already received), the sending would never get its turn
	while ((n++ < 32) && TestEvent(ev_read))
	{
		if (rx_len + read_size > RX_BUFFER)
			rx_len = 0;
		memcpy(rx + rx_len, comb_rx, read_size);
		rx_len += read_size;
		rx_bytes += read_size;
		read(comb_read, comb_rx, read_size);
	}
	ParsePackets();
}

// Waits for the next frame, the handshake and the reads kept going meanwhile (VSync(0) otherwise)
static void	CombWaitFrame(void)
{
	long	v = VSync(-1);

	while (VSync(-1) == v)
	{
		if (write_busy)
			CombKick();
		ReceiveComb();
	}
}
#endif

static void	Receive(void)
{
	long	n;

	while ((rx_len < RX_BUFFER) && ((n = read(tty, rx + rx_len, RX_BUFFER - rx_len)) > 0))
	{
		rx_len += n;
		rx_bytes += n;
	}
	ParsePackets();
	if (rx_len == RX_BUFFER)	// Only garbage: drop it
		rx_len = 0;
}

int	main(void)
{
	uint32_t		pad = 0, opad;
	int			i, baud = 4, paused = 0, is_pal, connected, frame_ms;
	uint16_t		stat;

#ifndef PSN00BSDK
	ResetCallback();	// (PSn00bSDK: ResetGraph does it, and must be first, see psyq_compat.h)
#endif

	// Same video mode as the console region, like the PS1 DEV CART menu (BIOS version string: 'E' = Europe = PAL)
	is_pal = (*((char *)0xBFC7FF52) == 'E');
	frame_ms = is_pal ? 20 : 17;
	ResetGraph(0);
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
#ifndef PSN00BSDK		// (PSn00bSDK's PutDispEnv centres the picture for PAL)
		if (is_pal)
			disp[i].screen.y = 16;
#endif
	}
	FntLoad(960, 256);
	UiInit(draw);
	fnt_status = UiOpenText(8, 20, 304, 2);
	fnt_stats = UiOpenText(8, 54, 304, 5);
	fnt_rtt = UiOpenText(8, 112, 304, 2);
	fnt_hint = UiOpenText(8, 146, 304, 1);
	SetDispMask(1);

	usb = (HOOK_TTY_MODE == TTY_MODE_USB);
	psyq = !usb && (HOOK_TTY_MODE != TTY_MODE_SIO) && (HOOK_TTY_MODE != TTY_MODE_LINK);
#ifdef HAVE_USB
	comb = psyq || usb;
	if (usb)
		CombSetLink(COMB_LINK_USB);		// (before AddCOMB: the rings are published at once)
#else
	comb = psyq;					// (PsyQ build: no USB link)
#endif
	#define	OPEN()	(comb ? OpenComb(bauds[baud], sizes[size_idx] * per_frame) : (usb ? (void)0 : OpenSerial(bauds[baud])))
	OPEN();
	ResetStats();

	while (1)
	{
		opad = pad;
		pad = PadRead(1);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		// (PsyQ: the read size follows the packets per frame and their size)
		if (PRESSED(PADLup) && (per_frame < 8))		{per_frame++; if (comb) OPEN();}
		if (PRESSED(PADLdown) && (per_frame > 1))	{per_frame--; if (comb) OPEN();}
		if (PRESSED(PADLright) && (size_idx < SIZES - 1))	{size_idx++; if (comb) OPEN();}
		if (PRESSED(PADLleft) && (size_idx > 0))	{size_idx--; if (comb) OPEN();}
		if (PRESSED(PADR1) && (baud < 5))			{baud++; OPEN(); ResetStats();}
		if (PRESSED(PADL1) && (baud > 0))			{baud--; OPEN(); ResetStats();}
		if (PRESSED(PADRdown))						paused = !paused;
		if (PRESSED(PADstart))						ResetStats();

		if (comb && (comb_read >= 0) && (comb_write >= 0))
		{
			ReceiveComb();
			if (!paused)
				SendComb(sizes[size_idx], per_frame);
		}
		else if (!comb && !usb && (tty >= 0))
		{
			Receive();
			if (!paused)
				for (i = 0; i < per_frame; i++)
					SendPacket(sizes[size_idx]);
		}

		// Throughput every second
		if (frame - rate_frame >= (is_pal ? 50 : 60))
		{
			rx_rate = rx_bytes - rate_rx_bytes;
			tx_rate = tx_bytes - rate_tx_bytes;
			rate_rx_bytes = rx_bytes;
			rate_tx_bytes = tx_bytes;
			rate_frame = frame;
		}

		stat = SIO1_STAT;
		connected = rx_started && (frame - last_rx_frame < LINK_TIMEOUT);
		// Screen (devcart_ui.h): mode in the title bar, link status (with a light), statistics, round trip
#ifdef PSN00BSDK
		UiTitle("PS1 DEV CART LINK TESTER", usb ? "USB LINK" : (psyq ? "LIBCOMB" :
#else
		UiTitle("PS1 DEV CART LINK TESTER", usb ? "PSN00B ONLY" : (psyq ? "PSYQ LIBCOMB" :
#endif
			((HOOK_TTY_MODE == TTY_MODE_LINK) ? "YAROZE USB" : "NET YAROZE")));
		UiTextPanel(8, 20, 304, 2);
		UiTextPanel(8, 54, 304, 5);
		UiTextPanel(8, 112, 304, 2);
		UiTile(292, 26, 12, 8, connected ? UI_OK : UI_ERROR);
		fnt_cur = fnt_status;
		if (!comb && !usb && (tty < 0))
			PRINT("CAN'T OPEN TTY:\n");
		else if (comb && ((comb_read < 0) || (comb_write < 0)))
			PRINT("CAN'T OPEN SIO:\n");
		else
			PRINT("LINK  %s   CTS %s  DSR %s\n", connected ? "CONNECTED" : "NO DATA", (stat & 0x100) ? "ON " : "OFF",
				(stat & 0x80) ? "ON " : "OFF");
		// Note: FntPrint only knows %d (no %ld, no %%)
		if (usb)
			PRINT("USB  %d x %d BYTES/FRAME%s", per_frame, sizes[size_idx], paused ? "  PAUSED" : "");
		else
			PRINT("BAUD  %d  %d x %d BYTES/FRAME%s", (int)bauds[baud], per_frame, sizes[size_idx],
				paused ? "  PAUSED" : "");
		fnt_cur = fnt_stats;
		PRINT("SENT      %d PACKETS  %d B/S\n", (int)tx_packets, (int)tx_rate);
		PRINT("RECEIVED  %d PACKETS  %d B/S\n", (int)rx_packets, (int)rx_rate);
		PRINT("LOST      %d", (int)lost);
		if (rx_packets + lost)
			PRINT("  (%d.%d%s)", (int)(lost * 100 / (rx_packets + lost)), (int)((lost * 1000 / (rx_packets + lost)) % 10), "%");
		PRINT("\nCORRUPTED %d", (int)corrupted);
		if (comb)
			PRINT("\nBLOCKED   %d  RESYNC %d", (int)blocked, (int)resyncs);
		fnt_cur = fnt_rtt;
		if (rtt_count)
			PRINT("ROUND TRIP  %d FRAMES (%d MS)\nAVG %d.%d  MIN %d  MAX %d FRAMES", (int)rtt_last, (int)(rtt_last * frame_ms),
				(int)(rtt_sum / rtt_count), (int)((rtt_sum * 10 / rtt_count) % 10), (int)rtt_min, (int)rtt_max);
		else
			PRINT("ROUND TRIP  -");
		if (!connected)
		{
			UiTextPanel(8, 146, 304, 1);
			fnt_cur = fnt_hint;
			PRINT("NO LINK: RUN THE TESTER ON BOTH PS1");
		}
		UiHelp(usb ? "UP/DOWN:PACKETS  LEFT/RIGHT:SIZE\nX:PAUSE  START:RESET" :
			"UP/DOWN:PACKETS  LEFT/RIGHT:SIZE\nL1/R1:BAUD(BOTH) X:PAUSE START:RESET");

		DrawSync(0);
		if (comb && (comb_read >= 0) && (comb_write >= 0))
			CombWaitFrame();
		else
			VSync(0);
		frame++;
		side ^= 1;
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		UiFlush();
		FntFlush(fnt_status);
		FntFlush(fnt_stats);
		FntFlush(fnt_rtt);
		FntFlush(fnt_hint);
	}

	return (0);
}
