/*
* Pad play driver for Crash Team Racing (USA, SCUS_944.26): 2-4 consoles run the same game in lockstep, only the
* players' buttons cross the network (like XBAND). Research: ps1-linkplay/research/ctr/NOTES.md.
*
* Its code replaces Time Trial's ghost replay (GhostReplay_ThTick 0x80026ED8, 0x960 bytes: only ever run in Time
* Trial, which a Net Play game never is); its data is the exchange area in the kernel hook's cheat table space
* (PL_BUF, the cheat codes stay off). The computer (or the emulator's carrier) writes the "pc" part and reads the
* "ps1" part with the kernel hook's exchange.
*
* Patches (build.py): the game's per-frame pad function (GAMEPAD_ProcessAnyoneVars 0x800262D0) -> pl_frame_stub, the
* race start's frame step store (0x8003CB78) -> pl_load_stub, the BIOS rand stub (0x80078BE8) -> pl_rand, the frame
* step's "negative -> 32" branch (0x80034ED4) a nop (a fixed step of 1/30 s).
*
* While active (pc.flags & PL_ACTIVE):
*   - a fake multitap with pc.players pads on port 1: libpad's own copy of port 1 goes to a spare buffer (its pointer
*     0x8009ECDC), the game's buffer (0x80096AD4) is written only here, each frame;
*   - races ("epochs"): a load's start (the load hook, or the LOADING flag) ends one and seeds the generators the same
*     on every console (the BIOS rand replaced, RngDeadCoed's states 0x8008D668 and gGT+0x252C, MixRNG 0x8008D424) and
*     resets the counters the game's logic reads (gGT+0x1CEC, +0x1CF8); the first frame after the load starts the next;
*   - each frame f of a race: this player's buttons for f + delay published (ps1.out), the others' for f waited for
*     (pc.have / pc.buttons, written by the computer), all players' written into the multitap; a hash of the game's
*     state for the frame published (desync check by the computer).
*/
#include <stdint.h>

#ifndef PL_BUF
#define	PL_BUF		0x8000D810
#endif
#define	IN			__attribute__((section(".text.pl")))

#define	PL_MAGIC	0x4B4E4C50	// "PLNK"
#define	PL_ACTIVE	1			// pc.flags: on
#define	PL_TEST		2			// pc.flags: this player's buttons from pc.test_buttons (emulator tests)
#define	PL_WIN		16			// frames kept per player in the windows (a power of 2)

typedef struct
{
	// The computer's part (it writes, the console reads)
	uint32_t	magic;					// +0x00 PL_MAGIC
	uint8_t		players, me, delay, flags;	// +0x04 players 1-4, me 0-3, input delay (frames)
	uint32_t	seed;					// +0x08
	uint16_t	epoch;					// +0x0C the race the windows below are for
	uint16_t	test_buttons;			// +0x0E (PL_TEST)
	uint16_t	have[4];				// +0x10 per player: the last frame given (0xFFFF none yet)
	uint16_t	buttons[4][PL_WIN];		// +0x18 per player: buttons (bits set = pressed) of frame f at [f & 15]
	uint32_t	abort;					// +0x98 nonzero: stop waiting (missing players' buttons = none)
	uint32_t	pc_pad;					// +0x9C
	// The console's part (it writes, the computer reads)
	uint16_t	epoch_now;				// +0xA0 the race running (0 none yet)
	uint16_t	frame;					// +0xA2 the last frame done in it
	uint16_t	out_upto;				// +0xA4 the last frame of this player's published
	uint16_t	state;					// +0xA6 0 off, 1 loading, 2 racing, 3 waiting
	uint16_t	out[PL_WIN];			// +0xA8 this player's buttons of frame f at [f & 15]
	uint32_t	hash[8];				// +0xC8 the game's state hash after frame f at [f & 7]
	uint32_t	waits, wait_loops;		// +0xE8 frames that waited, service calls while waiting
	uint32_t	loads;					// +0xF0
	uint32_t	ps1_pad;				// +0xF4
	// Private
	uint8_t		spare_pad[0x24];		// +0xF8 libpad's port 1 copy (the console's real pad 1)
	uint16_t	racing, loading_seen;	// +0x11C
	int32_t		cur_frame;				// +0x120
	uint32_t	rnd;					// +0x124 the rand generator
} PadLink;

