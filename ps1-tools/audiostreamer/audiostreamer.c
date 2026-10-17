/*
* PS1 DEV CART audio streamer: plays internet radio stations and audio files streamed by the computer.
*
* The computer runs audio_server.py (it fetches a station or reads a file, decodes it and encodes it to SPU ADPCM) and
* "ps1transfer -net 127.0.0.1:7777", the bridge between the server and this program's psxcomb USB link (or the PS1 Dev
* Cart Manager's Audio streamer page does both):
*	python audio_server.py [stations or files]
*	ps1transfer -r -e -f audiostreamer.ps-exe -net 127.0.0.1:7777
* Protocol: audiostreamer.h. The audio blocks go to a FIFO in RAM, then to a ring of two halves in SPU RAM played by one
* voice in a loop (end + repeat flags on the last block, the repeat address written to the voice). The SPU interrupt
* flag (address at the start of a half, polled) says when the SPU starts a half: the other one is filled again. Playing
* starts once a second of audio is buffered; when the FIFO runs dry, silence is played until it is filled again.
* Controls: Left/Right previous/next in the playlist, Up/Down volume.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxspu.h>
#include "psyq_compat.h"
#include "devcart_ui.h"
#include "psxcomb/psxcomb.h"
#include "audiostreamer.h"

#define	SCREEN_W		320
#define	SCREEN_H		240

// Link: chunks read from psxcomb (a read ends once all its bytes have arrived)
#define	CHUNK			512

// Audio FIFO in RAM (ADPCM bytes), power of 2: about 5 s at 22050 Hz mono
#define	FIFO_SIZE		65536
#define	START_SECONDS	1			// Buffered before playing

// SPU ring: two halves of HALF_BLOCKS blocks (about 0.16 s each at 22050 Hz)
#define	SPU_RING_ADRS	0x2000
#define	HALF_BLOCKS		128
#define	HALF_BYTES		(HALF_BLOCKS * AS_BLOCK)
#define	VOICE			0

#define	SPU_VOICE(_v_)	((volatile uint16_t *)(0x1F801C00 + (_v_) * 16))	// volL, volR, pitch, start, adsr1, adsr2
#define	SPU_REG(_a_)	(*((volatile uint16_t *)(_a_)))
#define	SPU_KEY_ON		SPU_REG(0x1F801D88)
#define	SPU_KEY_OFF		SPU_REG(0x1F801D8C)
#define	SPU_IRQ_ADRS	SPU_REG(0x1F801DA4)		// SPU RAM address / 8
#define	AS_SPU_CNT			SPU_REG(0x1F801DAA)
#define	AS_SPU_STAT		SPU_REG(0x1F801DAE)
#define	SPU_IRQ_ENABLE	(1 << 6)				// AS_SPU_CNT; AS_SPU_STAT bit 6: the address was reached
#define	AS_IRQ_MASK		(*((volatile uint32_t *)0x1F801074))

static DISPENV		disp[2];
static DRAWENV		draw[2];
static int			fnt_station, fnt_title, fnt_buffer, fnt_status;

// Link
static long			comb_read = -1, comb_write = -1, ev_read, ev_write;
static uint8_t		rx[CHUNK], tx_cmd;
static int			write_busy, read_pending;
static uint32_t		bytes_second, rate_bytes;	// Received this second, last second

// Packets
static uint8_t		hdr[AS_HEADER];
static int			hdr_n, type, left;
static char			text[AS_MAX_TEXT + 1];
static int			text_n;

// Station
static uint32_t		sample_rate;
static int			channels, station, stations;
static char			station_name[AS_MAX_TEXT + 1], title[AS_MAX_TEXT + 1], error[AS_MAX_TEXT + 1];

// Audio
static uint8_t		fifo[FIFO_SIZE];
static uint32_t		fifo_head, fifo_tail;		// Free-running
static uint8_t		half_buf[HALF_BYTES];
static int			playing, buffering = 1, irq_half, volume = 100, underruns, restarts;
static uint32_t		last_irq_frame;				// Frame of the last SPU interrupt flag (watchdog)

/****************************************/
// Packets from the server

