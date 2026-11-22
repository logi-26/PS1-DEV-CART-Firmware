/*
* Virtual link cable for Red Alert Retaliation (SLES_013.43 / SLES_113.43)
*
* The game reaches the serial port only through three functions of Sony's serial library linked into its exe:
*   _sio_control(cmd, sub, arg)  0x80154354   lines, status, settings
*   sync read(buf, n)            0x80153DF4   the "sio:" driver's read (BIOS read() on the sio: handle)
*   sync write(buf, n)           0x801540D8   the "sio:" driver's write
* (the link page and setup screen, loaded from DATA.MIX, call the same ones). Each gets a jump to this code, which
* passes the bytes and the handshake lines through memory rings (VLinkBuf) instead of the serial port:
*   - tx: records written by the game's side, in order: [0, len lo, len hi, bytes...] data, [1, lines] line change
*         (lines: bit 0 DTR, bit 1 RTS, as _sio_control(0,1) returns them), [2, n lo, n hi] credit (a read wants n
*         bytes), [3, n lo, n hi] credit taken back (a read gave up with n bytes missing);
*   - credit_in: the bytes the other side's reads have asked for (the carrier adds the credits received);
*   - rx: the other console's data bytes;
*   - ln: the other console's line changes, one at a time: the next one is only taken after the game has looked at
*         the lines once (a short pulse, e.g. RTS up for one frame, is seen even if two changes arrive together).
* Whoever carries the link (the emulator's Lua script, later the computer over USB) moves the rings' contents and is
* called through vlink_service() whenever the game waits or sends.
*
* What the virtual port does like the real one:
*   - status (_sio_control(0,0)): TX ready and empty always, RX ready if bytes are waiting, DSR = the other's DTR,
*     CTS = the other's RTS;
*   - read blocks until it has all its bytes, calling the game's poll callback (set with _sio_control(4,0,f)) on each
*     wait, so the game's own timeout still applies; at the end RTS is low (the library's read drops it);
*   - write sends only what the other side's reads asked for (credit), like the cable, where the sender's CTS is the
*     reader's RTS, up during its read only: a write waits for the other side to read, calling the poll callback
*     (a writer can't run ahead: Red Alert's setup screen sends its settings every frame, and the extra copies
*     would be read later as game messages);
*   - a full reset (_sio_control(2,0)) brings the lines back to the last _sio_control(1,0) setting and drops the
*     received bytes (the FIFO reset), and the credit not used yet.
* Everything else (mode, baud, ...) still goes to the library (its hardware writes do no harm).
*/
#include <stdint.h>

// PS1 DEV CART build (VLINK_HW): the code goes in the serial library's own routines the patch makes dead (the sync
// read and write and the async interrupt handler: three gaps, see vlink_hw.ld), the rings in the kernel hook's cheat
// table space (unused without cheats): no RAM of the game is used (its free-looking RAM wasn't: BILL's screen went blank)
#ifdef VLINK_HW
#define	IN(gap)		__attribute__((section(gap)))
#else
#define	IN(gap)
#endif

#define	VLINK_MAGIC		0x4B4E4C56	// "VLNK"
#ifndef RX_SIZE
#define	RX_SIZE			4096		// Powers of 2
#define	TX_SIZE			4096
#endif
#define	LN_SIZE			16

// Layout for short USB exchanges (ps1transfer -vlink writes a block then reads the bytes after it, in one exchange):
// the receive area, then the carrier's counters, then the console's, then the send area. A turn with nothing to give
// writes the carrier's counters only and reads the console's counters and the start of the send area (95 bytes in all
// instead of 347 with the rings before). So neither area is a ring:
//   - rx: the carrier gives bytes only when the console has read all it had (rx_tail == rx_head), placed so that they
//     end where the area ends (just before its counters: one block with them): byte i of n at rx[RX_SIZE - n + i], the
//     next one to read at rx[RX_SIZE - (rx_head - rx_tail)];
//   - tx: the console's bytes start at the beginning of the area (just after its counters) each time the carrier has
//     taken them all: tx_base = tx_head then, byte k at tx[k - tx_base].
typedef struct
{
	uint32_t	magic;
	volatile uint8_t	rx[RX_SIZE];
	// Written by the carrier (Lua / computer)
	volatile uint32_t	rx_head;		// rx bytes written
	volatile uint32_t	tx_tail;		// tx bytes taken
	volatile uint32_t	ln_head;		// line changes written
	volatile uint32_t	credit_in;		// bytes asked for by the other side's reads
	volatile uint32_t	abort;			// raised by the carrier: the waits going on end (the other side gone)
	volatile uint8_t	ln_ring[LN_SIZE];
	// Written by the game's side
	volatile uint32_t	rx_tail;
	volatile uint32_t	tx_head;
	volatile uint32_t	ln_tail;
	volatile uint32_t	my_lines;		// bit 0 DTR, bit 1 RTS
	volatile uint32_t	saved_lines;	// the last _sio_control(1,0) (restored by a reset)
	volatile uint32_t	peer_lines;		// the other's lines as the game sees them now
	volatile uint32_t	peer_seen;		// the game has looked at peer_lines
	volatile uint32_t	credit_used;	// bytes sent against credit_in
	volatile uint32_t	waiting;		// set while vlink_service() is called from a wait for the other side: 1 a read
										// (for rx bytes), 2 a write (for credit)
	volatile uint32_t	tx_base;		// tx_head when the send area was last empty (its first byte's number)
	volatile uint8_t	tx[TX_SIZE];
	volatile uint32_t	stats[8];		// reads, writes, bytes read, bytes written, read timeouts, resets, (unused),
										// write timeouts
} VLinkBuf;

