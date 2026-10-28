/*
* PS1 DEV CART VIDEO TESTER: plays an STR movie (MDEC video, XA sound) from the computer's files, as a game plays it
* from the CD: the sectors at the drive's pace (1x: 75 a second, 2x: 150), each frame decoded once its sectors are in,
* the sound heard with it.
*
* The movie is read through the PS1 DEV CART file server (PCDRV: "ps1transfer -e -f videotest.ps-exe -c -p FOLDER"):
* - RAM: the whole movie loaded first (when it fits: about 1 MB, 3 seconds at 2x), then played at the exact pace;
* - stream: read while it plays (the file server gives about 400 KB/s, the CPU busy meanwhile: 1x movies play at their
*   pace, 2x ones too fast for it: the clock waits, "stalls" are counted).
* Sectors of 2336 bytes (Mode 2: subheader + data, as made for disc images), 2352 (raw CD sectors, also after a RIFF
* CDXA header) or 2048 (video only). Frames: BS versions 1, 2 and 3 (psxpress DecDCTvlc), up to 640 pixels wide
* (wider than 320: drawn at half width), decoded by the MDEC in 15 bits (the picture drawn on the screen, scaled or not,
* with the information panel) or 24 bits (as most games show their movies: no drawing over it).
* Sound: XA ADPCM 4 bits (37800 or 18900 Hz, mono or stereo), the CD-ROM controller's decoder, played here by the SPU:
* its sound units are SPU ADPCM blocks (the same filters and shifts) streamed to two voices (left, right), which are
* also the clock of the movie (no drift between the picture and the sound).
*
* Commands, one per line, read from the computer (the file server's "@input": the lines sent to ps1transfer's input,
* e.g. by the PS1 Dev Cart Manager's Movie Tools):
*	open NAME [speed 1|2] [mode auto|ram|stream] [loop 0|1]	plays a movie of the served folder
*	play, pause, toggle, restart, step (paused: the next frame), loop 0|1, depth 15|24, scale 1x|fit|full,
*	info 0|1, volume 0-100, close, quit
* Lines printed (the console of ps1transfer): "VIDTEST ready", "VIDTEST open NAME SECTORS SECTOR_SIZE MODE",
* "VIDTEST pos FRAME FRAMES DECODE_US BYTES SCALE STALLS LATE DROPPED BUF%" (frames: the last shown, the most seen;
* late: shown after their time; dropped: incomplete), "VIDTEST info W H RATE STEREO" (the movie's picture and sound),
* "VIDTEST end", "VIDTEST error TEXT".
* Controller: Start play/pause, Cross restart, Circle loop, Square scale, Triangle 15/24 bits, Select information,
* R1 next frame (paused), Up/Down volume.
*
* The MDEC decoding (the VLC data in the scratchpad, the frame decoded by the MDEC in the background then uploaded
* strip after strip) and the SPU sound ring come from the PS1 DEV CART video streamer.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxspu.h>
#include <psxpress.h>
#include "psyq_compat.h"
#include "devcart_ui.h"
#include "psxpcdrv.h"

#define	SCREEN_W		320
#define	SCREEN_H		240

// Sectors in RAM: the whole movie (RAM mode) or a ring read ahead (stream mode), whole chunks
#define	RING_BYTES		(1024 * 1024)
#define	CHUNK			4							// Sectors per read (about 23 ms: shorter than a sound half)
static uint8_t		ring[RING_BYTES] __attribute__((aligned(4)));

// Frames: the payloads of a frame's sectors put together (2 buffers: one waiting, one being received)
#define	FRAME_MAX		(64 * 1024)
#define	VIDEO_HEADER	32
#define	VIDEO_PAYLOAD	2016
static uint32_t		frame_data[2][FRAME_MAX / 4];

// Decoding: MDEC run-length codes, the MDEC output of a whole frame (up to 320x256, 24 bits) or of 2 strips (wider)
#define	RLE_WORDS		40960
#define	FRAME_BUF		(320 * 256 * 3 / 4)
static uint32_t		rle[RLE_WORDS + 64];
static uint32_t		frame_buf[FRAME_BUF];

// VRAM: screens (0,0) and (0,256); 15-bit textures (decoded frames, see SlotX) from x 320, y 0 and 256
#define	TEX_X			320
#define	SCREEN_Y(_n_)	((_n_) * 256)

// Sound: SPU blocks of each channel in a RAM FIFO, then two rings in SPU RAM (two halves each)
#define	AUDIO_FIFO		32768						// Bytes per channel (power of 2): 2048 blocks, 1.5 s at 37800 Hz
#define	HALF_BLOCKS		128
#define	HALF_BYTES		(HALF_BLOCKS * 16)
#define	RING_ADRS(_c_)	(0x2000 + (_c_) * 2 * HALF_BYTES)
#define	LEAD_US			600000						// Sound (and pictures) this much behind the sectors read (18900 Hz
															// mono: a sound sector every 213 ms at 2x)
#define	SPU_VOICE(_v_)	((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))
#define	SPU_REG(_a_)	(*((volatile uint16_t *)(_a_)))
#define	SPU_KEY_ON		SPU_REG(0x1F801D88)
#define	SPU_KEY_OFF		SPU_REG(0x1F801D8C)
#define	SPU_IRQ_ADRS	SPU_REG(0x1F801DA4)
#define	SPU_CNT			SPU_REG(0x1F801DAA)
#define	VT_SPU_STAT		SPU_REG(0x1F801DAE)
#define	SPU_IRQ_ENABLE	(1 << 6)
#define	VT_IRQ_MASK		(*((volatile uint32_t *)0x1F801074))
#define	HBLANK_COUNTER	(*((volatile uint32_t *)0x1F801110))	// Root counter 1 counting lines (decode times)
#define	HBLANK_MODE		(*((volatile uint32_t *)0x1F801114))

static DISPENV		disp[2];
static DRAWENV		draw[2];
static POLY_FT4		poly[2][3];
static DR_TPAGE		font_page[2];		// (after the picture: the debug font's texture page for the text)
static uint32_t		ot[2][2];
static int			fnt_info, is_pal;

// Movie
static int			fd = -1, input_fd = -1, server_lost;			// (lost: no more file server calls: each would wait)
static char			name[64], status_text[48];
static uint32_t		file_base, total, fmt, data_off, sub_off;	// Sectors, sector size, offsets in a sector
static uint32_t		ring_cap, loaded, consumed;					// Sectors: ring capacity, loaded, processed
static uint32_t		cur_a, cur_v;								// Next sector of the sound, of the pictures
static int			ram_mode, speed = 2, loop = 1, playing, paused, ended, step_req;
static uint32_t		play_us, last_vsync;							// Movie time (pictures and sound)
static uint32_t		stalls, stalled;
static int			primed;			// The start: the clock and the sound wait for the first frame decoded
static int			show_info = 1, depth24, scale_mode = 1;			// scale: 0 1x, 1 fit, 2 full
static int			volume = 100;

// Video
static int			asm_buf = -1, asm_number, asm_count, asm_got, asm_size, asm_w, asm_h;
static int			wait_buf = -1, wait_size, wait_w, wait_h, wait_number;
static uint32_t		wait_time, asm_time;							// When the frame is shown (movie time)
static int			frames_total, frames_shown, frames_dropped, frames_late, last_frame, last_scale, last_bytes;
static int			vid_w, vid_h, tex_shown = -1, decode_state, decode_tex, decode_w, decode_h;
#define	SLOTS			4
enum	{SLOT_FREE, SLOT_DECODING, SLOT_READY, SLOT_SHOWN};
static int			slot_state[SLOTS], slot_w[SLOTS], slot_h[SLOTS];
static uint32_t		slot_time[SLOTS];
static uint32_t		decode_time, decode_start, decode_us, decode_vsync;
static int			mdec_resets, frame_errors;
enum	{DECODE_IDLE, DECODE_MDEC, DECODE_UPLOAD};

// Sound
static uint32_t		rate, stereo, coding = 0xFF;
static uint8_t		fifo[2][AUDIO_FIFO];
static uint32_t		fifo_head[2], fifo_tail[2];
static uint8_t		half_buf[HALF_BYTES];
static int			sound_on, irq_half, underruns, silent, restarts;
static int32_t		half_pos[2], consumed_samples, clock_base = -1, clock_now;
static uint32_t		clock_vsync, last_irq_vsync, sound_base_us;	// (movie time when the sound started)

static uint32_t	VsyncUs(void)
{
	return (is_pal ? 20096 : 16715);
}

// Movie time (us) of a sector: when the drive gives it (75 or 150 sectors a second)
static uint32_t	SectorUs(uint32_t s)
{
	uint32_t	per3 = 40000 / speed;		// (us of 3 sectors: 32-bit arithmetic)

	return ((s / 3) * per3 + (s % 3) * per3 / 3);
}

/****************************************/
// Commands from the computer and status lines

