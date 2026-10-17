/*
* PS1 DEV CART video streamer: plays a video streamed by the computer (MDEC video, SPU ADPCM sound).
*
* The computer runs video_server.py (it decodes a video file or stream, encodes the frames to BS v2 MDEC bitstreams and
* the sound to SPU ADPCM) and "ps1transfer -net 127.0.0.1:7778", the bridge to this program's psxcomb USB link:
*	python video_server.py
*	ps1transfer -r -e -f videostreamer.ps-exe -net 127.0.0.1:7778
* Protocol: videostreamer.h. The sound plays like the audio streamer's (FIFO in RAM, ring of two halves in SPU RAM) and is the
* clock: the stream position of the half being played, plus the time since it started. The frames wait in a ring in
* RAM until their time; late ones are dropped. A frame is decompressed with DecDCTvlc (psxpress), decoded by the MDEC
* in 16-pixel strips into one of two textures in VRAM (the other one is on the screen), and drawn 2x to the screen.
* Controls: Left/Right video, Up/Down volume, Select information.
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
#include "psxcomb/psxcomb.h"
#include "videostreamer.h"

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	CHUNK			1024			// Bytes per psxcomb read

// Sound FIFO in RAM (ADPCM bytes, power of 2) and SPU ring (two halves of HALF_BLOCKS blocks)
#define	AUDIO_FIFO		65536			// (about 5 s at 22050 Hz: more than the server's lead + the start buffer)
#define	START_SECONDS	1
#define	SPU_RING_ADRS	0x2000
#define	HALF_BLOCKS		64
#define	HALF_BYTES		(HALF_BLOCKS * VS_BLOCK)
#define	HALF_SAMPLES	(HALF_BLOCKS * VS_BLOCK_SAMPLES)
#define	VOICE			0

// Frames waiting for their time: [length][frame number][BS data] entries, 4-byte aligned, WRAP to go back to the start
#define	VIDEO_RING		(384 * 1024)
#define	WRAP			0xFFFFFFFF

// Textures: two frame buffers in VRAM (one decoded while the other one is drawn)
#define	TEX_X			640
#define	TEX_Y(_n_)		((_n_) * 256)
#define	RLE_WORDS		32768			// MDEC run-length codes of a frame (2 per word)

#define	SPU_VOICE(_v_)	((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))	// volL, volR, pitch, start, adsr1, adsr2, -, repeat
#define	SPU_REG(_a_)	(*((volatile uint16_t *)(_a_)))
#define	SPU_KEY_ON		SPU_REG(0x1F801D88)
#define	SPU_KEY_OFF		SPU_REG(0x1F801D8C)
#define	SPU_IRQ_ADRS	SPU_REG(0x1F801DA4)
#define	VS_SPU_CNT		SPU_REG(0x1F801DAA)
#define	VS_SPU_STAT		SPU_REG(0x1F801DAE)
#define	SPU_IRQ_ENABLE	(1 << 6)
#define	VS_IRQ_MASK		(*((volatile uint32_t *)0x1F801074))
// (MDEC1: the MDEC control/status register 0x1F801824, from PSn00bSDK's hwregs_c.h)

static DISPENV		disp[2];
static DRAWENV		draw[2];
static POLY_FT4		poly[2][2];					// The frame: up to 2 parts of 256 pixels (texture pages)
static uint32_t		ot[2][2];
static int			fnt_info;
static int			is_pal, show_info = 1;

// Link
static long			comb_read = -1, comb_write = -1, ev_read, ev_write;
static uint8_t		rx[CHUNK], tx_cmd;
static int			read_pending, write_busy;
static uint32_t		bytes_second, rate_bytes;

// Packets
static uint8_t		hdr[VS_HEADER];
static int			hdr_n, type, left;
static uint8_t		text_buf[VS_MAX_TEXT + 1];
static int			text_n;
static uint8_t		*frame_dst;					// Where the frame packet being received goes (NULL: dropped)
static uint32_t		frame_need;					// Its size in the ring

// Video
static int			can_w, can_h, vis_w, vis_h, enc_w, enc_h, fps100, item, items;	// (vis: the picture)
static char			name[VS_MAX_TEXT + 1], info_text[VS_MAX_TEXT + 1], error[VS_MAX_TEXT + 1];
static uint8_t		video_ring[VIDEO_RING] __attribute__((aligned(4)));
static uint32_t		vhead, vtail, vused;		// Byte offsets, bytes used
static uint32_t		rle[RLE_WORDS + 64];
static uint32_t		frame_buf[320 * 256 / 2];	// MDEC output of a whole frame (16-pixel strips one after the other)
static int			tex_shown = -1, frames_shown, frames_dropped;
enum	{DECODE_IDLE, DECODE_MDEC, DECODE_UPLOAD};
static int			decode_state, decode_tex, decode_discard;
static int			ready;						// decode_tex holds the next frame, shown at its time
static uint32_t		decode_vsync, decode_number;

// Sound
static uint32_t		sample_rate;
static int			channels;
static uint8_t		fifo[AUDIO_FIFO];
static uint32_t		fifo_head, fifo_tail;		// Free-running
static uint8_t		half_buf[HALF_BYTES];
static int			playing, buffering = 1, irq_half, volume = 100, underruns;
static int			restarts, mdec_resets;		// (statistics: sound watchdog, MDEC still busy before a frame)
static int32_t		half_pos[2];				// Stream sample of the start of each half (-1: silence)
static int32_t		consumed;					// Stream samples moved to the SPU
static int32_t		clock_base = -1;			// Stream sample of the start of the half being played
static uint32_t		clock_vsync, last_irq_vsync;
static int32_t		clock_now;

/****************************************/
// Video ring

