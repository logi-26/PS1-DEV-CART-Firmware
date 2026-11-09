/*
* Sound tester host: the console's VAB and SEQ player (soundtest.c before main, copied and patched by build_host.py)
* run on the PC, a frame at a time as the tester's main loop runs it (SeqUpdate, SpuKeys), the SPU replaced by memory
* (a voice's envelope: sounding from its key on until a few frames after its key off).
*
*	seqhost [options] FILE.SEQ
*	--vh FILE --vb FILE		the bank (else no voices: the notes are still read and reported)
*	--track N				the SEQ of a SEP (0...)
*	--frame-us N			a frame (16683 NTSC, 20000 PAL)
*	--play-ms N				time played (default: to the music's end or 10 minutes)
*	--seek MS				SeqSeek first (the channels' state then printed: STATE)
*	--repeat 0|1			at the end: played again (1, stops at the first restart) or stopped (0)
*	--tempo PCT --transpose N --mute MASK		the effects (fx_tempo, fx_transpose, fx_mute)
*	--guard					the file put just before a page that can't be read: a read past its end is a CRASH line
*
* Prints: TRACKS n, TRACK i resolution tempo size, SCAN len_us channels, then while playing NOTE wall_us music_us ch note
* vel (each note on read), VOICE ch note prog center shift pitch voll volr (each voice keyed for it), RESTART wall_us,
* STOP wall_us, STATE (after a seek), DONE frames wall_us loops. HANG (nothing for 3 s: an endless loop) and CRASH
* (memory read outside the data) end it.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {int unused;} DISPENV;
typedef struct {int unused;} DRAWENV;

static volatile uint16_t	host_spu[0x200];
static volatile uint32_t	host_dma;
static uint32_t				host_restarts;
static void	HostNote(int c, int note, int vel);
static void	HostNoteDone(int c, int note);

#include "soundtest_host.c"

static volatile uint32_t	host_progress;
static uint32_t				host_frames, host_wall_us;
static uint8_t				*guard_end;
static int					release_frames[VOICES];
static uint32_t				age_before;

static void	HostNote(int c, int note, int vel)
{
	age_before = voice_age;
	if (!seq_seeking)
		printf("NOTE %u %u %d %d %d\n", host_wall_us, seq_us, c, note, vel);
}

static void	HostNoteDone(int c, int note)
{
	int	v;

	for (v = 0; v < VOICES - 1; v++)
		if (voices[v].on && (voices[v].age > age_before) && (voices[v].channel == c) && (voices[v].note == note))
			printf("VOICE %d %d %d %d %d %d %d %d\n", c, note, voices[v].prog, voices[v].tone->center,
				   voices[v].tone->shift, host_spu[v * 8 + 2], host_spu[v * 8], host_spu[v * 8 + 1]);
}

// SpuKeys, and the voices' envelopes as the SPU would have them (ENVX: sounding, 0 a few frames after the key off)
static void	HostSpuKeys(void)
{
	uint32_t	on = key_on, off = key_off;
	int			v;

	SpuKeys();
	for (v = 0; v < VOICES; v++)
	{
		if (release_frames[v] && !--release_frames[v])
			host_spu[v * 8 + 6] = 0;
		if (off & (1 << v))
			release_frames[v] = 6;
		if (on & (1 << v))
		{
			host_spu[v * 8 + 6] = 0x7FFF;
			release_frames[v] = 0;
		}
	}
}

static DWORD WINAPI	Watchdog(LPVOID arg)
{
	uint32_t	last = host_progress - 1;

	for (;;)
	{
		Sleep(3000);
		if (host_progress == last)
		{
			printf("HANG frame %u wall_us %u music_us %u\n", host_frames, host_wall_us, seq_us);
			fflush(stdout);
			ExitProcess(3);
		}
		last = host_progress;
	}
	return (0);
}

static LONG WINAPI	Crash(EXCEPTION_POINTERS *e)
{
	uint8_t	*at = (uint8_t *)e->ExceptionRecord->ExceptionInformation[1];

	printf("CRASH code %08lX at %p past_end %d\n", e->ExceptionRecord->ExceptionCode, (void *)at,
		   guard_end ? (int)(at - guard_end) : 0);
	fflush(stdout);
	ExitProcess(4);
	return (EXCEPTION_CONTINUE_SEARCH);
}

static uint8_t	*ReadFile_(const char *path, uint32_t *size, int guard)
{
	FILE	*f = fopen(path, "rb");
	uint8_t	*data;
	long	n;

	if (!f)
	{
		printf("ERROR can't open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (guard)		// Up to the end of a page, the next one not readable
	{
		DWORD	old;
		uint32_t	pages = (n + 4095) / 4096 + 1;
		uint8_t		*base = VirtualAlloc(NULL, pages * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		VirtualProtect(base + (pages - 1) * 4096, 4096, PAGE_NOACCESS, &old);
		guard_end = base + (pages - 1) * 4096;
		data = guard_end - n;
	}
	else
		data = calloc(1, n + 64);	// (the tester's buffer: zeros after the file)
	if (n && (fread(data, 1, n, f) != (size_t)n))
	{
		printf("ERROR can't read %s\n", path);
		exit(2);
	}
	fclose(f);
	*size = n;
	return (data);
}

int	main(int argc, char **argv)
{
	const char	*vh_path = NULL, *vb_path = NULL, *seq_path = NULL;
	uint32_t	size, vh_size, vb_size, play_ms = 600000, i;
	int			track = 0, seek = -1, guard = 0, was_playing;
	uint8_t		*seq, *vh, *vb;

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	frame_us = 16683;
	for (i = 1; i < (uint32_t)argc; i++)
	{
		const char	*a = argv[i], *v = (i + 1 < (uint32_t)argc) ? argv[i + 1] : "0";

		if (!strcmp(a, "--vh"))				vh_path = argv[++i];
		else if (!strcmp(a, "--vb"))		vb_path = argv[++i];
		else if (!strcmp(a, "--track"))		track = atoi(v), i++;
		else if (!strcmp(a, "--frame-us"))	frame_us = atoi(v), i++;
		else if (!strcmp(a, "--play-ms"))	play_ms = atoi(v), i++;
		else if (!strcmp(a, "--seek"))		seek = atoi(v), i++;
		else if (!strcmp(a, "--repeat"))	seq_repeat = atoi(v), i++;
		else if (!strcmp(a, "--tempo"))		fx_tempo = atoi(v), i++;
		else if (!strcmp(a, "--transpose"))	fx_transpose = atoi(v), i++;
		else if (!strcmp(a, "--mute"))		fx_mute = strtoul(v, NULL, 0), i++;
		else if (!strcmp(a, "--guard"))		guard = 1;
		else								seq_path = a;
	}
	if (!seq_path)
	{
		printf("ERROR no SEQ\n");
		return (2);
	}
	AddVectoredExceptionHandler(1, Crash);
	CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);

	quiet_load = 1;
	if (vh_path && vb_path)
	{
		vh = ReadFile_(vh_path, &vh_size, 0);
		vb = ReadFile_(vb_path, &vb_size, 0);
		if (!LoadVab(vh, vb))
			printf("ERROR not a VH\n");
	}
	seq = ReadFile_(seq_path, &size, guard);
	seq_size = size;
	seq_count = SeqParse(seq, size);
	printf("TRACKS %d\n", seq_count);
	for (i = 0; i < (uint32_t)seq_count; i++)
		printf("TRACK %u %d %u %d\n", i, seq_tracks[i].resolution, seq_tracks[i].tempo,
			   (int)(seq_tracks[i].end - seq_tracks[i].data));
	if (!seq_count || (track >= seq_count))
	{
		printf("DONE 0 0 0\n");
		return (0);
	}
	// As LoadSounds and SeqSelect: the SEQ chosen, scanned, started
	seq_data = seq_file = seq;
	seq_index = track;
	SeqScan();
	printf("SCAN %u %u\n", seq_len_us, seq_channels);
	SeqStart();
	seq_playing = 1;
	seq_loops = 0;
	if (seek >= 0)
	{
		SeqSeek(seek);
		printf("STATE %u", seq_us);
		for (i = 0; i < 16; i++)
			printf(" %d,%d,%d,%d,%d,%d", channels[i].prog, channels[i].volume, channels[i].expression, channels[i].pan,
				   channels[i].bend, channels[i].sustain);
		printf("\n");
	}
	for (host_frames = 0; host_wall_us < play_ms * 1000; host_frames++)
	{
		uint32_t	restarts = host_restarts;

		was_playing = seq_playing;
		SeqUpdate();
		HostSpuKeys();
		host_progress++;
		host_wall_us += frame_us;		// (real time: the tempo effect changes the music's)
		if (host_restarts != restarts)
		{
			printf("RESTART %u\n", host_wall_us);
			break;
		}
		if (was_playing && !seq_playing)
		{
			printf("STOP %u\n", host_wall_us);
			break;
		}
	}
	printf("DONE %u %u %u\n", host_frames, host_wall_us, seq_loops);
	fflush(stdout);
	return (0);
}