static void	Status(const char *text)
{
	snprintf(status_text, sizeof(status_text), "%s", text);
}

/****************************************/
// Sound: XA sound units -> SPU ADPCM blocks (the same filters and shifts), FIFO, rings in SPU RAM

static uint32_t	FifoBlocks(int c)
{
	return ((fifo_head[c] - fifo_tail[c]) / 16);
}

static void	AudioSector(const uint8_t *d)
{
	int		g, u, k, c;
	uint8_t	*b;

	if ((coding & 0x10) || !rate)		// (8-bit XA: not played)
		return;
	if (FifoBlocks(0) * 16 + 18 * 8 * 16 > AUDIO_FIFO)	// (full: the sound is too far ahead, dropped)
		return;
	for (g = 0; g < 18; g++)
	{
		const uint8_t	*grp = d + g * 128;

		for (u = 0; u < 8; u++)
		{
			uint8_t	param = grp[4 + u];

			c = stereo ? (u & 1) : 0;
			b = &fifo[c][fifo_head[c] & (AUDIO_FIFO - 1)];
			b[0] = ((param & 15) > 12) ? ((param & 0x30) | 9) : (param & 0x3F);
			b[1] = 0;
			for (k = 0; k < 14; k++)
			{
				uint8_t	lo = (grp[16 + (2 * k) * 4 + (u >> 1)] >> ((u & 1) * 4)) & 15;
				uint8_t	hi = (grp[16 + (2 * k + 1) * 4 + (u >> 1)] >> ((u & 1) * 4)) & 15;

				b[2 + k] = lo | (hi << 4);
			}
			fifo_head[c] += 16;
		}
	}
}