static void	ResetVideo(void)
{
	vhead = vtail = vused = 0;
	frame_dst = NULL;
	tex_shown = -1;
	ready = 0;
	decode_discard = (decode_state != DECODE_IDLE);	// (a frame of the previous video being decoded)
}

// Room for a frame packet of size n (frame number + BS): its place in the ring, NULL if full
static uint8_t	*VideoReserve(uint32_t n)
{
	uint32_t	need = 4 + ((n + 3) & ~3);

	if (vhead == VIDEO_RING)			// Ended exactly at the end
		vhead = 0;
	if (vhead + need > VIDEO_RING)		// Not enough room before the end: back to the start
	{
		if (vused + (VIDEO_RING - vhead) + need > VIDEO_RING)
			return (NULL);
		*(uint32_t *)(video_ring + vhead) = WRAP;
		vused += VIDEO_RING - vhead;
		vhead = 0;
	}
	if (vused + need > VIDEO_RING)
		return (NULL);
	frame_need = need;
	*(uint32_t *)(video_ring + vhead) = n;
	return (video_ring + vhead + 4);
}

// The frame packet is complete: it can be read
static void	VideoCommit(void)
{
	vhead += frame_need;
	vused += frame_need;
}

// Next frame waiting: its number and BS data (NULL if none)
static const uint32_t	*VideoPeek(uint32_t *number, uint32_t *size)
{
	uint32_t	n;

	if (!vused)
		return (NULL);
	if (vtail == VIDEO_RING)
		vtail = 0;
	n = *(uint32_t *)(video_ring + vtail);
	if (n == WRAP)
	{
		vused -= VIDEO_RING - vtail;
		vtail = 0;
		if (!vused)
			return (NULL);
		n = *(uint32_t *)(video_ring + vtail);
	}
	*number = *(uint32_t *)(video_ring + vtail + 4);
	*size = n - 4;
	return ((const uint32_t *)(video_ring + vtail + 8));
}

static void	VideoPop(void)
{
	uint32_t	need = 4 + ((*(uint32_t *)(video_ring + vtail) + 3) & ~3);

	vtail += need;
	vused -= need;
}

/****************************************/
// Packets from the server

static uint16_t	U16(const uint8_t *p)
{
	return (p[0] | (p[1] << 8));
}

