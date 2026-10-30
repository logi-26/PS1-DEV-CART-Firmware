/*
* PS1 REWIND: the library (see rewind.h).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxapi.h>
#include <psxetc.h>
#include "psyq_compat.h"
#include "psxcomb/psxcomb.h"
#include "rewind.h"

#define	HOOK_USB_PROCESS	(*(volatile uint32_t *)0x8000DF74)	// PS1 DEV CART kernel hook: its USB service
#define	HEADER				3
#define	UP_PER_FRAME		6		// Frames sent to the computer at most a frame (catching up)
#define	ASK					64		// Older frames asked at a time
#define	ASK_TIMEOUT			50		// Frames before asking again when they don't come

static uint8_t	*state;
static int		size;
static uint8_t	ring[REWIND_RING][REWIND_MAX_STATE];	// Frame f in ring[f % REWIND_RING]
static int		lo, hi = -1;		// Frames in the ring
static int		sent = -1;			// The last frame the computer has
static int		need_key = 1, truncate = -1, rewinding, waiting;
static int		pc_oldest = 0x7FFFFFFF, asked, asked_frame;
static long		link_fd = -1;
static int		welcomed, last_rx_frame, hello_frame = -100;
static int		up_bytes, down_bytes, up_rate, down_rate, second_frame;

#define	SLOT(_f_)	(ring[(_f_) & (REWIND_RING - 1)])

/****************************************/
// Link: psxcomb's USB link, packets in a send buffer flushed each frame (what doesn't fit waits)

static uint8_t	tx[8192];
static int		tx_n;
static uint8_t	rx[2048], hdr[HEADER], payload[REWIND_MAX_STATE * 2 + 64];
static int		hdr_n, type, left, got;

#define	U16(_p_)	((_p_)[0] | ((_p_)[1] << 8))
#define	U32(_p_)	(U16(_p_) | ((uint32_t)U16((_p_) + 2) << 16))

static long	NoWait(long rw, long count)
{
	return (0);			// Stop at once: a read gives what has arrived, a write what fits
}

// A packet; 0 if it doesn't fit in the send buffer now
static int	Send(int t, const void *a, int na, const void *b, int nb)
{
	if ((link_fd < 0) || (tx_n + HEADER + na + nb > (int)sizeof(tx)))
		return (0);
	tx[tx_n++] = t;
	tx[tx_n++] = na + nb;
	tx[tx_n++] = (na + nb) >> 8;
	memcpy(tx + tx_n, a, na);
	tx_n += na;
	if (nb)
		memcpy(tx + tx_n, b, nb);
	tx_n += nb;
	up_bytes += HEADER + na + nb;
	return (1);
}

static void	Put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

// What changed between two blocks: runs of skip (2), count (1), the bytes XOR. Returns its length
static int	Diff(uint8_t *out, const uint8_t *a, const uint8_t *b)
{
	int	i = 0, n = 0;

	while (i < size)
	{
		int	start = i, count;

		while ((i < size) && (a[i] == b[i]))
			i++;
		if (i == size)
			break;
		out[n++] = i - start;
		out[n++] = (i - start) >> 8;
		count = n++;
		out[count] = 0;
		while ((i < size) && (out[count] < 255))
		{
			// (a run goes on over up to 3 equal bytes: cheaper than a new run)
			if ((a[i] == b[i]) && ((i + 1 >= size) || (a[i + 1] == b[i + 1])) &&
				((i + 2 >= size) || (a[i + 2] == b[i + 2])) && ((i + 3 >= size) || (a[i + 3] == b[i + 3])))
				break;
			out[n++] = a[i] ^ b[i];
			out[count]++;
			i++;
		}
	}
	return (n);
}

// out = base XOR the runs; 0 if they don't fit the block
static int	Patch(uint8_t *out, const uint8_t *base, const uint8_t *d, int n)
{
	int	i = 0, k = 0;

	memcpy(out, base, size);
	while (k + 3 <= n)
	{
		int	count;

		i += U16(d + k);
		count = d[k + 2];
		k += 3;
		if ((i + count > size) || (k + count > n))
			return (0);
		while (count--)
			out[i++] ^= d[k++];
	}
	return (k == n);
}

static void	Packet(const uint8_t *p, int n)
{
	switch (type)
	{
		case 'W':
			if ((n >= 1) && (p[0] == REWIND_VERSION) && !welcomed)
			{
				// (a new computer, or back: it gets the whole ring, from a keyframe)
				welcomed = 1;
				need_key = 1;
				sent = lo - 1;
				pc_oldest = 0x7FFFFFFF;
			}
			break;
		case 'X':
			need_key = 1;
			break;
		case 'N':
			if (n >= 4)
				pc_oldest = U32(p);
			asked = 0;
			break;
		case 'O':			// An older frame: lo - 1 (else late: dropped), from lo
		{
			int	f = (n >= 4) ? (int)U32(p) : -1;

			if ((f == lo - 1) && (f >= 0) && (hi - f < REWIND_RING) && Patch(SLOT(f), SLOT(lo), p + 4, n - 4))
				lo = f;
			if (asked > 0)
				asked--;
			break;
		}
	}
}