static void	FillHalf(int half)
{
	int	c, i, n = HALF_BLOCKS, channels = stereo ? 2 : 1;

	// The blocks the channels both have (stereo), silence if none
	for (c = 0; c < channels; c++)
		if (FifoBlocks(c) < (uint32_t)n)
			n = FifoBlocks(c);
	if (n < HALF_BLOCKS)
	{
		if (!silent && playing && !ended && (cur_a < total))	// (not the end of the sound)
			underruns++;
		silent = 1;
		n = 0;
	}
	else
		silent = 0;
	half_pos[half] = n ? consumed_samples : -1;
	for (c = 0; c < channels; c++)
	{
		for (i = 0; i < HALF_BLOCKS; i++)
		{
			uint8_t	*b = half_buf + i * 16;

			if (n)
			{
				memcpy(b, &fifo[c][fifo_tail[c] & (AUDIO_FIFO - 1)], 16);
				fifo_tail[c] += 16;
			}
			else
				memset(b, 0, 16);
			b[1] = 0;
		}
		if (half == 1)
			half_buf[(HALF_BLOCKS - 1) * 16 + 1] = 1 | 2;	// End + repeat
		SpuSetTransferStartAddr(RING_ADRS(c) + half * HALF_BYTES);
		SpuWrite((const uint32_t *)half_buf, HALF_BYTES);
		SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
		SPU_VOICE(c)[7] = RING_ADRS(c) / 8;
	}
	if (n)
		consumed_samples += HALF_BLOCKS * 28;
}

static void	SetVolume(void)
{
	int	v = volume * 0x3FFF / 100;

	SPU_VOICE(0)[0] = v;
	SPU_VOICE(0)[1] = stereo ? 0 : v;
	SPU_VOICE(1)[0] = 0;
	SPU_VOICE(1)[1] = v;
}

static void	StopSound(void)
{
	SPU_KEY_OFF = 3;
	sound_on = 0;
	clock_base = -1;
}

static void	StartSound(void)
{
	int	c;

	SPU_KEY_OFF = 3;
	consumed_samples = 0;
	FillHalf(0);
	FillHalf(1);
	SetVolume();
	for (c = 0; c < 2; c++)
	{
		volatile uint16_t	*v = SPU_VOICE(c);

		v[2] = rate * 4096 / 44100;
		v[3] = RING_ADRS(c) / 8;
		v[4] = 0x000F;
		v[5] = 0x0000;
	}
	irq_half = 1;
	SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (RING_ADRS(0) + HALF_BYTES) / 8;
	SPU_CNT |= SPU_IRQ_ENABLE;
	SPU_KEY_ON = stereo ? 3 : 1;
	for (c = 0; c < 2; c++)
		SPU_VOICE(c)[7] = RING_ADRS(c) / 8;
	clock_base = half_pos[0];
	clock_now = (clock_base > 0) ? clock_base : 0;
	clock_vsync = last_irq_vsync = VSync(-1);
	sound_base_us = play_us;
	sound_on = 1;
}

static void	SoundService(void)
{
	if (!sound_on)
		return;
	if (!(VT_SPU_STAT & SPU_IRQ_ENABLE))
	{
		if (VSync(-1) - last_irq_vsync > 30)		// Watchdog: the interrupt address was passed
		{
			restarts++;
			StartSound();
		}
		return;
	}
	last_irq_vsync = VSync(-1);
	clock_base = half_pos[irq_half];
	if (clock_base >= 0)
		clock_now = clock_base;
	clock_vsync = last_irq_vsync;
	FillHalf(irq_half ^ 1);
	irq_half ^= 1;
	SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (RING_ADRS(0) + irq_half * HALF_BYTES) / 8;
	SPU_CNT |= SPU_IRQ_ENABLE;
}

// Samples played (does not move during silence)
static int32_t	SoundClock(void)
{
	int32_t	t;

	if (clock_base < 0)
		return (clock_now);
	t = clock_base + (int32_t)((VSync(-1) - clock_vsync) * rate / (is_pal ? 50 : 60));
	if (t > clock_base + HALF_BLOCKS * 28)
		t = clock_base + HALF_BLOCKS * 28;
	if (t > clock_now)
		clock_now = t;
	return (clock_now);
}

/****************************************/
// Video: frames put together from their sectors, decoded in the background, shown at their time

static void	MdecInit(void)
{
	int	i;

	DecDCTReset(1);
	for (i = 0; i < 1000; i++)
		__asm__ volatile ("nop");
	MDEC1 = 0x60000000;
	DecDCTPutEnv(NULL, 0);
}

// Decoded frames wait for their time in texture slots (15 bits, up to 320 wide: 4 slots, so the decoder works up to 3
// frames ahead of the one shown: decoding one only after the previous one was shown made them late when the file
// server's reads took the CPU; wider or 24 bits: 2, the 24-bit ones are the screens)
static int	SlotX(int n)
{
	return (depth24 ? 0 : TEX_X + (n >> 1) * 320);
}

static int	SlotY(int n)
{
	return ((n & 1) * 256);
}

static int	Slots(int w)
{
	return ((depth24 || (w > 320)) ? 2 : SLOTS);
}

// The decoded frame with the earliest time, -1 if none
static int	ReadySlot(void)
{
	int	i, best = -1;

	for (i = 0; i < SLOTS; i++)
		if ((slot_state[i] == SLOT_READY) && ((best < 0) || (slot_time[i] < slot_time[best])))
			best = i;
	return (best);
}

static void	ResetSlots(void)
{
	int	i;

	for (i = 0; i < SLOTS; i++)
		if (slot_state[i] != SLOT_DECODING)
			slot_state[i] = SLOT_FREE;
	tex_shown = -1;
}

static void	ResetVideo(void)
{
	asm_buf = wait_buf = -1;
	if (decode_state == DECODE_MDEC)
		MdecInit();
	decode_state = DECODE_IDLE;
	memset(slot_state, 0, sizeof(slot_state));
	tex_shown = -1;
}