static void	PacketDone(void)
{
	text_buf[text_n] = 0;
	switch (type)
	{
		case VS_INFO:
			if (text_n >= 21)
			{
				can_w = U16(text_buf);
				can_h = U16(text_buf + 2);
				vis_w = U16(text_buf + 4);
				vis_h = U16(text_buf + 6);
				enc_w = U16(text_buf + 8);
				enc_h = U16(text_buf + 10);
				fps100 = U16(text_buf + 12);
				sample_rate = U16(text_buf + 14) | (U16(text_buf + 16) << 16);
				channels = text_buf[18];
				item = text_buf[19];
				items = text_buf[20];
				strcpy(name, (char *)text_buf + 21);
				error[0] = 0;
			}
			break;
		case VS_TEXT:
			strcpy(info_text, (char *)text_buf);
			break;
		case VS_ERROR:
			strcpy(error, (char *)text_buf);
			break;
		case VS_FRAME:
			if (frame_dst)
				VideoCommit();
			break;
	}
}

static void	StopSound(void);

static void	Parse(const uint8_t *p, int n)
{
	int	k;

	while (n)
	{
		if (hdr_n < VS_HEADER)
		{
			hdr[hdr_n++] = *p++;
			n--;
			if (hdr_n < VS_HEADER)
				continue;
			type = hdr[0];
			left = hdr[1] | (hdr[2] << 8);
			text_n = 0;
			if (type == VS_RESET)		// New video: what was received before is dropped
			{
				fifo_tail = fifo_head;
				ResetVideo();
				info_text[0] = 0;
				StopSound();
			}
			if (type == VS_FRAME)
				frame_dst = (left > 4) && (left <= VS_MAX_FRAME) ? VideoReserve(left) : NULL;
			if (!left)
			{
				PacketDone();
				hdr_n = 0;
			}
			continue;
		}
		k = (n < left) ? n : left;
		if (type == VS_AUDIO)
		{
			int	i;

			for (i = 0; i < k; i++)
				if (fifo_head - fifo_tail < AUDIO_FIFO)
					fifo[fifo_head++ & (AUDIO_FIFO - 1)] = p[i];
		}
		else if (type == VS_FRAME)
		{
			if (frame_dst)
			{
				memcpy(frame_dst, p, k);
				frame_dst += k;
			}
		}
		else
		{
			int	i;

			for (i = 0; i < k; i++)
				if (text_n < VS_MAX_TEXT)
					text_buf[text_n++] = p[i];
		}
		p += k;
		n -= k;
		left -= k;
		if (!left)
		{
			PacketDone();
			frame_dst = NULL;
			hdr_n = 0;
		}
	}
}