#define	PL			((volatile PadLink *)PL_BUF)
#define	U32(a)		(*(volatile uint32_t *)(a))
#define	U16(a)		(*(volatile uint16_t *)(a))
#define	U8(a)		(*(volatile uint8_t *)(a))

// The game (USA SCUS_944.26; CTR-ModSDK names)
#define	GGT			U32(0x8008D2AC)				// sdata->gGT
#define	LOADING		0x40000000					// gGT->gameMode1
#define	GAME_BUF	0x80096AD4					// port 1's pad buffer the game reads (0x22 bytes)
#define	LIBPAD_DST	0x8009ECDC					// libpad's pointer to where it copies port 1
#define	RNG_A		0x8008D668					// RngDeadCoed state (2 words)
#define	RNG_MIX		0x8008D424					// MixRNG state (16 bits)
#define	RAW_PAD1	0x8009F60B					// (unused: libpad's receive)

#ifdef PL_HW
// PS1 DEV CART: the kernel hook's USB service run while waiting, so the computer's exchange is answered at once
typedef struct
{
	uint8_t	is_enabled, tty_mode, usb_packet_log2, features;
	int		(*p_USB_Process)(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
} KernelHookParams;
#define	KHP			((volatile KernelHookParams *)0x8000DF70)
#define	USB_STATUS	(*(volatile uint8_t *)0x1F060000)
#define	USB_STATUS_NOT_READY	(1 << 2)
#endif

// The carrier's hook: the console serves the computer's USB exchange here; the emulator's carrier breaks here
IN __attribute__((noinline)) void	pl_service(void)
{
#ifdef PL_HW
	uint32_t	sr;

	if (!KHP->is_enabled || (USB_STATUS & USB_STATUS_NOT_READY))
		return;
	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr & ~1));
	KHP->p_USB_Process(0, 0, 0, 0, 0);
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr));
#else
	__asm__ volatile("" ::: "memory");
#endif
}

// The BIOS's generator (seed * 0x41C64E6D + 0x3039, bits 16-30), on a seed of our own
IN int	pl_rand(void)
{
	uint32_t	r = PL->rnd * 0x41C64E6D + 0x3039;

	PL->rnd = r;
	return ((r >> 16) & 0x7FFF);
}

IN static void	reset_counters(uint32_t g)
{
	U32(g + 0x1CEC) = 0;
	U32(g + 0x1CF8) = 0;
}

// A load starts: the generators seeded from the seed and the race's number, the counters reset
IN static void	load_starts(void)
{
	uint32_t	g = GGT, s;

	if (PL->loading_seen)
		return;
	PL->loading_seen = 1;
	PL->loads++;
	PL->state = 1;
	s = PL->seed + PL->epoch_now + 1;
	PL->rnd = s;
	U32(RNG_A) = 0x30215400 ^ (s * 2654435761u);
	U32(RNG_A + 4) = 0x493583FE ^ s;
	U32(g + 0x252C) = 0x12345678 ^ (s * 40503u);
	U32(g + 0x2530) = 0x9ABCDEF0 ^ s;
	U16(RNG_MIX) = (uint16_t)(s * 7919u);
	reset_counters(g);
}

// The fake multitap: slots 0..players-1 connected (status 0, type 41h, buttons active low), the rest absent
IN static void	fill_multitap(const uint16_t *b)
{
	volatile uint8_t	*p = (volatile uint8_t *)GAME_BUF;
	int					s, i;

	p[0] = 0;
	p[1] = 0x80;
	for (s = 0; s < 4; s++)
	{
		volatile uint8_t	*q = p + 2 + 8 * s;

		if (s < PL->players)
		{
			uint16_t	v = (uint16_t)~b[s];

			q[0] = 0;
			q[1] = 0x41;
			q[2] = v & 0xFF;
			q[3] = v >> 8;
			for (i = 4; i < 8; i++)
				q[i] = 0xFF;
		}
		else
		{
			q[0] = 0xFF;
			for (i = 1; i < 8; i++)
				q[i] = 0;
		}
	}
}