static void	TextDone(void)
{
	text[text_n] = 0;
	switch (type)
	{
		case AS_INFO:
			if (text_n >= 7)
			{
				const uint8_t	*u = (const uint8_t *)text;

				sample_rate = u[0] | (u[1] << 8) | (u[2] << 16) | ((uint32_t)u[3] << 24);
				channels = text[4];
				station = (uint8_t)text[5];
				stations = (uint8_t)text[6];
				strcpy(station_name, text + 7);
				error[0] = 0;
			}
			break;
		case AS_TITLE:
			strcpy(title, text);
			break;
		case AS_ERROR:
			strcpy(error, text);
			break;
	}
}

static void	Parse(const uint8_t *p, int n)
{
	while (n)
	{
		if (hdr_n < AS_HEADER)
		{
			hdr[hdr_n++] = *p++;
			n--;
			if (hdr_n < AS_HEADER)
				continue;
			type = hdr[0];
			left = hdr[1] | (hdr[2] << 8);
			text_n = 0;
			if (type == AS_RESET)		// New station or file: the audio of the previous one is dropped
			{
				fifo_tail = fifo_head;
				title[0] = 0;
				buffering = 1;
			}
			if (!left)
			{
				TextDone();
				hdr_n = 0;
			}
			continue;
		}
		// Payload
		if (type == AS_AUDIO)
		{
			while (n && left)
			{
				if (fifo_head - fifo_tail < FIFO_SIZE)		// (full: dropped, the server is ahead)
					fifo[fifo_head++ & (FIFO_SIZE - 1)] = *p;
				p++;
				n--;
				left--;
			}
		}
		else
		{
			while (n && left)
			{
				if (text_n < AS_MAX_TEXT)
					text[text_n++] = *p;
				p++;
				n--;
				left--;
			}
		}
		if (!left)
		{
			if (type != AS_AUDIO)
				TextDone();
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

// A read is started only when the FIFO has room for it: when it is full, the link's ring fills up, the bridge stops
// reading the server, and the server waits (nothing is dropped)
static void	LinkService(void)
{
	if (comb_read < 0)
		return;
	if (read_pending && (TestEvent(ev_read) == 1))
	{
		read_pending = 0;
		Parse(rx, CHUNK);
		bytes_second += CHUNK;
	}
	if (!read_pending && (FIFO_SIZE - (fifo_head - fifo_tail) >= CHUNK))
	{
		read_pending = 1;
		read(comb_read, rx, CHUNK);
	}
	if (TestEvent(ev_write) == 1)
		write_busy = 0;
}

/****************************************/
// SPU

static uint32_t	FifoBlocks(void)
{
	return ((fifo_head - fifo_tail) / AS_BLOCK);
}

// A half of the SPU ring: audio from the FIFO, or silence (block of zeros: filter 0, nothing added)
static void	FillHalf(int half)
{
	int	i, j;

	if (buffering && (FifoBlocks() >= sample_rate * START_SECONDS / AS_BLOCK_SAMPLES))
		buffering = 0;
	for (i = 0; i < HALF_BLOCKS; i++)
	{
		uint8_t	*b = half_buf + i * AS_BLOCK;

		if (!buffering && (FifoBlocks() > 0))
		{
			for (j = 0; j < AS_BLOCK; j++)
				b[j] = fifo[fifo_tail++ & (FIFO_SIZE - 1)];
		}
		else
		{
			if (!buffering)		// Ran dry: silence until a second is buffered again
			{
				buffering = 1;
				underruns++;
			}
			memset(b, 0, AS_BLOCK);
		}
		b[1] = 0;				// Flags: only the loop flag below
	}
	// End + repeat on the last block: back to the repeat address, the start of the ring. The repeat address is written
	// to the voice (no loop start flag in the data: once, during a station change, the SPU took a repeat address 4 blocks
	// into the ring, never read the interrupt address again, and the radio stayed silent with a full FIFO)
	if (half == 1)
		half_buf[(HALF_BLOCKS - 1) * AS_BLOCK + 1] = 1 | 2;
	SpuSetTransferStartAddr(SPU_RING_ADRS + half * HALF_BYTES);
	SpuWrite((const uint32_t *)half_buf, HALF_BYTES);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	SPU_VOICE(VOICE)[7] = SPU_RING_ADRS / 8;
}

static void	SetVolume(void)
{
	SPU_VOICE(VOICE)[0] = SPU_VOICE(VOICE)[1] = volume * 0x3FFF / 100;
}

// (also to restart cleanly: see the watchdog in SpuService)
static void	StartVoice(void)
{
	volatile uint16_t	*v = SPU_VOICE(VOICE);

	SPU_KEY_OFF = 1 << VOICE;
	FillHalf(0);
	FillHalf(1);
	SetVolume();
	v[2] = sample_rate * 4096 / 44100;		// Pitch: 0x1000 = 44100 Hz
	v[3] = SPU_RING_ADRS / 8;
	v[4] = 0x000F;							// ADSR: fastest attack, sustain level max
	v[5] = 0x0000;							// Sustain held
	irq_half = 1;							// Interrupt flag when the SPU starts the second half
	AS_SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (SPU_RING_ADRS + HALF_BYTES) / 8;
	AS_SPU_CNT |= SPU_IRQ_ENABLE;
	SPU_KEY_ON = 1 << VOICE;
	v[7] = SPU_RING_ADRS / 8;				// Repeat address (after the key on, which sets it to the start)
	last_irq_frame = VSync(-1);
	playing = 1;
}

static void	SpuService(void)
{
	if (!playing)
	{
		if (sample_rate)
			StartVoice();
		return;
	}
	if (!(AS_SPU_STAT & SPU_IRQ_ENABLE))
	{
		// Watchdog: a half lasts about 0.16 s, no interrupt for 0.6 s means the SPU missed the interrupt address
		// (e.g. this program was too late once): the voice is started again from the start of the ring
		if (VSync(-1) - last_irq_frame > 30)
		{
			restarts++;
			StartVoice();
		}
		return;
	}
	last_irq_frame = VSync(-1);
	// The SPU started the half at the interrupt address: the other one is filled, the next interrupt is at its start
	// (written before the address is set: a transfer to the interrupt address would set the flag)
	FillHalf(irq_half ^ 1);
	irq_half ^= 1;
	AS_SPU_CNT &= ~SPU_IRQ_ENABLE;
	SPU_IRQ_ADRS = (SPU_RING_ADRS + irq_half * HALF_BYTES) / 8;
	AS_SPU_CNT |= SPU_IRQ_ENABLE;
}

/****************************************/
// Screen (devcart_ui.h)

// Text in yellow (devcart_ui.h UiColorText) on up to "lines" lines of 36 characters from y, cut at spaces
static void	PrintYellow(int y, const char *s, int lines)
{
	char	line[40];
	int		n, cut;

	while (*s && lines--)
	{
		n = strlen(s);
		if (n > 36)
		{
			for (cut = 36; (cut > 20) && (s[cut] != ' '); cut--)
				;
			n = (s[cut] == ' ') ? cut : 36;
		}
		memcpy(line, s, n);
		line[n] = 0;
		UiColorText(16, y, line, UI_YELLOW);
		y += 8;
		s += n;
		while (*s == ' ')
			s++;
	}
}

// SPU RAM used: the reserved area at the start (0x0000-0x0FFF: capture buffers of the CD audio and voices 1/3) and
// the ring of this program; the rest is free
#define	SPU_RAM_SIZE	(512 * 1024)
#define	SPU_RAM_USED	(0x1000 + 2 * HALF_BYTES)

static void	Draw(int side)
{
	uint32_t	blocks = FifoBlocks(), tenths = sample_rate ? blocks * AS_BLOCK_SAMPLES * 10 / sample_rate : 0;
	uint32_t	full = sample_rate ? FIFO_SIZE / AS_BLOCK : 1, spu_tenths = (SPU_RAM_USED * 1000 + SPU_RAM_SIZE / 2) / SPU_RAM_SIZE;
	const char	*state = !sample_rate ? "OFFLINE" : (buffering ? "BUFFERING" : "PLAYING");

	UiTitle("PS1 DEV CART AUDIO STREAMER", state);
	UiTextPanel(8, 20, 304, 2);
	UiTextPanel(8, 54, 304, 4);
	UiTextPanel(8, 106, 304, 4);
	UiTextPanel(8, 172, 304, 2);
	if (sample_rate)
	{
		FntPrint(fnt_station, "PLAYLIST %d/%d", station + 1, stations);
		PrintYellow(26 + 8, station_name, 1);
	}
	else
		FntPrint(fnt_station, "WAITING FOR THE AUDIO SERVER\nPS1TRANSFER -NET 127.0.0.1:7777");
	FntPrint(fnt_title, "NOW PLAYING");
	PrintYellow(60 + 8, title[0] ? title : "-", 3);
	FntPrint(fnt_buffer, "BUFFER   %d.%d S\nSPU RAM  %d.%d%% USED\nAUDIO    %d HZ %s\nVOLUME   %d", (int)(tenths / 10),
		(int)(tenths % 10), (int)(spu_tenths / 10), (int)(spu_tenths % 10), (int)sample_rate,
		(channels == 2) ? "STEREO" : "MONO", volume);
	UiProgress(176, 113, 120, 6, blocks, full);
	UiProgress(176, 121, 120, 6, SPU_RAM_USED, SPU_RAM_SIZE);
	UiProgress(176, 137, 120, 6, volume, 100);
	if (error[0])
		FntPrint(fnt_status, "%s", error);
	else
		FntPrint(fnt_status, "RECEIVED %d KB/S\nUNDERRUNS %d  RESTARTS %d", (int)((rate_bytes + 512) / 1024), underruns,
			restarts);
	UiHelp("LEFT/RIGHT:PREV/NEXT  UP/DOWN:VOLUME");

	// (called at the start of a frame: no VSync wait, the link and the SPU are served between the frames)
	DrawSync(0);
	PutDispEnv(&disp[side]);
	PutDrawEnv(&draw[side]);
	UiFlush();
	FntFlush(fnt_station);
	FntFlush(fnt_title);
	FntFlush(fnt_buffer);
	FntFlush(fnt_status);
}

/****************************************/

int	main(void)
{
	uint32_t	pad, opad = 0, frame = 0, second_frame = 0;
	int			i, side = 0, is_pal;

	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);
	fnt_station = UiOpenText(8, 20, 304, 2);
	fnt_title = UiOpenText(8, 54, 304, 4);
	fnt_buffer = UiOpenText(8, 106, 304, 4);
	fnt_status = UiOpenText(8, 172, 304, 2);
	SetDispMask(1);

	SpuInit();
	SPU_REG(0x1F801D80) = 0x3FFF;	// Master volume
	SPU_REG(0x1F801D82) = 0x3FFF;
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	AS_IRQ_MASK &= ~(1 << 9);			// The SPU interrupt flag is polled (no interrupt handler)

	OpenLink();

	while (1)
	{
		// Link and SPU between the frames
		LinkService();
		SpuService();
		if (VSync(-1) == frame)
			continue;
		frame = VSync(-1);

		pad = PadRead(1);
#define	PRESSED(_b_)	((pad & (_b_)) && !(opad & (_b_)))
		if (PRESSED(PADLright))					SendCommand(AS_NEXT);
		if (PRESSED(PADLleft))					SendCommand(AS_PREVIOUS);
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