static void	OpenLink(void)
{
	CombSetLink(COMB_LINK_USB);		// (before AddCOMB: the rings are published at once)
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

static void	SendCommand(uint8_t cmd)
{
	if ((comb_write < 0) || write_busy)
		return;
	tx_cmd = cmd;
	write_busy = 1;
	write(comb_write, &tx_cmd, 1);
}

// A read is started only when the sound FIFO and the frame ring have room (else the server waits, nothing dropped)
static void	LinkService(void)
{
	int	i;

	if (comb_read < 0)
		return;
	// All the chunks already arrived (the frame loop also decodes and draws: one chunk per turn was too slow)
	for (i = 0; i < 16; i++)
	{
		if (read_pending)
		{
			if (TestEvent(ev_read) != 1)
				break;
			read_pending = 0;
			Parse(rx, CHUNK);
			bytes_second += CHUNK;
		}
		if ((AUDIO_FIFO - (fifo_head - fifo_tail) < CHUNK) || (VIDEO_RING - vused < 2 * VS_MAX_FRAME + CHUNK))
			break;
		read_pending = 1;
		read(comb_read, rx, CHUNK);
	}
	if (TestEvent(ev_write) == 1)
		write_busy = 0;
}

/****************************************/
// Sound (as the audio streamer) and clock

static uint32_t	FifoBlocks(void)
{
	return ((fifo_head - fifo_tail) / VS_BLOCK);
}

static void	FillHalf(int half)
{
	int	i, j;

	if (buffering && (FifoBlocks() >= sample_rate * START_SECONDS / VS_BLOCK_SAMPLES))
		buffering = 0;
	half_pos[half] = buffering ? -1 : consumed;
	for (i = 0; i < HALF_BLOCKS; i++)
	{
		uint8_t	*b = half_buf + i * VS_BLOCK;

		if (!buffering && (FifoBlocks() > 0))
		{
			for (j = 0; j < VS_BLOCK; j++)
				b[j] = fifo[fifo_tail++ & (AUDIO_FIFO - 1)];
			consumed += VS_BLOCK_SAMPLES;
		}
		else
		{
			if (!buffering)
			{
				buffering = 1;
				underruns++;
			}
			memset(b, 0, VS_BLOCK);
		}
		b[1] = 0;
	}
	if (half == 1)
		half_buf[(HALF_BLOCKS - 1) * VS_BLOCK + 1] = 1 | 2;	// End + repeat (repeat address written below)
	SpuSetTransferStartAddr(SPU_RING_ADRS + half * HALF_BYTES);
	SpuWrite((const uint32_t *)half_buf, HALF_BYTES);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	SPU_VOICE(VOICE)[7] = SPU_RING_ADRS / 8;
}

static void	SetVolume(void)
{
	SPU_VOICE(VOICE)[0] = SPU_VOICE(VOICE)[1] = volume * 0x3FFF / 100;
}

static void	StopSound(void)
{
	SPU_KEY_OFF = 1 << VOICE;
	playing = 0;
	buffering = 1;
	consumed = 0;
	clock_base = -1;
	clock_now = 0;
}

static void	StartSound(void)
{
	volatile uint16_t	*v = SPU_VOICE(VOICE);

	SPU_KEY_OFF = 1 << VOICE;
	FillHalf(0);
	FillHalf(1);
	SetVolume();
	v[2] = sample_rate * 4096 / 44100;
	v[3] = SPU_RING_ADRS / 8;
	v[4] = 0x000F;
	v[5] = 0x0000;
	irq_half = 1;
	VS_SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (SPU_RING_ADRS + HALF_BYTES) / 8;
	VS_SPU_CNT |= SPU_IRQ_ENABLE;
	SPU_KEY_ON = 1 << VOICE;
	v[7] = SPU_RING_ADRS / 8;
	clock_base = half_pos[0];
	clock_vsync = last_irq_vsync = VSync(-1);
	playing = 1;
}

static void	SoundService(void)
{
	if (!playing)
	{
		// Starts once a second is buffered (the clock waits meanwhile)
		if (sample_rate && (FifoBlocks() >= sample_rate * START_SECONDS / VS_BLOCK_SAMPLES))
			StartSound();
		return;
	}
	if (!(VS_SPU_STAT & SPU_IRQ_ENABLE))
	{
		if (VSync(-1) - last_irq_vsync > 30)		// Watchdog (see audiostreamer.c)
		{
			restarts++;
			StartSound();
		}
		return;
	}
	last_irq_vsync = VSync(-1);
	// The SPU started the half at the interrupt address: the clock follows it, the other half is filled
	clock_base = half_pos[irq_half];
	clock_vsync = last_irq_vsync;
	FillHalf(irq_half ^ 1);
	irq_half ^= 1;
	VS_SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (SPU_RING_ADRS + irq_half * HALF_BYTES) / 8;
	VS_SPU_CNT |= SPU_IRQ_ENABLE;
}

// Stream sample being played (does not move during silence)
static int32_t	Clock(void)
{
	int32_t	t;

	if (clock_base < 0)
		return (clock_now);
	t = clock_base + (int32_t)((VSync(-1) - clock_vsync) * sample_rate / (is_pal ? 50 : 60));
	if (t > clock_base + HALF_SAMPLES)
		t = clock_base + HALF_SAMPLES;
	if (t > clock_now)
		clock_now = t;
	return (clock_now);
}

/****************************************/
// Frames

// MDEC reset: the DMA request enable that DecDCTReset writes right after the reset was lost on the consoles tested (the
// first transfer then waited forever): written again after a short delay, then the tables are loaded
static void	MdecInit(void)
{
	int	i;

	DecDCTReset(1);
	for (i = 0; i < 1000; i++)
		__asm__ volatile ("nop");
	MDEC1 = 0x60000000;
	DecDCTPutEnv(NULL, 0);
}

// A frame is decoded in the background: decompressed by the CPU (DecDCTvlc), then the MDEC decodes the whole frame into
// frame_buf while the main loop goes on (link, sound, screen: waiting for the MDEC strip after strip took about 28ms
// per 320x240 frame), then its 16-pixel strips are uploaded to the texture, shown once the GPU has them.

static void	StartDecode(const uint32_t *bs, int tex)
{
	VLC_Context	ctx;
	uint32_t	n, padded, i;

	if (DecDCTvlcStart(&ctx, rle, RLE_WORDS, bs) != 0)
		return;
	// Still busy (not expected since the encoder makes whole 32-word chunks; DecDCTin would wait about a second): reset
	if (MDEC1 & (1 << 29))
	{
		mdec_resets++;
		MdecInit();
	}
	// The MDEC is fed by 32-word chunks: the words after the codes, up to the end of the last chunk, are zero (MDEC
	// command 0: no function). The count stays exact: padding with end of block codes made the MDEC decode extra
	// blocks that nothing read, and the next frame's DecDCTin waited about a second for it.
	n = rle[0] & 0xFFFF;
	padded = (n + 31) & ~31;
	for (i = n; i < padded; i++)
		rle[1 + i] = 0;
	DecDCTin(rle, DECDCT_MODE_16BPP_BIT15);
	DecDCTout(frame_buf, enc_w * enc_h / 2);
	decode_state = DECODE_MDEC;
	decode_tex = tex;
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
			if (VSync(-1) - decode_vsync > 30)		// (not expected: the MDEC stopped)
			{
				MdecInit();
				decode_state = DECODE_IDLE;
			}
			return;
		}
		// The macroblocks come column after column: each 16-pixel strip is contiguous in frame_buf
		for (x = 0; x < enc_w; x += 16)
		{
			setRECT(&rect, TEX_X + x, TEX_Y(decode_tex), 16, enc_h);
			LoadImage(&rect, frame_buf + x * enc_h / 2);
		}
		decode_state = DECODE_UPLOAD;
	}
	if ((decode_state == DECODE_UPLOAD) && !DrawSync(1))
	{
		ready = !decode_discard;
		decode_discard = 0;
		decode_state = DECODE_IDLE;
	}
}