// A frame whose sectors are all in: it waits for the decoder (a frame already waiting is dropped: too late)
static void	FrameComplete(void)
{
	if (wait_buf >= 0)
		frames_dropped++;
	wait_buf = asm_buf;
	wait_size = asm_size;
	wait_w = asm_w;
	wait_h = asm_h;
	wait_number = asm_number;
	wait_time = asm_time;
	asm_buf = -1;
	if (asm_number > frames_total)
		frames_total = asm_number;
}

static void	VideoSector(uint32_t s, const uint8_t *d)
{
	int	index = d[4] | (d[5] << 8), count = d[6] | (d[7] << 8);
	int	number = d[8] | (d[9] << 8) | (d[10] << 16) | (d[11] << 24);
	int	size = d[12] | (d[13] << 8) | (d[14] << 16) | (d[15] << 24);

	if ((asm_buf >= 0) && (number != asm_number))	// (a frame not complete: dropped)
	{
		frames_dropped++;
		asm_buf = -1;
	}
	if (asm_buf < 0)
	{
		if ((size > FRAME_MAX) || (count * VIDEO_PAYLOAD > FRAME_MAX + VIDEO_PAYLOAD))
		{
			frame_errors++;
			return;
		}
		asm_buf = (wait_buf == 0) ? 1 : 0;
		asm_number = number;
		asm_count = count;
		asm_got = 0;
		asm_size = size;
		asm_w = d[16] | (d[17] << 8);
		asm_h = d[18] | (d[19] << 8);
	}
	if ((index < count) && ((index + 1) * VIDEO_PAYLOAD <= FRAME_MAX + VIDEO_PAYLOAD))
	{
		int	n = ((index + 1) * VIDEO_PAYLOAD > FRAME_MAX) ? FRAME_MAX - index * VIDEO_PAYLOAD : VIDEO_PAYLOAD;

		memcpy((uint8_t *)frame_data[asm_buf] + index * VIDEO_PAYLOAD, d + VIDEO_HEADER, n);
		asm_got++;
	}
	if (asm_got >= asm_count)
	{
		asm_time = SectorUs(s);		// (shown when its last sector would be in)
		FrameComplete();
	}
}

static uint32_t	Lines(void)
{
	return (HBLANK_COUNTER & 0xFFFF);
}

// Starts decoding the waiting frame: VLC by the CPU, then the MDEC in the background (whole frame up to 320 wide)
static void	StartDecode(void)
{
	VLC_Context	ctx;
	uint32_t	n, padded, i, l0 = Lines();
	const uint32_t	*bs = frame_data[wait_buf];
	int			w = wait_w, h = (wait_h + 15) & ~15, slot;

	for (slot = 0; (slot < Slots(w)) && (slot_state[slot] != SLOT_FREE); slot++)
		;
	if (slot == Slots(w))
		return;		// (no free slot: the frames ahead wait for their time)
	last_frame = wait_number;
	last_bytes = wait_size;
	last_scale = ((const uint16_t *)bs)[2];
	decode_time = wait_time;
	wait_buf = -1;
	if ((w < 16) || (w > 640) || (h > 256) || (DecDCTvlcStart(&ctx, rle, RLE_WORDS, bs) != 0))
	{
		frame_errors++;
		return;
	}
	if (MDEC1 & (1 << 29))
	{
		mdec_resets++;
		MdecInit();
	}
	n = rle[0] & 0xFFFF;
	padded = (n + 31) & ~31;
	for (i = n; i < padded; i++)
		rle[1 + i] = 0;
	decode_w = w;
	decode_h = h;
	decode_tex = slot;
	slot_state[slot] = SLOT_DECODING;
	slot_time[slot] = decode_time;
	slot_w[slot] = w;
	slot_h[slot] = h;
	decode_start = l0;
	if (depth24 && (w <= 320))
	{
		DecDCTin(rle, DECDCT_MODE_24BPP);
		DecDCTout(frame_buf, w * h * 3 / 4);
	}
	else if (w <= 320)
	{
		DecDCTin(rle, DECDCT_MODE_16BPP_BIT15);
		DecDCTout(frame_buf, w * h / 2);
	}
	else
	{
		// Wider: strip after strip (2 strips of buffer), waiting for each one
		RECT	rect;
		int		x;

		DecDCTin(rle, DECDCT_MODE_16BPP_BIT15);
		for (x = 0; x < w; x += 16)
		{
			uint32_t	*buf = frame_buf + ((x >> 4) & 1) * (16 * 256 / 2);

			DecDCTout(buf, 16 * h / 2);
			DecDCToutSync(0);
			setRECT(&rect, SlotX(slot) + x, SlotY(slot), 16, h);
			LoadImage(&rect, buf);
		}
		decode_state = DECODE_UPLOAD;
		decode_vsync = VSync(-1);
		return;
	}
	decode_state = DECODE_MDEC;
	decode_vsync = VSync(-1);
}