#define	LB			((VLinkBuf *)VLINK_BUF)
// stats[6]: Net Play's auto word (vlink_head.S vlink_auto_entry), written by the computer only
_Static_assert(__builtin_offsetof(VLinkBuf, stats[6]) == 0x168, "vlink_head.S VLINK_AUTO");
#define	POLL_CALLBACK	(*(int (**)(int, int))0x801F8898)	// the library's callback (_sio_control(4,0,f))

#ifndef VLINK_HW
extern void	vlink_service(void);			// vlink_head.S: the carrier's hook (emulator: a breakpoint there)
#endif

#ifdef VLINK_HW
// PS1 DEV CART: the computer moves the rings with memory reads and writes ("ps1transfer -memlive"), which the kernel
// hook serves at each interrupt. Here the hook's USB service is also run each time the game waits or sends, so a
// request is answered at once instead of at the next interrupt (about a frame). With interrupts off: the hook
// runs the same service from the interrupts (kernel hook params at 0x8000DF70, see kernel_hook/kernel_hook.c).
typedef struct
{
	uint8_t	is_enabled, tty_mode, usb_packet_log2, features;
	int		(*p_USB_Process)(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
} KernelHookParams;
#define	KHP			((volatile KernelHookParams *)0x8000DF70)
#define	USB_STATUS	(*(volatile uint8_t *)0x1F060000)
#define	USB_STATUS_NOT_READY	(1 << 2)	// (FT245R PWREN, inverted)

IN(".text.a") void	vlink_service(void)
{
	uint32_t	sr;

	if (!KHP->is_enabled || (USB_STATUS & USB_STATUS_NOT_READY))
		return;
	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr & ~1));
	KHP->p_USB_Process(0, 0, 0, 0, 0);
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr));
}
#endif

// The game waits for the other side: the carrier may hold the console there a moment (the emulator: so as not to
// run on to its next frame first)
static inline void	vlink_wait(uint32_t what)
{
	LB->waiting = what;
	vlink_service();
	LB->waiting = 0;
}
extern int	vlink_orig_control(int cmd, int sub, int arg);

static inline __attribute__((always_inline)) uint32_t	rx_avail(void)	{ return LB->rx_head - LB->rx_tail; }

// (the send area starts again at its beginning once the carrier has taken everything: see VLinkBuf)
IN(".text.b") static __attribute__((noinline)) void	tx_put(uint8_t b)
{
	for (;;)
	{
		if (LB->tx_tail == LB->tx_head)
			LB->tx_base = LB->tx_head;
		if (LB->tx_head - LB->tx_base < TX_SIZE)
			break;
		vlink_service();
	}
	LB->tx[LB->tx_head - LB->tx_base] = b;
	LB->tx_head++;
}

IN(".text.a") static void	set_lines(uint32_t lines)
{
	lines &= 3;
	if (lines == LB->my_lines)
		return;
	LB->my_lines = lines;
	tx_put(1);
	tx_put(lines);
	vlink_service();
}

// The next line change of the other console, once the game has seen the current state
IN(".text.b") static void	lines_update(void)
{
	vlink_service();
	if (LB->peer_seen && LB->ln_tail != LB->ln_head)
	{
		LB->peer_lines = LB->ln_ring[LB->ln_tail & (LN_SIZE - 1)];
		LB->ln_tail++;
		LB->peer_seen = 0;
	}
}