// Stream sample where frame n starts: n * rate / fps, in 32 bits (whole seconds, then the rest)
static int32_t	FrameTime(uint32_t n)
{
	uint32_t	r = sample_rate * 100;

	return ((int32_t)((n / fps100) * r + (n % fps100) * (r / 100) * 100 / fps100));
}

// Decoded ahead: the next frame is decoded into the texture not shown as soon as the decoder is free (the frames arrive
// about 2 s early), and shown when its time comes; frames already late when they would be decoded are dropped
static void	VideoService(void)
{
	uint32_t		number, size, next_number, next_size;
	const uint32_t	*bs;
	int32_t			now = Clock();

	if (!playing || !fps100)
		return;
	if (ready && (FrameTime(decode_number) <= now))
	{
		tex_shown = decode_tex;
		frames_shown++;
		ready = 0;
	}
	if ((decode_state != DECODE_IDLE) || ready)
		return;
	while ((bs = VideoPeek(&number, &size)))
	{
		VideoPop();
		// A later frame is already due: this one is dropped
		if (VideoPeek(&next_number, &next_size) && (FrameTime(next_number) <= now))
		{
			frames_dropped++;
			continue;
		}
		decode_number = number;
		StartDecode(bs, (tex_shown == 0) ? 1 : 0);
		return;
	}
}

/****************************************/
// Screen