static void	DecodeService(void)
{
	RECT	rect;
	int		x;

	if (decode_state == DECODE_MDEC)
	{
		if (DecDCToutSync(1))
		{
			if (VSync(-1) - decode_vsync > 30)
			{
				MdecInit();
				decode_state = DECODE_IDLE;
			}
			return;
		}
		for (x = 0; x < decode_w; x += 16)
		{
			if (depth24)	// (straight to the screen not shown: 24 halfwords a strip)
			{
				int	x0 = (SCREEN_W - decode_w) / 2 + x, y0 = (SCREEN_H - decode_h) / 2;

				setRECT(&rect, x0 * 3 / 2, SlotY(decode_tex) + (y0 > 0 ? y0 : 0), 24, decode_h);
				LoadImage(&rect, frame_buf + x * decode_h * 3 / 4);
			}
			else
			{
				setRECT(&rect, SlotX(decode_tex) + x, SlotY(decode_tex), 16, decode_h);
				LoadImage(&rect, frame_buf + x * decode_h / 2);
			}
		}
		decode_state = DECODE_UPLOAD;
	}
	if ((decode_state == DECODE_UPLOAD) && !DrawSync(1))
	{
		decode_us = ((Lines() - decode_start) & 0xFFFF) * 64;
		if (slot_state[decode_tex] == SLOT_DECODING)
			slot_state[decode_tex] = SLOT_READY;
		decode_state = DECODE_IDLE;
	}
}

static void	VideoService(void)
{
	int	r = ReadySlot();

	// The next frame decoded is shown at its time (paused: at once when stepping)
	if ((r >= 0) && ((play_us >= slot_time[r]) || step_req))
	{
		if (!step_req && (play_us > slot_time[r] + 4 * VsyncUs()))	// (late: a 15 fps frame or more)
			frames_late++;
		if ((tex_shown >= 0) && (slot_state[tex_shown] == SLOT_SHOWN))
			slot_state[tex_shown] = SLOT_FREE;
		slot_state[r] = SLOT_SHOWN;
		tex_shown = r;
		vid_w = slot_w[r];
		vid_h = slot_h[r];
		frames_shown++;
		if (step_req)
			play_us = slot_time[r];		// (paused: the time of the frame shown)
		step_req = 0;
	}
	if ((decode_state == DECODE_IDLE) && (wait_buf >= 0))
		StartDecode();
}

/****************************************/
// Sectors: loading (PCDRV) and processing at the drive's pace

static uint8_t	*Sector(uint32_t s)
{
	return (ring + (s % ring_cap) * fmt);
}

// Stream mode: the next chunk read if there is room. While it plays, only 1.5 s of sectors ahead: read just in time
// (reading is the CPU's work: reading ahead as fast as possible left too little time for decoding, frames late)
static void	Load(int chunks)
{
	uint32_t	ahead = playing && !paused ? 75 * speed * 3 / 2 : ring_cap;

	while (chunks-- && !ram_mode && (loaded < total) && (loaded - consumed + CHUNK <= ring_cap) &&
		(loaded - consumed < ahead))
	{
		uint32_t	n = (total - loaded < CHUNK) ? total - loaded : CHUNK;

		if (server_lost || (PCread(fd, (char *)Sector(loaded), n * fmt) != (int)(n * fmt)))
		{
			printf("VIDTEST error read failed at sector %u\n", (unsigned)loaded);
			server_lost = 1;
			total = loaded;
			Status("FILE SERVER GONE");
			break;
		}
		loaded += n;
	}
}

static void	Rewind(void)
{
	consumed = cur_a = cur_v = 0;
	primed = 0;
	coding = 0xFF;
	frames_shown = frames_dropped = frames_late = frame_errors = underruns = restarts = 0;
	play_us = 0;
	ended = 0;
	ResetVideo();
	StopSound();
	fifo_head[0] = fifo_tail[0] = fifo_head[1] = fifo_tail[1] = 0;
	if (!ram_mode)
	{
		loaded = 0;
		if ((fd >= 0) && (server_lost || (PClseek(fd, file_base, PCDRV_SEEK_SET) < 0)))
		{
			server_lost = 1;
			total = 0;		// (stream mode without the file server: nothing more to play)
			playing = 0;
		}
	}
}

static void	CloseMovie(void)
{
	Rewind();
	if ((fd >= 0) && !server_lost)
		PCclose(fd);
	fd = -1;
	playing = 0;
	total = 0;
	name[0] = 0;
}

// Opens a movie: its sector size from the first sectors, then loaded (RAM) or the ring filled (stream)
static void	OpenMovie(const char *file, const char *mode)
{
	uint8_t	*p = ring;
	int		size, n;

	CloseMovie();
	snprintf(name, sizeof(name), "%s", file);
	if ((fd = PCopen(name, PCDRV_READ, 0)) < 0)
	{
		printf("VIDTEST error can't open %s\n", name);
		Status("CAN'T OPEN THE FILE");
		name[0] = 0;
		return;
	}
	size = PClseek(fd, 0, PCDRV_SEEK_END);
	PClseek(fd, 0, PCDRV_SEEK_SET);
	n = PCread(fd, (char *)p, (size < 2352 * 2 + 44) ? size : 2352 * 2 + 44);
	file_base = 0;
	if ((n >= 44) && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "CDXA", 4))
	{
		file_base = 44;
		p += 44;
		n -= 44;
	}
	sub_off = 0xFFFFFFFF;
	if ((n >= 2352) && (p[0] == 0) && (p[1] == 0xFF) && (p[10] == 0xFF) && (p[11] == 0))
	{
		fmt = 2352;
		sub_off = 16;
		data_off = 24;
	}
	else if ((n >= 2336) && !memcmp(p, p + 4, 4))
	{
		fmt = 2336;
		sub_off = 0;
		data_off = 8;
	}
	else
	{
		fmt = 2048;
		data_off = 0;
	}
	total = (size - file_base) / fmt;
	ram_mode = !strcmp(mode, "ram") || (strcmp(mode, "stream") && (total * fmt <= RING_BYTES));
	ring_cap = ram_mode ? RING_BYTES / fmt : (RING_BYTES / fmt) / CHUNK * CHUNK;
	if (ram_mode && (total > ring_cap))
		total = ring_cap;		// (forced RAM mode: the start of the movie)
	printf("VIDTEST open %s %u %u %s\n", name, (unsigned)total, (unsigned)fmt, ram_mode ? "ram" : "stream");
	PClseek(fd, file_base, PCDRV_SEEK_SET);
	loaded = 0;
	frames_total = 0;
	if (ram_mode)
	{
		Status("LOADING");
		while (loaded < total)
		{
			uint32_t	k = (total - loaded < 32) ? total - loaded : 32;

			if (PCread(fd, (char *)(ring + loaded * fmt), k * fmt) != (int)(k * fmt))
				break;
			loaded += k;
		}
		total = loaded;
	}
	Rewind();
	if (!ram_mode)
		Load(75 * speed / CHUNK);		// (1 second of sectors before it starts)
	playing = 1;
	paused = 0;
	last_vsync = VSync(-1);
	Status("");
}

