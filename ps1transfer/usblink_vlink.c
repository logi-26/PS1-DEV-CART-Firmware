/*
* PS1 DEV CART Transfer tool: -vlink, the virtual link cable of Red Alert Retaliation (ps1-linkplay/virtual) (from usblink.c)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "usblink_int.h"

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

									// as the real port with no partner (the game retries many times before it gives up)
									// waiting, tx_base (then tx[], stats[8])
static volatile int		vl_pcopy = 1;	// (2026-11-13: 23.8 -> 24.9 fps, no mismatch)

static volatile uint32_t	vl_delay_us;

static void	VlTimeAdd(volatile VlTime *t, uint32_t us)
{
	t->n++;
	t->sum += us;
	if (us > t->max)
		t->max = us;
}

static VlEv				vl_trace[VL_TRACE_MAX];
static volatile long	vl_trace_n;
volatile int		vl_trace_on, vl_fake;
// Line changes held back from a console that isn't in link mode (its own DTR and RTS low: at its menus): the last one
// given once steady for VL_HOLD_MS, or at once when it raises a line (VLINK_TUNE "hold 0/1"). Short pulses are not
// shown to it then. BEN froze at its main menu each time BILL's game, left alone,
// finished timing out and its last line changes reached it: the driver shows every change at least once (the link
// page's one-frame RTS pulse), so BEN's menu may have seen a raised line and started linking there
static volatile int		vl_hold = 0;	// (off: it held back line changes in the network handshake; the freezes it was for were the relay giving up)
volatile uint64_t	vl_trace_until, vl_trace_t0;

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

uint32_t	VlMs(void)
{
#ifdef _WIN32
	return (GetTickCount());
#else
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000));
#endif
}

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

uint64_t	VlUs(void)
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

static unsigned int		vl_join_last, vl_leave_last;	// (lobby commands in VLINK_TUNE: acted on when changed)
volatile int		vl_lobby, vl_paired;		// -vlinklobby: connected to the lobby server, paired there
static volatile int		vl_sym = 1;	// (on when the instructions are given; "sym 0" in VLINK_TUNE turns it off)
static int				vl_sym_n = -1;
static uint32_t			vl_sym_adrs[VL_SYM_WORDS], vl_sym_word[2][VL_SYM_WORDS];

static bool	VlPeerThere(VlEnd *e);

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
static bool	VlPeerThere(VlEnd *e)
{
	if (e->net)
		return VlNetThere();
	return !e->peer->failed && !e->peer->reset;
}

uint32_t	VlPatience(VlEnd *e, uint32_t now)
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
DWORD WINAPI	VlThreadMain(LPVOID arg)
{
	VlLoop((VlEnd *)arg);
	return (0);
}
#else
void	*VlThreadMain(void *arg)
{
	VlLoop((VlEnd *)arg);
	return (NULL);
}
#endif

// The rings' state read first (carried on as the consoles left it: the driver may be in use, e.g. on the link page),
// and each console's lines told to the other
bool	VlStart(VlEnd *e)
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
void	VlTraceWrite(void)
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
void	VlTimePrint(VlEnd *e)
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