// This console's real pad 1 (libpad's copy, now in spare_pad): bits set = pressed; none if no pad answered
IN static uint16_t	local_buttons(void)
{
	volatile uint8_t	*p = PL->spare_pad;

	if (PL->flags & PL_TEST)
		return (PL->test_buttons);
	if (p[0] != 0)
		return (0);
	return ((uint16_t)~(p[2] | (p[3] << 8)));
}

// The game's state after a frame, hashed: every driver's position, the generators, the race's clock
IN static uint32_t	state_hash(uint32_t g)
{
	uint32_t	h = U32(RNG_A) ^ (U16(RNG_MIX) << 7) ^ PL->rnd;
	int			i, k;

	for (i = 0; i < 8; i++)
	{
		uint32_t	d = U32(g + 0x24EC + 4 * i);

		if (!d)
			continue;
		for (k = 0; k < 3; k++)
			h = ((h << 5) | (h >> 27)) ^ U32(d + 0x2D4 + 4 * k);	// posCurr
	}
	return (h ^ U32(g + 0x1D10));		// elapsedEventTime
}

// Once per pass of the game's main loop (where it reads its pads)
IN void	pl_frame(void)
{
	volatile PadLink	*pl = PL;
	uint32_t			g, f, d;
	uint16_t			cur[4];
	int					p;

	if ((pl->magic != PL_MAGIC) || !(pl->flags & PL_ACTIVE))
	{
		if (U32(LIBPAD_DST) == (uint32_t)pl->spare_pad)	// switched off: libpad's copy back to the game's buffer
			U32(LIBPAD_DST) = GAME_BUF;
		pl->state = 0;
		return;
	}
	if (U32(LIBPAD_DST) == GAME_BUF)
		U32(LIBPAD_DST) = (uint32_t)pl->spare_pad;
	g = GGT;
	for (p = 0; p < 4; p++)
		cur[p] = 0;
	if (U32(g) & LOADING)
	{
		pl->racing = 0;
		load_starts();
		fill_multitap(cur);
		return;
	}
	d = pl->delay;
	if (!pl->racing)
	{
		if (!pl->loading_seen)
		{
			fill_multitap(cur);
			return;
		}
		// A load just ended: the next race starts here, on every console alike
		pl->loading_seen = 0;
		pl->racing = 1;
		pl->epoch_now++;
		pl->cur_frame = -1;
		pl->frame = 0xFFFF;
		reset_counters(g);
		for (f = 0; f < d; f++)
			pl->out[f & (PL_WIN - 1)] = 0;
		pl->out_upto = (uint16_t)(d - 1);
	}
	f = (uint32_t)++pl->cur_frame;
	// This player's buttons for frame f + delay
	pl->out[(f + d) & (PL_WIN - 1)] = local_buttons();
	pl->out_upto = (uint16_t)(f + d);
	pl->state = 2;
	// The others' for frame f
	for (p = 0; p < pl->players; p++)
	{
		if (p == pl->me)
		{
			cur[p] = pl->out[f & (PL_WIN - 1)];
			continue;
		}
		if ((pl->epoch != pl->epoch_now) || (pl->have[p] == 0xFFFF) || ((int16_t)(pl->have[p] - (uint16_t)f) < 0))
		{
			pl->waits++;
			pl->state = 3;
			while (((pl->epoch != pl->epoch_now) || (pl->have[p] == 0xFFFF) ||
					((int16_t)(pl->have[p] - (uint16_t)f) < 0)) && !pl->abort)
			{
				pl->wait_loops++;
				pl_service();
			}
			pl->state = 2;
		}
		cur[p] = pl->abort ? 0 : pl->buttons[p][f & (PL_WIN - 1)];
	}
	fill_multitap(cur);
	pl->hash[(f - 1) & 7] = state_hash(g);		// (the state after the previous frame)
	pl->frame = (uint16_t)f;
	pl_service();
}

// A load's start (the race start's frame step store, 0x8003CB78): a race ends here
IN void	pl_load(void)
{
	volatile PadLink	*pl = PL;

	if ((pl->magic != PL_MAGIC) || !(pl->flags & PL_ACTIVE))
		return;
	pl->racing = 0;
	load_starts();
}