/****************************************/
// The clock: the sound's when it plays, else the vertical blanks; sectors processed LEAD_US ahead of it

static void	PlayService(void)
{
	uint32_t	now = VSync(-1), target;

	if (!playing)
		return;
	if (paused && !step_req)
	{
		last_vsync = now;
		return;
	}
	// Time moves on with the vertical blanks (not while the sectors aren't in: stream mode too slow), pulled towards
	// the sound's position while it plays (no drift between the picture and the sound; silence: no pull)
	// The start: the first frame decoded (or no pictures in the first second of the movie) before the clock moves
	if (!primed && ((ReadySlot() >= 0) || ((cur_v >= total) || (SectorUs(cur_v) > 1000000 && frames_total == 0 &&
		wait_buf < 0 && asm_buf < 0))))
		primed = 1;
	if (!stalled && !paused && primed)
	{
		play_us += (now - last_vsync) * VsyncUs();
		if (sound_on && (clock_base >= 0))
		{
			uint32_t	n = SoundClock();
			uint32_t	us = (n / rate) * 1000000 + ((n % rate) * 1000 / rate) * 1000;
			int32_t		err = (int32_t)(sound_base_us + us - play_us);

			play_us += err / 8;
		}
	}
	last_vsync = now;
	// The sound's sectors LEAD_US ahead of the clock (its FIFO), the pictures' as far as the decoder has room (a
	// frame waiting: the next one isn't started), each cursor on its own: the sectors before both are free (stream)
	target = play_us + LEAD_US;
	stalled = 0;
	while (!paused && (cur_a < total) && (SectorUs(cur_a) <= target))
	{
		uint8_t	*p;

		if (cur_a >= loaded)
		{
			if (!stalled)
				stalls++;
			stalled = 1;
			break;
		}
		p = Sector(cur_a++);
		if ((sub_off != 0xFFFFFFFF) && (p[sub_off + 2] & 0x04))
		{
			if (coding == 0xFF)		// (the format from the first sound sector: its sectors are kept from there)
			{
				coding = p[sub_off + 3];
				rate = (coding & 4) ? 18900 : 37800;
				stereo = coding & 1;
			}
			AudioSector(p + data_off);
		}
	}
	if (step_req && (ReadySlot() < 0) && (decode_state == DECODE_IDLE) && (wait_buf < 0))
		target = 0xFFFFFFFF;			// (paused: the sectors of the next frame)
	while ((cur_v < total) && (SectorUs(cur_v) <= target))
	{
		uint8_t	*p, *d;

		if (cur_v >= loaded)
		{
			if (!stalled && !step_req)
				stalls++;
			stalled = !step_req;
			break;
		}
		p = Sector(cur_v);
		d = p + data_off;
		if (!((sub_off != 0xFFFFFFFF) && (p[sub_off + 2] & 0x04)) &&
			(d[0] == 0x60) && (d[1] == 0x01) && (d[2] == 0x01) && (d[3] == 0x80))
		{
			int	number = d[8] | (d[9] << 8) | (d[10] << 16) | (d[11] << 24);

			if ((wait_buf >= 0) && ((asm_buf < 0) || (number != asm_number)))
				break;			// (no room for the next frame yet)
			VideoSector(cur_v, d);
		}
		cur_v++;
		if (step_req && (wait_buf >= 0))
			break;
	}
	consumed = (cur_a < cur_v) ? cur_a : cur_v;
	// The sound starts once it has its lead (the sectors of its first LEAD_US read)
	if ((coding != 0xFF) && !sound_on && !paused && primed)
	{
		rate = (coding & 4) ? 18900 : 37800;
		stereo = coding & 1;
		if (FifoBlocks(0) >= HALF_BLOCKS * 2)
			StartSound();
	}
	// The end: the sectors processed, the frames shown, the sound played
	if ((cur_a >= total) && (cur_v >= total) && (wait_buf < 0) && (decode_state == DECODE_IDLE) && (ReadySlot() < 0) &&
		(!sound_on || (FifoBlocks(0) < HALF_BLOCKS)) && (play_us >= SectorUs(total)))
	{
		if (!ended)
			printf("VIDTEST end\n");
		ended = 1;
		if (loop)
			Rewind();
		else
			StopSound();
	}
}

/****************************************/
// Commands (@input) and controller

// The next word of the line (spaces skipped), NULL at the end (PSn00bSDK's strtok returned the whole line)
static char	*Word(char **line)
{
	char	*p = *line, *w;

	while ((*p == ' ') || (*p == '\t') || (*p == '\r'))
		p++;
	if (!*p)
		return (NULL);
	if (*p == '"')		// "a name with spaces"
	{
		w = ++p;
		while (*p && (*p != '"'))
			p++;
	}
	else
	{
		w = p;
		while (*p && (*p != ' ') && (*p != '\t') && (*p != '\r'))
			p++;
	}
	if (*p)
		*p++ = 0;
	*line = p;
	return (w);
}