static void	Draw(int side)
{
	uint32_t	tenths = sample_rate ? FifoBlocks() * VS_BLOCK_SAMPLES * 10 / sample_rate : 0;

	ClearOTagR(ot[side], 2);
	if (tex_shown >= 0)
	{
		// The canvas (e.g. 320x240) on the whole screen (1:1 at 320x240, 2x at 160x120), the picture (vis_w x vis_h)
		// centered in it (black bars around), in parts of 256 pixels: a texture page is 256 pixels wide
		int	u, part = 0, x0 = (can_w - vis_w) / 2, y0 = (can_h - vis_h) / 2;

		for (u = 0; (u < vis_w) && (part < 2); u += 256, part++)
		{
			POLY_FT4	*p = &poly[side][part];
			int			w = (vis_w - u > 256) ? 256 : vis_w - u;

			setPolyFT4(p);
			setRGB0(p, 128, 128, 128);
			setXYWH(p, (x0 + u) * SCREEN_W / can_w, y0 * SCREEN_H / can_h, w * SCREEN_W / can_w, vis_h * SCREEN_H / can_h);
			setUVWH(p, 0, 0, w - 1, vis_h - 1);
			p->tpage = getTPage(2, 0, TEX_X + u, TEX_Y(tex_shown));
			addPrim(&ot[side][1], p);
		}
	}
	if (show_info || !sample_rate)
	{
		const char	*state = !sample_rate ? "OFFLINE" : (buffering ? "BUFFERING" : "PLAYING");
		char		line[40];

		UiTitle("PS1 DEV CART VIDEO STREAMER", state);
		UiTextPanel(8, 166, 304, 4);
		if (sample_rate)
		{
			snprintf(line, 37, "%d/%d %s", item + 1, items, name);
			UiColorText(16, 172, line, UI_YELLOW);
			FntPrint(fnt_info, "\n%.36s\n%dX%d  %d FPS  SOUND %d HZ\nBUFFER %d.%d S  %d KB/S  DROPPED %d",
				error[0] ? error : info_text, vis_w, vis_h, fps100 / 100, (int)sample_rate, (int)(tenths / 10),
				(int)(tenths % 10), (int)((rate_bytes + 512) / 1024), frames_dropped);
		}
		else
			FntPrint(fnt_info, "WAITING FOR THE VIDEO SERVER\nPS1TRANSFER -NET 127.0.0.1:7778");
		UiHelp("</>:VIDEO  UP/DOWN:VOLUME  SELECT:INFO");
	}

	DrawSync(0);
	PutDispEnv(&disp[side]);
	PutDrawEnv(&draw[side]);
	DrawOTag(&ot[side][1]);
	if (show_info || !sample_rate)
	{
		UiFlush();
		FntFlush(fnt_info);
	}
}

/****************************************/

int	main(void)
{
	uint32_t	pad, opad = 0, frame = 0, second_frame = 0;
	int			i, side = 0;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	InitGeom();		// (the VLC decompressor uses the GTE)
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	for (i = 0; i < 2; i++)
		setRGB0(&draw[i], 0, 0, 0);		// Black around the picture
	fnt_info = UiOpenText(8, 166, 304, 4);
	SetDispMask(1);

	MdecInit();
	DecDCTvlcCopyTableV2((VLC_TableV2 *)0x1F800000);	// Decompression table in the scratchpad (faster)

	SpuInit();
	SPU_REG(0x1F801D80) = 0x3FFF;
	SPU_REG(0x1F801D82) = 0x3FFF;
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	VS_IRQ_MASK &= ~(1 << 9);

	OpenLink();

	while (1)
	{
		LinkService();
		SoundService();
		DecodeService();
		VideoService();
		if (VSync(-1) == frame)
			continue;
		frame = VSync(-1);

		pad = PadRead(1);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		if (PRESSED(PADLright))					SendCommand(VS_NEXT);
		if (PRESSED(PADLleft))					SendCommand(VS_PREVIOUS);
		if (PRESSED(PADselect))					show_info = !show_info;
		if ((pad & PADLup) && (volume < 100))	{volume++; SetVolume();}
		if ((pad & PADLdown) && (volume > 0))	{volume--; SetVolume();}
		opad = pad;

		if (frame - second_frame >= (is_pal ? 50 : 60))
		{
			rate_bytes = bytes_second;
			bytes_second = 0;
			second_frame = frame;
		}
		side ^= 1;
		Draw(side);
	}

	return (0);
}