IN(".text.c") int	vlink_control(int cmd, int sub, int arg)
{
	switch (cmd)
	{
	case 0:
		if (sub == 0)		// status
		{
			uint32_t	st = 5;
			lines_update();
			if (rx_avail())
				st |= 2;
			if (LB->peer_lines & 1)
				st |= 0x80;
			if (LB->peer_lines & 2)
				st |= 0x100;
			LB->peer_seen = 1;
			return st;
		}
		if (sub == 1)		// own lines
			return LB->my_lines;
		break;
	case 1:
		if (sub == 0)		// set DTR (bit 0), RTS (bit 1)
		{
			LB->saved_lines = arg & 3;
			set_lines(arg);
			return 0;
		}
		break;
	case 2:
		if (sub == 0)		// full reset
		{
			LB->stats[5]++;
			LB->rx_tail = LB->rx_head;
			LB->credit_used = LB->credit_in;
			set_lines(LB->saved_lines);
		}
		break;				// (the library's reset too: its pending state)
	case 3:
		if (sub == 0)		// RTS
		{
			set_lines(arg ? LB->my_lines | 2 : LB->my_lines & ~2);
			return 0;
		}
		if (sub == 1)		// CTS
		{
			lines_update();
			LB->peer_seen = 1;
			return (LB->peer_lines & 2) ? 1 : 0;
		}
		break;
	}
	return vlink_orig_control(cmd, sub, arg);
}

// A wait for the other side: given up like a timeout of the real port, by the game's poll callback (its own count of
// loop passes) or when the carrier raises abort: it measures real time (3 s waiting with nothing coming). The game's
// count alone took minutes here (each pass much longer, the callback even waiting a frame in some states: when the
// other console left a link game, this one looked frozen); a console timer didn't do: GPUSTAT bit 31 changes every
// scanline on a PS1 in 240-line mode (each wait gave up at once), every frame in the emulator.
IN(".text.b") static int	gave_up(int what, int *polls, uint32_t abort0)
{
	if (LB->abort != abort0)
		return 1;
	return POLL_CALLBACK && !POLL_CALLBACK(what, (*polls)++);
}

IN(".text.a") int	vlink_read(uint8_t *buf, int n)
{
	int		got = 0, polls = 0;
	uint32_t	want, abort0 = LB->abort;
	LB->stats[0]++;
	vlink_service();
	want = rx_avail() < (uint32_t)n ? n - rx_avail() : 0;
	if (want)
	{
		tx_put(2);
		tx_put(want & 0xFF);
		tx_put(want >> 8);
	}
	while (got < n)
	{
		vlink_service();
		while (got < n && rx_avail())
		{
			buf[got++] = LB->rx[RX_SIZE - rx_avail()];
			LB->rx_tail++;
		}
		if (got < n)
			vlink_wait(1);
		if (got < n && gave_up(1, &polls, abort0))
		{
			uint32_t	missing = n - got;
			LB->stats[4]++;
			if (missing > rx_avail())
			{
				missing -= rx_avail();
				tx_put(3);
				tx_put(missing & 0xFF);
				tx_put(missing >> 8);
				vlink_service();
			}
			break;
		}
	}
	LB->stats[2] += got;
	set_lines(LB->my_lines & ~2);
	return got;
}

// Red Alert's in-game exchange made symmetric (build_hw.py WORDS: two instructions of the game's exchange function,
// 0x80115264): both consoles write first then read, instead of one reading first, so they send at once and do their
// game work at the same time (its messages don't depend on what was just received: traced 2026-11-13). Its write
// comes here instead of the BIOS write: the bytes go out without waiting for the other side's request (both would
// wait); credit already given is used up, so none is left over for a later write (the setup screen's, still waiting
// for requests). Only the in-game messages come this way: the setup screen and link page are DATA.MIX code.
IN(".text.b") int	vlink_free_write(int fd, const uint8_t *buf, int n)
{
	int32_t	credit = (int32_t)(LB->credit_in - LB->credit_used);
	int		i;

	(void)fd;
	tx_put(0);
	tx_put(n & 0xFF);
	tx_put(n >> 8);
	for (i = 0; i < n; i++)
		tx_put(buf[i]);
	if (credit > 0)
		LB->credit_used += (credit < n) ? credit : n;
	vlink_service();
	return n;
}

IN(".text.c") int	vlink_write(const uint8_t *buf, int n)
{
	int		sent = 0, polls = 0, i;
	uint32_t	abort0 = LB->abort;
	LB->stats[1]++;
	while (sent < n)
	{
		int32_t	credit;
		vlink_service();
		credit = (int32_t)(LB->credit_in - LB->credit_used);
		if (credit > 0)
		{
			int		k = n - sent;
			if (k > credit)
				k = credit;
			tx_put(0);
			tx_put(k & 0xFF);
			tx_put(k >> 8);
			for (i = 0; i < k; i++)
				tx_put(buf[sent + i]);
			LB->credit_used += k;
			sent += k;
			continue;
		}
		vlink_wait(2);
		if (gave_up(2, &polls, abort0))
		{
			LB->stats[7]++;
			break;
		}
	}
	LB->stats[3] += sent;
	// (always: the loop's last service was before the bytes went in the ring. Without it the bytes waited for the next
	// interrupt: BEN's game hung twice, at the setup screen and on leaving a game. A wait here until they were taken
	// (the "drain") made it slower: 20 -> 15-18 frames a second)
	vlink_service();
	return sent;
}