static void	LinkReceive(void)
{
	long	n;
	int		i;

	for (i = 0; (i < 16) && (link_fd >= 0); i++)
	{
		const uint8_t	*p = rx;

		n = read(link_fd, rx, sizeof(rx));
		if (n <= 0)
			break;
		last_rx_frame = VSync(-1);
		down_bytes += n;
		while (n)
		{
			if (hdr_n < HEADER)
			{
				hdr[hdr_n++] = *p++;
				n--;
				if (hdr_n == HEADER)
				{
					type = hdr[0];
					left = U16(hdr + 1);
					got = 0;
				}
			}
			else
			{
				while (n && left)
				{
					if (got < (int)sizeof(payload))
						payload[got] = *p;
					got++;
					p++;
					n--;
					left--;
				}
			}
			if ((hdr_n == HEADER) && !left)
			{
				if (got <= (int)sizeof(payload))
					Packet(payload, got);
				hdr_n = 0;
			}
		}
	}
}

static void	LinkFlush(void)
{
	long	n;

	if ((link_fd < 0) || !tx_n)
		return;
	n = write(link_fd, tx, tx_n);
	if (n > 0)
	{
		memmove(tx, tx + n, tx_n - n);
		tx_n -= n;
	}
}

static int	Connected(void)
{
	return (welcomed && (VSync(-1) - last_rx_frame < 50 * 5));
}

/****************************************/

void	Rewind_Init(void *block, int block_size)
{
	state = block;
	size = (block_size > REWIND_MAX_STATE) ? REWIND_MAX_STATE : block_size;
	if (HOOK_USB_PROCESS - 0x8000C000 < 0x2000)		// The PS1 DEV CART firmware: the link
	{
		CombSetLink(COMB_LINK_USB);
		AddCOMB();
		link_fd = open("sio:", O_RDWR);
		CombWaitCallback(NoWait);
	}
	second_frame = VSync(-1);
}

// The frames the computer doesn't have yet (a keyframe when it lost the thread), a few a frame
static void	Upload(void)
{
	static uint8_t	d[REWIND_MAX_STATE * 2];
	int				k;

	if (truncate >= 0)			// (after a rewind: the computer forgets the frames after it, first)
	{
		uint8_t		b[8];
		uint32_t	sum = 0;
		int			i;

		for (i = 0; i < size; i++)
			sum += SLOT(truncate)[i];
		Put32(b, truncate);
		Put32(b + 4, sum);
		if (!Send('T', b, 8, NULL, 0))
			return;
		truncate = -1;
	}
	for (k = 0; (k < UP_PER_FRAME) && (sent < hi); k++)
	{
		int		f = sent + 1;
		uint8_t	b[4];

		Put32(b, f);
		if (need_key || (f - 1 < lo) || (sent < 0))
		{
			if (!Send('K', b, 4, SLOT(f), size))
				return;
			need_key = 0;
			pc_oldest = f;			// (the computer starts again from it)
		}
		else if (!Send('D', b, 4, d, Diff(d, SLOT(f), SLOT(f - 1))))
			return;
		sent = f;
	}
}

int		Rewind_Frame(int speed)
{
	int	now = VSync(-1), k;

	LinkReceive();
	if ((link_fd >= 0) && (now - hello_frame >= 50))	// Hello each second: the welcome back keeps the connection known
	{
		uint8_t	h[3] = {REWIND_VERSION, size, size >> 8};

		if (Send('H', h, 3, NULL, 0))
			hello_frame = now;
	}
	if (now - second_frame >= 50)
	{
		up_rate = up_bytes;
		down_rate = down_bytes;
		up_bytes = down_bytes = 0;
		second_frame = now;
		if (welcomed && !Connected())		// (the computer is gone: hello again when it's back)
			welcomed = 0;
	}

	if (speed > 0)
	{
		// Back from the ring; older frames asked from the computer to refill it behind
		if (!rewinding)
		{
			rewinding = 1;
			asked = 0;
		}
		waiting = 0;
		for (k = 0; k < speed; k++)
		{
			if (hi <= lo)
			{
				waiting = Connected() && (lo > pc_oldest);
				break;
			}
			hi--;
		}
		if (hi >= 0)
			memcpy(state, SLOT(hi), size);
		if (sent > hi)
			sent = hi;
		if (asked && (now - asked_frame > ASK_TIMEOUT))
			asked = 0;
		if (Connected() && !asked && (lo > pc_oldest) && (lo > 0) && (hi - lo + 1 + ASK <= REWIND_RING))
		{
			uint8_t	b[6];
			int		n = (lo - pc_oldest < ASK) ? lo - pc_oldest : ASK;

			Put32(b, lo);
			b[4] = n;
			b[5] = n >> 8;
			if (Send('B', b, 6, NULL, 0))
			{
				asked = n;
				asked_frame = now;
			}
		}
		LinkFlush();
		return (1);
	}
	if (rewinding)
	{
		rewinding = 0;
		waiting = 0;
		truncate = hi;
	}
	return (0);
}

void	Rewind_Save(void)
{
	hi++;
	if (hi - lo >= REWIND_RING)
		lo = hi - REWIND_RING + 1;
	memcpy(SLOT(hi), state, size);
	if (sent < lo - 1)			// (the computer is too far behind: the ring went past what it has, a keyframe)
	{
		sent = lo - 1;
		need_key = 1;
	}
	if (welcomed)
		Upload();
	LinkFlush();
}

void	Rewind_GetInfo(RewindInfo *info)
{
	int	oldest = lo;

	if (Connected() && (pc_oldest < oldest) && (pc_oldest <= sent + 1))
		oldest = pc_oldest;
	info->connected = Connected();
	info->rewinding = rewinding;
	info->ring_frames = hi - lo;
	info->frames_back = hi - oldest;
	info->up_bytes = up_rate;
	info->down_bytes = down_rate;
	info->waiting = waiting;
}
