/*
* Movie tester host: the console's movie player (videotest.c before its screen drawing, copied and patched by
* build_host.py) run on the PC a vertical blank at a time, as its main loop runs it, the hardware replaced:
* - file server: the files of --dir, read at --read-kbps (the time a read takes counts against the frame's CPU time);
* - MDEC: a frame decodes in --decode-vsyncs vertical blanks; its BS data checked (header) and its CRC printed;
* - SPU: the two voices play the sound ring from SPU RAM (ADPCM decoded, the loop flags followed, the interrupt raised
*   when a voice reaches its address), into --audio-out (16-bit stereo WAV on the time line of the vertical blanks:
*   left voice 0, right voice 1);
* - GPU: the uploads checked against the VRAM.
*
*	vthost --dir DIR [--cmd VSYNC:COMMAND]... [--vsyncs N] [--until-end] [--pal] [--decode-vsyncs N] [--read-kbps N]
*	       [--audio-out FILE]
*
* Prints the tester's own lines (VIDTEST ...), and DECODE vsync frame bytes crc (a frame given to the MDEC), SHOW vsync
* frame play_us (a frame shown), KEYON vsync voices, OVERFLOW / VRAM / BADBS text (a buffer or the VRAM written outside
* its bounds, a frame the MDEC would refuse), DONE vsync shown dropped late underruns restarts stalls errors, HANG,
* CRASH.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <windows.h>
#include <stdlib.h>
#include "vthost_sdk.h"
#include "videotest_host.c"

static uint32_t		host_vsync, host_cpu_us, decode_vsyncs = 1, read_kbps = 500, mdec_busy_until;
static uint8_t		spu_ram[512 * 1024];
static uint32_t		spu_xfer;
static int			slot_frame[SLOTS];
static char			dir[512];
static FILE			*files[16];
static volatile uint32_t	progress;

// ---------------------------------------------------------------------------------------------------- SDK

static int	VSync(int mode)
{
	return ((int)host_vsync);
}

static int	DrawSync(int mode)
{
	return (0);
}

static void	CheckRect(const char *what, const RECT *r)
{
	if ((r->x < 0) || (r->y < 0) || (r->w <= 0) || (r->h <= 0) || (r->x + r->w > 1024) || (r->y + r->h > 512))
		printf("VRAM %s outside the VRAM: %d,%d %dx%d\n", what, r->x, r->y, r->w, r->h);
}

static void	LoadImage(const RECT *r, const uint32_t *data)
{
	CheckRect("LoadImage", r);
}

static void	ClearImage(const RECT *r, int red, int green, int blue)
{
	CheckRect("ClearImage", r);
}

static void	DecDCTReset(int mode)
{
	mdec_busy_until = 0;
}

static void	DecDCTPutEnv(const void *env, int mono)
{
}

static uint32_t	Crc32(const uint8_t *p, int n)
{
	uint32_t	c = 0xFFFFFFFF;
	int			k;

	while (n-- > 0)
	{
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
	}
	return (~c);
}

// The VLC decoding: the frame's header checked (as the decoder would refuse it), its bytes' CRC printed
static int	DecDCTvlcStart(VLC_Context *ctx, uint32_t *buf, size_t max, const uint32_t *bs)
{
	const uint16_t	*h = (const uint16_t *)bs;
	int				size = last_bytes;

	if ((size < 8) || (size > FRAME_MAX) || (h[1] != 0x3800) || (h[3] < 1) || (h[3] > 3))
	{
		printf("BADBS %u frame %d size %d magic %04X version %d\n", host_vsync, last_frame, size, h[1], h[3]);
		return (-1);
	}
	printf("DECODE %u %d %d %08X\n", host_vsync, last_frame, size, Crc32((const uint8_t *)bs, size));
	buf[0] = h[0];
	return (0);
}

static void	DecDCTin(const uint32_t *data, int mode)
{
	mdec_busy_until = host_vsync + decode_vsyncs;
}

static void	DecDCTout(uint32_t *data, size_t words)
{
	if ((data < frame_buf) || (data + words > frame_buf + FRAME_BUF))
		printf("OVERFLOW DecDCTout %u words at frame_buf + %d (the buffer: %d words)\n", (unsigned)words,
			   (int)(data - frame_buf), FRAME_BUF);
}

static int	DecDCToutSync(int mode)
{
	if (mode == 0)
	{
		mdec_busy_until = 0;
		return (0);
	}
	return (host_vsync < mdec_busy_until);
}

static void	SpuSetTransferStartAddr(uint32_t adrs)
{
	spu_xfer = adrs;
}

static size_t	SpuWrite(const uint32_t *data, size_t size)
{
	if (spu_xfer + size > sizeof(spu_ram))
		printf("OVERFLOW SpuWrite %u bytes at %05X\n", (unsigned)size, (unsigned)spu_xfer);
	else
		memcpy(spu_ram + spu_xfer, data, size);
	return (size);
}

static int	SpuIsTransferCompleted(int mode)
{
	return (1);
}

// ---------------------------------------------------------------------------------------------------- file server

static int	PCopen(const char *name, int flags, int perms)
{
	char	path[1024];
	int		i;

	if (name[0] == '@')
		return (-1);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	for (i = 3; i < 16; i++)
		if (!files[i])
		{
			if (!(files[i] = fopen(path, "rb")))
				return (-1);
			return (i);
		}
	return (-1);
}

static int	PCclose(int fd)
{
	if ((fd >= 3) && (fd < 16) && files[fd])
	{
		fclose(files[fd]);
		files[fd] = NULL;
	}
	return (0);
}

static int	PCread(int fd, char *buf, int size)
{
	if ((fd < 3) || (fd >= 16) || !files[fd])
		return (-1);
	host_cpu_us += (uint32_t)((uint64_t)size * 1000000 / (read_kbps * 1024));
	return ((int)fread(buf, 1, size, files[fd]));
}

static int	PClseek(int fd, int offset, int mode)
{
	if ((fd < 3) || (fd >= 16) || !files[fd])
		return (-1);
	fseek(files[fd], offset, (mode == PCDRV_SEEK_END) ? SEEK_END : SEEK_SET);
	return ((int)ftell(files[fd]));
}

// ---------------------------------------------------------------------------------------------------- hooks

static void	HostCopy(int buf, int offset, const uint8_t *src, int n)
{
	if ((buf < 0) || (buf > 1) || (offset < 0) || (n < 0) || (offset + n > FRAME_MAX))
	{
		printf("OVERFLOW frame_data[%d] + %d, %d bytes (the buffer: %d)\n", buf, offset, n, FRAME_MAX);
		return;
	}
	memcpy((uint8_t *)frame_data[buf] + offset, src, n);
}

static void	HostDecode(int slot, int w, int h)
{
	slot_frame[slot] = last_frame;
}

static void	HostShown(int slot)
{
	printf("SHOW %u %d %u\n", host_vsync, slot_frame[slot], play_us);
}

// ---------------------------------------------------------------------------------------------------- SPU

typedef struct
{
	int			on;
	uint32_t	adrs, pos, frac;		// Block, sample in it, 16.16 fraction of a sample
	int16_t		dec[28];
	int32_t		h1, h2;
	uint32_t	out;					// Next sample of the output (time line)
} HostVoice;

static HostVoice	hv[2];
static int16_t		*audio[2];
static uint32_t		audio_len, audio_rate;
static uint16_t		irq_fired = 0xFFFF;

#define	REG(_a_)	host_spu[((_a_) - 0x1F801C00) / 2]

static void	DecodeBlock(HostVoice *v)
{
	static const int	pos[5] = {0, 60, 115, 98, 122}, neg[5] = {0, 0, -52, -55, -60};
	const uint8_t		*b = spu_ram + v->adrs;
	int					shift = b[0] & 15, filter = (b[0] >> 4) & 7, i;

	if (filter > 4)
		filter = 4;
	if (shift > 12)
		shift = 9;
	for (i = 0; i < 28; i++)
	{
		int32_t	s = (int16_t)(((b[2 + i / 2] >> ((i & 1) * 4)) & 15) << 12) >> shift;

		s += (v->h1 * pos[filter] + v->h2 * neg[filter] + 32) >> 6;
		s = (s > 32767) ? 32767 : ((s < -32768) ? -32768 : s);
		v->h2 = v->h1;
		v->h1 = s;
		v->dec[i] = (int16_t)s;
	}
}

static void	Put(int c, uint32_t at, int16_t s)
{
	if (at >= audio_len)
	{
		uint32_t	n = at + 44100 * 10;

		audio[0] = realloc(audio[0], n * 2);
		audio[1] = realloc(audio[1], n * 2);
		memset(audio[0] + audio_len, 0, (n - audio_len) * 2);
		memset(audio[1] + audio_len, 0, (n - audio_len) * 2);
		audio_len = n;
	}
	audio[c][at] = s;
}

// Key on / off written by the tester; the interrupt flag down once it set the next address (it acknowledged it)
static void	SpuRegs(void)
{
	uint16_t	on = REG(0x1F801D88), off = REG(0x1F801D8C);
	int			c;

	if (off)
	{
		for (c = 0; c < 2; c++)
			if (off & (1 << c))
				hv[c].on = 0;
		REG(0x1F801D8C) = 0;
	}
	if (on)
	{
		printf("KEYON %u %d\n", host_vsync, on);
		for (c = 0; c < 2; c++)
			if (on & (1 << c))
			{
				hv[c].on = 1;
				hv[c].adrs = REG(0x1F801C06 + c * 16) * 8;
				hv[c].pos = hv[c].frac = 0;
				hv[c].h1 = hv[c].h2 = 0;
				hv[c].out = (uint32_t)((uint64_t)host_vsync * VsyncUs() * audio_rate / 1000000);
			}
		REG(0x1F801D88) = 0;
		REG(0x1F801DAE) &= ~0x40;
		irq_fired = 0xFFFF;
	}
	if ((REG(0x1F801DAE) & 0x40) && (REG(0x1F801DA4) != irq_fired))
		REG(0x1F801DAE) &= ~0x40;
}

// The voices played for a vertical blank (at their pitch: 4096 = 44100 Hz)
static void	SpuAdvance(void)
{
	int	c;

	for (c = 0; c < 2; c++)
	{
		HostVoice	*v = &hv[c];
		uint32_t	step = (uint32_t)((uint64_t)REG(0x1F801C04 + c * 16) * 44100 * 65536 / 4096 * VsyncUs() / 1000000);

		if (!v->on)
			continue;
		v->frac += step;
		while (v->on && (v->frac >= 65536))
		{
			v->frac -= 65536;
			if (v->pos == 0)
			{
				if ((REG(0x1F801DAA) & 0x40) && (v->adrs == REG(0x1F801DA4) * 8u) && !(REG(0x1F801DAE) & 0x40))
				{
					REG(0x1F801DAE) |= 0x40;
					irq_fired = REG(0x1F801DA4);
				}
				DecodeBlock(v);
			}
			Put(c, v->out++, v->dec[v->pos]);
			if (++v->pos == 28)
			{
				uint8_t	flags = spu_ram[v->adrs + 1];

				v->pos = 0;
				if (flags & 1)
				{
					if (flags & 2)
						v->adrs = REG(0x1F801C0E + c * 16) * 8;
					else
						v->on = 0;
				}
				else
					v->adrs = (v->adrs + 16) & 0x7FFFF;
			}
		}
	}
}

static void	WriteWav(const char *path)
{
	FILE		*f = fopen(path, "wb");
	uint32_t	n = audio_len, i, rate = audio_rate, bytes = n * 4;

	if (!f)
		return;
	fwrite("RIFF", 1, 4, f);
	i = 36 + bytes;
	fwrite(&i, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	i = 16;
	fwrite(&i, 4, 1, f);
	{
		uint16_t	fmt_[2] = {1, 2};
		uint32_t	r[2] = {rate, rate * 4};
		uint16_t	b[2] = {4, 16};

		fwrite(fmt_, 2, 2, f);
		fwrite(r, 4, 2, f);
		fwrite(b, 2, 2, f);
	}
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
	for (i = 0; i < n; i++)
	{
		fwrite(&audio[0][i], 2, 1, f);
		fwrite(&audio[1][i], 2, 1, f);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------------------------------- running

static DWORD WINAPI	Watchdog(LPVOID arg)
{
	uint32_t	last = progress - 1;

	for (;;)
	{
		Sleep(5000);
		if (progress == last)
		{
			printf("HANG vsync %u\n", host_vsync);
			fflush(stdout);
			ExitProcess(3);
		}
		last = progress;
	}
	return (0);
}

static LONG WINAPI	Crash(EXCEPTION_POINTERS *e)
{
	printf("CRASH code %08lX at %p (vsync %u)\n", e->ExceptionRecord->ExceptionCode,
		   (void *)e->ExceptionRecord->ExceptionInformation[1], host_vsync);
	fflush(stdout);
	ExitProcess(4);
	return (EXCEPTION_CONTINUE_SEARCH);
}

#define	MAX_CMDS	64

int	main(int argc, char **argv)
{
	uint32_t	cmd_at[MAX_CMDS], vsyncs = 60 * 60, frame_report = 0, end_at = 0;
	char		*cmd_text[MAX_CMDS], *audio_out = NULL;
	int			n_cmds = 0, i, until_end = 0;

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	strcpy(dir, ".");
	for (i = 1; i < argc; i++)
	{
		const char	*a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : "";

		if (!strcmp(a, "--dir"))					snprintf(dir, sizeof(dir), "%s", v), i++;
		else if (!strcmp(a, "--vsyncs"))			vsyncs = atoi(v), i++;
		else if (!strcmp(a, "--until-end"))			until_end = 1;
		else if (!strcmp(a, "--pal"))				is_pal = 1;
		else if (!strcmp(a, "--decode-vsyncs"))		decode_vsyncs = atoi(v), i++;
		else if (!strcmp(a, "--read-kbps"))			read_kbps = atoi(v), i++;
		else if (!strcmp(a, "--audio-out"))			audio_out = argv[++i];
		else if (!strcmp(a, "--cmd") && (n_cmds < MAX_CMDS))
		{
			const char	*colon = strchr(v, ':');

			cmd_at[n_cmds] = atoi(v);
			cmd_text[n_cmds++] = strdup(colon ? colon + 1 : v);
			i++;
		}
	}
	AddVectoredExceptionHandler(1, Crash);
	CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
	audio_rate = 37800;

	MdecInit();
	REG(0x1F801D80) = 0x3FFF;
	printf("VIDTEST ready\n");
	for (host_vsync = 0; host_vsync < vsyncs; )
	{
		int		it;

		progress++;
		for (i = 0; i < n_cmds; i++)
			if (cmd_at[i] == host_vsync)
			{
				char	line[160];

				snprintf(line, sizeof(line), "%s", cmd_text[i]);
				Command(line);
				SpuRegs();
				if (rate)
					audio_rate = rate;
			}
		// The loop of main: as many times as the frame's CPU time allows (the file server's reads take it)
		host_cpu_us = 0;
		for (it = 0; (it < 6) && (host_cpu_us < VsyncUs()); it++)
		{
			Load(playing && !paused ? 1 : 4);
			SoundService();
			PlayService();
			SpuRegs();
			if (rate)
				audio_rate = rate;
			DecodeService();
			VideoService();
		}
		if (paused && sound_on)		// (as main)
			PauseSound();
		SpuRegs();
		if (playing && (host_vsync - frame_report >= 30))
		{
			frame_report = host_vsync;
			printf("VIDTEST pos %d %d %u %d %d %u %d %d %d\n", last_frame, frames_total, (unsigned)decode_us, last_bytes,
				   last_scale, (unsigned)stalls, frames_late, frames_dropped, (int)(ram_mode ? 100 :
				   (loaded - consumed) * 100 / (ring_cap ? ring_cap : 1)));
		}
		if (until_end && ended && !end_at)
			end_at = host_vsync + 30;
		if (end_at && (host_vsync >= end_at))
			break;
		host_vsync++;
		host_lines += 263;
		SpuAdvance();
	}
	printf("DONE %u %d %d %d %d %d %u %d %d\n", host_vsync, frames_shown, frames_dropped, frames_late, underruns, restarts,
		   (unsigned)stalls, frame_errors, ended);
	if (audio_out)
		WriteWav(audio_out);
	fflush(stdout);
	return (0);
}