static void	Command(char *line)
{
	char	*cmd = Word(&line), *arg, file[64] = "", mode[16] = "auto";

	if (!cmd)
		return;
	if (!strcmp(cmd, "open") && (arg = Word(&line)))
	{
		snprintf(file, sizeof(file), "%s", arg);
		while ((arg = Word(&line)))
		{
			char	*val = Word(&line);

			if (!val)
				break;
			if (!strcmp(arg, "speed"))
				speed = (val[0] == '1') ? 1 : 2;
			else if (!strcmp(arg, "mode"))
				snprintf(mode, sizeof(mode), "%s", val);
			else if (!strcmp(arg, "loop"))
				loop = (val[0] == '1');
		}
		OpenMovie(file, mode);
	}
	else if (!strcmp(cmd, "play"))		{ paused = 0; SetVolume(); }
	else if (!strcmp(cmd, "pause"))		paused = 1;
	else if (!strcmp(cmd, "toggle"))	paused = !paused;
	else if (!strcmp(cmd, "restart"))	{ Rewind(); paused = 0; }
	else if (!strcmp(cmd, "step"))		{ paused = 1; step_req = 1; }
	else if (!strcmp(cmd, "loop") && (arg = Word(&line)))		loop = (arg[0] == '1');
	else if (!strcmp(cmd, "depth") && (arg = Word(&line)))	{ depth24 = (arg[0] == '2'); ResetSlots(); }
	else if (!strcmp(cmd, "scale") && (arg = Word(&line)))
		scale_mode = (arg[0] == '1') ? 0 : ((arg[1] == 'i') ? 1 : 2);
	else if (!strcmp(cmd, "info") && (arg = Word(&line)))		show_info = (arg[0] == '1');
	else if (!strcmp(cmd, "volume") && (arg = Word(&line)))
	{
		volume = 0;
		while ((*arg >= '0') && (*arg <= '9'))
			volume = volume * 10 + *arg++ - '0';
		if (volume > 100)
			volume = 100;
		SetVolume();
	}
	else if (!strcmp(cmd, "close"))		CloseMovie();
	else if (!strcmp(cmd, "quit"))		{ CloseMovie(); Status("STOPPED"); }
	else
		printf("VIDTEST error unknown command: %s\n", cmd);
}

static void	InputService(void)
{
	static char	line[160];
	static int	n;
	char		buf[160];
	int			got, i;

	if (input_fd < 0)
		return;
	got = PCread(input_fd, buf, sizeof(buf));
	if (got < 0)	// (the file server has gone: not asked again, the USB link stays free)
	{
		input_fd = -1;
		server_lost = 1;
		return;
	}
	for (i = 0; i < got; i++)
	{
		if ((buf[i] == '\n') || (n == sizeof(line) - 1))
		{
			line[n] = 0;
			n = 0;
			Command(line);
		}
		else
			line[n++] = buf[i];
	}
}

/****************************************/
// Screen

static void	DrawPicture(int side)
{
	int	u, part = 0, sw = vid_w, sh = vid_h, dw = vid_w, dh = vid_h, x0, y0;

	if (sw > 320 && scale_mode == 0)
		dw = sw / 2;		// (wider than the screen: half width)
	if (scale_mode == 1)	// Fit: as big as the screen allows, the same shape
	{
		dw = SCREEN_W;
		dh = sh * SCREEN_W / sw;
		if (dh > SCREEN_H)
		{
			dh = SCREEN_H;
			dw = sw * SCREEN_H / sh;
		}
	}
	else if (scale_mode == 2)
	{
		dw = SCREEN_W;
		dh = SCREEN_H;
	}
	x0 = (SCREEN_W - dw) / 2;
	y0 = (SCREEN_H - dh) / 2;
	for (u = 0; (u < sw) && (part < 3); u += 256, part++)
	{
		POLY_FT4	*p = &poly[side][part];
		int			w = (sw - u > 256) ? 256 : sw - u;

		setPolyFT4(p);
		setRGB0(p, 128, 128, 128);
		setXYWH(p, x0 + u * dw / sw, y0, w * dw / sw, dh);
		setUVWH(p, 0, 0, w - 1, sh - 1);
		p->tpage = getTPage(2, 0, SlotX(tex_shown) + u, SlotY(tex_shown));
		addPrim(&ot[side][1], p);
	}
}

static void	Draw(int side)
{
	if (depth24)
	{
		// 24 bits: the decoded frame is the screen (no drawing); shown once the next one is in the other one
		if (tex_shown >= 0)
		{
			DISPENV	d;

			SetDefDispEnv(&d, 0, SlotY(tex_shown), SCREEN_W, SCREEN_H);
			d.isrgb24 = 1;
			if (is_pal)
				d.screen.y = 24;
			PutDispEnv(&d);
		}
		return;
	}
	ClearOTagR(ot[side], 2);
	if (tex_shown >= 0)
	{
		DrawPicture(side);
		// The text of the debug font has no texture page of its own: the font's set again after the picture
		setDrawTPage(&font_page[side], 0, 1, getTPage(0, 0, 960, 256));
		addPrim(&ot[side][0], &font_page[side]);
	}
	if (show_info || !playing)
	{
		char	line[40];
		int		buf = !total ? 0 : (int)((loaded - consumed) * 100 / (ring_cap ? ring_cap : 1));

		UiTitle("PS1 DEV CART VIDEO TESTER", !playing ? (name[0] ? "STOPPED" : "NO MOVIE") :
			(paused ? "PAUSED" : (ended ? "ENDED" : (ram_mode ? "RAM" : "STREAM"))));
		UiTextPanel(8, 152, 304, 5);
		if (name[0])
		{
			snprintf(line, 37, "%s", name);
			UiColorText(16, 158, line, UI_YELLOW);
			// (36 characters a line at most: longer ones wrap, the panel's last line is lost)
			FntPrint(fnt_info, "\n%dX%d FRAME %d/%d %dX%s\n", vid_w, vid_h, last_frame, frames_total, speed,
				loop ? " LOOP" : "");
			FntPrint(fnt_info, "DECODE %dMS %dB QSCALE %d\n", (int)(decode_us / 1000), last_bytes, last_scale);
			if (coding != 0xFF)
				FntPrint(fnt_info, "SOUND %d %s UNDERRUNS %d\n", (int)((coding & 4) ? 18900 : 37800),
					(coding & 1) ? "STEREO" : "MONO", underruns);
			else
				FntPrint(fnt_info, "NO SOUND\n");
			FntPrint(fnt_info, "LATE %d DROP %d STALL %d %s %d%%", frames_late, frames_dropped, (int)stalls,
				ram_mode ? "RAM" : "BUF", ram_mode ? 100 : buf);
		}
		else
			FntPrint(fnt_info, "%s\nOPEN A MOVIE FROM THE PS1 DEV CART\nMANAGER (MOVIE TOOLS), OR TYPE\nOPEN NAME.STR INTO PS1TRANSFER",
				status_text);
		UiHelp("START:PLAY O:LOOP X:RESTART SQR:SCALE\nTRI:15/24 BIT R1:STEP SELECT:INFO");
	}
	DrawSync(0);
	PutDispEnv(&disp[side]);
	PutDrawEnv(&draw[side]);
	DrawOTag(&ot[side][1]);
	if (show_info || !playing)
	{
		UiFlush();
		FntFlush(fnt_info);
	}
}

int	main(void)
{
	uint32_t	pad, opad = 0, frame = 0, report = 0;
	int			i, side = 0;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	InitGeom();
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, SCREEN_Y(i ? 0 : 1), SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, SCREEN_Y(i), SCREEN_W, SCREEN_H);
		if (is_pal)
			disp[i].screen.y = 24;
		draw[i].isbg = 1;
		draw[i].tpage = getTPage(0, 0, 960, 256);	// (the debug font's: its text has no texture page)
	}
	FntLoad(960, 256);
	UiInit(draw);
	for (i = 0; i < 2; i++)
		setRGB0(&draw[i], 0, 0, 0);
	fnt_info = UiOpenText(8, 152, 304, 5);
	SetDispMask(1);

	MdecInit();
	DecDCTvlcCopyTableV3((VLC_TableV3 *)0x1F800000);	// The VLC table in the scratchpad (v1, v2 and v3 frames)
	HBLANK_MODE = 0x0100;								// Root counter 1: lines

	SpuInit();
	SPU_REG(0x1F801D80) = 0x3FFF;
	SPU_REG(0x1F801D82) = 0x3FFF;
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	VT_IRQ_MASK &= ~(1 << 9);

	if (PCinit() < 0)
		Status("NO FILE SERVER (PS1TRANSFER -C)");
	input_fd = PCopen("@input", PCDRV_READ, 0);
	printf("VIDTEST ready\n");

	while (1)
	{
		Load(playing && !paused ? 1 : 4);
		SoundService();
		PlayService();
		DecodeService();
		VideoService();
		if (VSync(-1) == frame)
			continue;
		frame = VSync(-1);

		if ((frame & 7) == 0)
			InputService();
		pad = PadRead(1);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		if (PRESSED(PADstart))	{ paused = !paused; if (!paused) SetVolume(); }
		if (PRESSED(PADRdown))	{ Rewind(); paused = 0; }
		if (PRESSED(PADRright))	loop = !loop;
		if (PRESSED(PADRleft))	scale_mode = (scale_mode + 1) % 3;
		if (PRESSED(PADRup))	{ depth24 = !depth24; ResetSlots(); }
		if (PRESSED(PADselect))	show_info = !show_info;
		if (PRESSED(PADR1))		{ paused = 1; step_req = 1; }
		if ((pad & PADLup) && (volume < 100))	{ volume++; SetVolume(); }
		if ((pad & PADLdown) && (volume > 0))	{ volume--; SetVolume(); }
		opad = pad;
		if (paused && sound_on)
			SPU_KEY_OFF = 3, sound_on = 0, clock_base = -1;

		if (playing && (frame - report >= 30))
		{
			report = frame;
			printf("VIDTEST info %d %d %d %d\n", vid_w, vid_h, (coding == 0xFF) ? 0 : ((coding & 4) ? 18900 : 37800),
				(coding == 0xFF) ? 0 : (coding & 1));
			printf("VIDTEST sound %d %d %d\n", underruns, restarts, (int)FifoBlocks(0));
			printf("VIDTEST pos %d %d %u %d %d %u %d %d %d\n", last_frame, frames_total, (unsigned)decode_us,
				last_bytes, last_scale, (unsigned)stalls, frames_late, frames_dropped, (int)(ram_mode ? 100 :
				(loaded - consumed) * 100 / (ring_cap ? ring_cap : 1)));
		}
		side ^= 1;
		Draw(side);
	}

	return (0);
}
