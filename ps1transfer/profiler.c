/*
* PS1 DEV CART Transfer tool: profiler ("ps1transfer -prof")
*
* Statistical profiler of the running program, without changing it. The debug monitor (monitor/monitor.c, called by
* the kernel hook) uses a root counter the program doesn't use: at each of its ticks (1000 a second by default) it
* sends where the program was (PC, RA, SP) and the state of the hardware (GPU, DMA channels, CD-ROM), every 32 ticks
* the SPU voices, and the RAM use when the firmware filled the free RAM before executing the program (EXEC_PROF_FILL).
* The packets come between the program's printf text, after a Data Link Escape byte (see monitor/monitor.h).
*
* Every second: CPU load (100% - time in VSync/DrawSync/CdSync... found with the program's symbols) and what the
* program waits for, GPU busy, frames drawn (draw area swaps) and the frame time against the budget of the video mode
* (50/60 Hz), what limits the frame rate (CPU, GPU, VSync, CD-ROM) with hints, DMA activity, lowest stack pointer, RAM
* used and free, SPU voices playing, and the functions where the time goes (with the caller seen in the return
* address). As text, or as "PROF {json}" lines (-json, PS1 Dev Cart Manager), and in a CSV file (-csv).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

#include "profiler.h"
#include "../monitor/monitor.h"

#define	MON_FIELD(_f_)	(MONITOR_STATE_ADRS + offsetof(MonitorState, _f_))
#define	SYSCLOCK		33868800
#define	ESC				16			// Data Link Escape
#define	BUCKETS			8192		// Functions seen (hash table)
#define	PAIRS			16384		// Function / caller pairs seen (hash table)
#define	TOP_TOTAL		25
#define	TOP_RECENT		10
#define	MAX_HINTS		8
#define	HINT_SIZE		256

// Provided by main.c
const char	*SymbolAt(uint32_t adrs, uint32_t *start);
bool		SymbolExists(const char *name);

bool	prof_active;

// What a waiting function waits for: its time is not CPU load
enum	{WAIT_NONE, WAIT_VSYNC, WAIT_GPU, WAIT_CD, WAIT_SPU, WAIT_OTHER, WAITS};
static const char	*wait_keys[WAITS] = {"", "vsync", "gpu", "cd", "spu", "other"};

typedef struct
{
	uint32_t	key;		// Function start (or address group), 0 = free entry
	char		*name;
	uint8_t		wait;		// WAIT_* of a waiting function (VSync...), WAIT_NONE: CPU load
	uint32_t	total, period;
} Bucket;

typedef struct
{
	Bucket		*callee, *caller;	// The caller seen in the return address, NULL = free entry
	uint32_t	count;
} Pair;

typedef struct
{
	uint32_t	samples, dropped, idle, frames;
	uint32_t	wait[WAITS];
	uint32_t	flags[8];
	uint32_t	sp_min, sp_max;
} Stats;

// A period (or the whole run) against the budget of a frame: what limits the frame rate
typedef struct
{
	double		cpu, fps, frame_ms, work_ms, cpu_ms, budget_ms;
	int			target;		// Frames a second of the video mode (50 PAL, 60 NTSC)
	const char	*kind;		// ok, capped, vsync, gpu, cd, cpu, unknown (no waiting function), noframes, none
	char		text[320];
} Analysis;

static Bucket		buckets[BUCKETS];
static Pair			pairs[PAIRS];
static Stats		period, total;
static bool			have_seq, have_frames;
static uint8_t		last_seq, last_frames;
static uint16_t		voice_env[24];
static uint32_t		voice_endx;
static uint16_t		spu_cnt;
static int			voices_peak;
static bool			have_voices, have_ram;
static uint32_t		ram_dirty, ram_maxrun, ram_lo, ram_hi;
static uint32_t		gpustat;		// GPUSTAT of the program (debug monitor version 7), 0: not known
static int			mon_version;
static bool			waits_known;	// The symbols have a waiting function (or -idle was given)
static struct
{
	uint32_t	drawing, seconds;	// Seconds with frames drawn, those counted
	double		fps_min, fps_sum, cpu_max, gpu_max;
}					session;

// RAM use shown of the PlayStation's 2MB: everything below the free RAM measured (kernel 64KB, the program with its
// variables) counts as used, plus the free RAM the program has written (the RAM above it, the stack, is not counted)
#define	PS1_RAM_START		0x80000000
#define	PS1_PROGRAM_START	0x80010000
#define	PS1_RAM_KB			2048

static uint32_t	RamUsedKB(void)
{
	return ((ram_lo - PS1_RAM_START) / 1024 + ram_dirty * 4 / 1024);
}

// Free RAM measured: words still holding the pattern, the longest run of them (largest free block)
static uint32_t	RamFreeKB(void)
{
	return (((ram_hi - ram_lo) / 4 - ram_dirty) * 4 / 1024);
}
static int			conflict = -1;
static char			last_char = '\n';
static bool			json;
static char			*user_idle;		// -idle list

// Names of the functions where a program waits (PSn00bSDK, PsyQ, Net Yaroze, PsyQo): their time is not CPU load
static const struct
{
	const char	*name;
	uint8_t		wait;
}					wait_names[] = {
	{"VSync", WAIT_VSYNC}, {"_VSync", WAIT_VSYNC}, {"VSync_wait", WAIT_VSYNC}, {"WaitVSync", WAIT_VSYNC},
	{"_default_vsync_halt", WAIT_VSYNC}, {"psyqo::GPU::pumpCallbacks()", WAIT_VSYNC},
	{"DrawSync", WAIT_GPU}, {"_DrawSync", WAIT_GPU},
	{"CdSync", WAIT_CD}, {"CdReadSync", WAIT_CD},
	{"SpuIsTransferCompleted", WAIT_SPU},
	{NULL, WAIT_NONE}};

static void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

static double	Now(void)
{
#ifdef _WIN32
	return (GetTickCount() / 1000.0);
#else
	struct timeval	tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec + tv.tv_usec / 1e6);
#endif
}

static uint32_t	Get32(uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}

static uint8_t	WaitOf(const char *name)
{
	const char	*p;
	size_t		n = strlen(name);
	int			i;

	for (i = 0; wait_names[i].name; i++)
		if (!strcmp(name, wait_names[i].name))
			return (wait_names[i].wait);
	for (p = user_idle; p && *p; p += strcspn(p, ","), p += (*p == ','))
		if ((strcspn(p, ",") == n) && !strncmp(p, name, n))
			return (WAIT_OTHER);
	return (WAIT_NONE);
}

// Function (or group) of an address: symbol of the program, BIOS, kernel, or 64-byte group without symbols
static Bucket	*BucketFor(uint32_t pc)
{
	uint32_t	phys = pc & 0x1FFFFFFF, key, h;
	const char	*name = NULL;
	char		tmp[32];

	if ((phys >= 0x1FC00000) && (phys < 0x1FC80000))
	{
		key = 0x1FC00000;
		name = "(BIOS)";
	}
	else if (phys < 0x10000)
	{
		key = 0x80000000;
		name = "(kernel)";
	}
	else if (phys < 0x200000)
	{
		pc = 0x80000000 | phys;
		if (!(name = SymbolAt(pc, &key)))
			key = pc & ~0x3F;
	}
	else
	{
		key = 0xFFFFFFFF;
		name = "(other)";
	}
	for (h = (key >> 2) * 2654435761u % BUCKETS; buckets[h].key && (buckets[h].key != key); h = (h + 1) % BUCKETS)
		;
	if (!buckets[h].key)
	{
		if (!name)
		{
			snprintf(tmp, sizeof(tmp), "0x%08X", key);
			name = tmp;
		}
		buckets[h].key = key;
		buckets[h].name = strdup(name);
		buckets[h].wait = WaitOf(name);
	}
	return (&buckets[h]);
}

static bool	Special(const Bucket *b)	{return (b->name[0] == '(');}	// (BIOS), (kernel), (other)

static void	CountCaller(Bucket *callee, Bucket *caller)
{
	uint32_t	h = ((uint32_t)(callee - buckets) * 2654435761u + (uint32_t)(caller - buckets)) % PAIRS, n;

	for (n = 0; n < PAIRS; n++, h = (h + 1) % PAIRS)
		if (!pairs[h].callee || ((pairs[h].callee == callee) && (pairs[h].caller == caller)))
		{
			pairs[h].callee = callee;
			pairs[h].caller = caller;
			pairs[h].count++;
			return;
		}
}

// The caller seen most often in the return address while in this function (its samples in *count), NULL if none
static Bucket	*MainCaller(const Bucket *b, uint32_t *count)
{
	Bucket		*best = NULL;
	uint32_t	i;

	*count = 0;
	for (i = 0; i < PAIRS; i++)
		if ((pairs[i].callee == b) && (pairs[i].count > *count))
		{
			best = pairs[i].caller;
			*count = pairs[i].count;
		}
	return (best);
}

static void	Sample(uint8_t *p)
{
	uint32_t	pc = Get32(p + 2), ra = Get32(p + 6), sp = 0x80000000 | (Get32(p + 10) & 0x1FFFFF);
	uint8_t		f = p[1], wait;
	Bucket		*b = BucketFor(pc), *c = BucketFor(ra);
	int			i;

	if (have_seq)
	{
		period.dropped += (uint8_t)(p[0] - last_seq - 1);
		total.dropped += (uint8_t)(p[0] - last_seq - 1);
	}
	last_seq = p[0];
	have_seq = true;
	if (have_frames)
	{
		period.frames += (uint8_t)(p[13] - last_frames);
		total.frames += (uint8_t)(p[13] - last_frames);
	}
	last_frames = p[13];
	have_frames = true;

	b->total++;
	b->period++;
	period.samples++;
	total.samples++;
	// Waiting: in a waiting function, or in a function it called (its return address)
	if ((wait = b->wait ? b->wait : c->wait))
	{
		period.idle++;
		total.idle++;
		period.wait[wait]++;
		total.wait[wait]++;
	}
	// Caller: the return address in the program's code, when it isn't in the same function (a function that calls
	// no other, e.g. memcpy or maths, a BIOS call, or a function that hasn't called another yet)
	if ((c != b) && !(ra & 3) && ((ra & 0x1FFFFFFF) >= 0x10000) && ((ra & 0x1FFFFFFF) < 0x200000))
		CountCaller(b, c);
	for (i = 0; i < 8; i++)
		if (f & (1 << i))
		{
			period.flags[i]++;
			total.flags[i]++;
		}
	if ((Get32(p + 10) & 0x1FFFFF) && (sp < period.sp_min))
		period.sp_min = sp;
	if ((Get32(p + 10) & 0x1FFFFF) && (sp < total.sp_min))
		total.sp_min = sp;
	if ((Get32(p + 10) & 0x1FFFFF) && (sp > total.sp_max))
		total.sp_max = sp;
}

static int	VoicesPlaying(void)
{
	int	i, n = 0;

	for (i = 0; i < 24; i++)
		if (voice_env[i])
			n++;
	return (n);
}

static int	PacketSize(uint8_t type)
{
	switch (type)
	{
		case PROF_SAMPLE:	return (PROF_SAMPLE_SIZE);
		case PROF_VOICES:	return (PROF_VOICES_SIZE);
		case PROF_MEMORY:	return (PROF_MEMORY_SIZE);
		case PROF_CONFLICT:	return (PROF_CONFLICT_SIZE);
	}
	return (0);
}

static bool	HandlePacket(uint8_t type)
{
	uint8_t	p[PROF_VOICES_SIZE];
	int		i;

	if (!USB_ReadData(p, PacketSize(type)))
		return (false);
	switch (type)
	{
		case PROF_SAMPLE:
			Sample(p);
		break;
		case PROF_VOICES:
			for (i = 0; i < 24; i++)
				voice_env[i] = p[i * 2] | (p[i * 2 + 1] << 8);
			voice_endx = Get32(p + 48);
			spu_cnt = p[52] | (p[53] << 8);
			have_voices = true;
			if (VoicesPlaying() > voices_peak)
				voices_peak = VoicesPlaying();
		break;
		case PROF_MEMORY:
			ram_dirty = Get32(p);
			ram_maxrun = Get32(p + 4);
			have_ram = true;
		break;
		case PROF_CONFLICT:
			conflict = p[0];
		break;
	}
	return (true);
}

bool	ProfReadPacketType(uint8_t type)
{
	return (PacketSize(type) && HandlePacket(type));
}

bool	ProfReadPacket(void)
{
	uint8_t	type;

	return (USB_ReadData(&type, 1) && ProfReadPacketType(type));
}

/****************************************/
// Reports

static int	BucketCompare(const void *a, const void *b)
{
	const Bucket	*x = *(const Bucket **)a, *y = *(const Bucket **)b;

	return ((x->total < y->total) - (x->total > y->total));
}

static int	BucketComparePeriod(const void *a, const void *b)
{
	const Bucket	*x = *(const Bucket **)a, *y = *(const Bucket **)b;

	return ((x->period < y->period) - (x->period > y->period));
}

// Buckets with samples, sorted (total or period counts)
static int	SortedBuckets(Bucket ***list, bool by_period)
{
	static Bucket	*l[BUCKETS];
	int				i, n = 0;

	for (i = 0; i < BUCKETS; i++)
		if (buckets[i].key && (by_period ? buckets[i].period : buckets[i].total))
			l[n++] = &buckets[i];
	qsort(l, n, sizeof(Bucket *), by_period ? BucketComparePeriod : BucketCompare);
	*list = l;
	return (n);
}

static double	Pct(uint32_t n, uint32_t of)
{
	return (of ? n * 100.0 / of : 0.0);
}

static void	JsonString(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++)
		if ((*s == '"') || (*s == '\\'))
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s >= 32)
			fputc(*s, f);
	fputc('"', f);
}

// Initial stack pointer from the executable header, or the highest one seen (PSn00bSDK executables leave it to 0)
static uint32_t	StackTop(ProfOptions *o)
{
	return (o->stack_top ? o->stack_top : total.sp_max);
}

// Video mode of the program (GPUSTAT): 50 or 60 frames a second, 60 when not known (older debug monitor)
static int	TargetFps(void)
{
	return ((gpustat && (gpustat & (1 << 20))) ? 50 : 60);
}

static int	VideoWidth(void)
{
	static const int	w[4] = {256, 320, 512, 640};

	return ((gpustat & (1 << 16)) ? 368 : w[(gpustat >> 17) & 3]);
}

// The busiest function that isn't waiting (period or total counts), NULL if none
static Bucket	*TopBusy(bool by_period)
{
	Bucket	**list;
	int		n = SortedBuckets(&list, by_period), i;

	for (i = 0; i < n; i++)
		if (!list[i]->wait)
			return (list[i]);
	return (NULL);
}

// What limits the frame rate during a period (or the whole run): the time of a frame (from the draw area swaps)
// split into CPU, waiting for the GPU (DrawSync), the CD-ROM, and the VBlank (VSync), against the budget of a frame
static void	Analyse(const Stats *st, double seconds, bool by_period, Analysis *a)
{
	uint32_t	s = st->samples;
	double		vsync = Pct(st->wait[WAIT_VSYNC], s) / 100, gpu_wait = Pct(st->wait[WAIT_GPU], s) / 100;
	double		cd_wait = Pct(st->wait[WAIT_CD], s) / 100, gpu = Pct(st->flags[0], s), cd = Pct(st->flags[7], s);
	const char	*hz = gpustat ? "" : " (60 Hz assumed: update the firmware for the video mode)";
	Bucket		*top = TopBusy(by_period);
	double		top_pct = top ? Pct(by_period ? top->period : top->total, s) : 0;
	int			n;

	memset(a, 0, sizeof(*a));
	a->target = TargetFps();
	a->budget_ms = 1000.0 / a->target;
	a->cpu = s ? 100.0 - Pct(st->idle, s) : 0.0;
	a->fps = (seconds > 0) ? st->frames / seconds : 0.0;
	a->kind = "none";
	if (!s)
		return;
	if (a->fps >= 0.5)
	{
		a->frame_ms = 1000.0 / a->fps;
		a->work_ms = a->frame_ms * (1 - vsync);		// The frame without waiting for the VBlank
		a->cpu_ms = a->frame_ms * a->cpu / 100;
	}
	if (a->fps < 0.5)
	{
		a->kind = "noframes";
		snprintf(a->text, sizeof(a->text), "No frame drawn (the draw area doesn't change): loading, or a program that "
			"draws in one buffer. CPU busy %.0f%%.", a->cpu);
	}
	else if (a->fps >= a->target * 0.95)
	{
		a->kind = "ok";
		if (waits_known)
			snprintf(a->text, sizeof(a->text), "Full frame rate (%.0f fps): %.1f of the %.1f ms of a frame used "
				"(CPU %.1f ms), %.0f%% to spare%s.", a->fps, a->work_ms, a->budget_ms, a->cpu_ms,
				(a->work_ms < a->budget_ms) ? 100 - a->work_ms * 100 / a->budget_ms : 0.0, hz);
		else
			snprintf(a->text, sizeof(a->text), "Full frame rate (%.0f fps)%s.", a->fps, hz);
	}
	else if (!waits_known)
	{
		a->kind = "unknown";
		snprintf(a->text, sizeof(a->text), "%.0f fps of %d: no waiting function known (VSync, DrawSync...), so the "
			"CPU load includes the waits. Give the program's symbols, or mark its wait loop as waiting.", a->fps,
			a->target);
	}
	else if ((gpu_wait >= 0.25) || ((gpu >= 90) && (gpu_wait >= 0.1)))
	{
		a->kind = "gpu";
		snprintf(a->text, sizeof(a->text), "GPU-bound: %.0f fps of %d, %.1f ms a frame. The CPU waits for drawing "
			"(DrawSync) %.1f ms of it, GPU busy %.0f%%. Draw less: fewer or smaller polygons, less overdraw and "
			"semi-transparency, no texture upload each frame.", a->fps, a->target, a->frame_ms,
			a->frame_ms * gpu_wait, gpu);
	}
	else if ((cd_wait >= 0.2) || ((cd >= 50) && (cd_wait >= 0.05)))
	{
		a->kind = "cd";
		snprintf(a->text, sizeof(a->text), "Waiting for the CD-ROM: %.0f fps of %d, %.1f ms a frame waiting for it "
			"(CD busy %.0f%%): loading or streaming during the game.", a->fps, a->target, a->frame_ms * cd_wait, cd);
	}
	else if ((vsync >= 0.1) && (a->work_ms <= a->budget_ms * 1.05))
	{
		a->kind = "capped";
		snprintf(a->text, sizeof(a->text), "Capped at %.0f fps by VSync: a frame takes %.1f ms, it would fit the "
			"%.1f ms of a %d fps frame (the program waits for more than one VBlank, e.g. VSync(2))%s.", a->fps,
			a->work_ms, a->budget_ms, a->target, hz);
	}
	else if (vsync >= 0.1)
	{
		a->kind = "vsync";
		snprintf(a->text, sizeof(a->text), "%.0f fps of %d: a frame takes %.1f ms (CPU %.1f, waiting for the GPU "
			"%.1f), %.1f ms over the %.1f ms budget, so it waits for the next VBlank. Save %.1f ms a frame (%.0f%%) "
			"for %d fps%s.", a->fps, a->target, a->work_ms, a->cpu_ms, a->frame_ms * gpu_wait,
			a->work_ms - a->budget_ms, a->budget_ms, a->work_ms - a->budget_ms,
			(a->work_ms - a->budget_ms) * 100 / a->work_ms, a->target, hz);
	}
	else
	{
		a->kind = "cpu";
		n = snprintf(a->text, sizeof(a->text), "CPU-bound: %.0f fps of %d, busy %.0f%% (%.1f ms a frame for a %.1f ms "
			"budget). Start with the functions at the top", a->fps, a->target, a->cpu, a->cpu_ms, a->budget_ms);
		if (top && (n > 0) && (n < (int)sizeof(a->text)))
			snprintf(a->text + n, sizeof(a->text) - n, ": %s %.0f%%.", top->name, top_pct);
		else if ((n > 0) && (n < (int)sizeof(a->text)))
			snprintf(a->text + n, sizeof(a->text) - n, ".");
	}
}

// Advice from the whole run: where the time goes that can be saved, RAM, SPU voices, sampling
static int	Hints(char h[MAX_HINTS][HINT_SIZE])
{
	Bucket		**list, *c, *top = TopBusy(false);
	uint32_t	s = total.samples, count;
	int			n = SortedBuckets(&list, false), i, k = 0;
	double		p;

	if (!s)
		return (0);
	if (!symbol_count)
		snprintf(h[k++], HINT_SIZE, "No symbols: functions are shown as 64-byte address groups and the CPU load "
			"includes the waits. Put the .elf, .map or .sym next to the program (a Net Yaroze ECOFF executable has "
			"its own), or give one with -sym.");
	else if (!waits_known)
		snprintf(h[k++], HINT_SIZE, "No waiting function (VSync, DrawSync...) in the symbols: the CPU load includes "
			"the waits. Mark the program's wait loop as waiting (-idle name, or a double click in the GUI).");
	for (i = 0; (i < n) && (k < MAX_HINTS); i++)
	{
		p = Pct(list[i]->total, s);
		c = MainCaller(list[i], &count);
		if (!strcmp(list[i]->name, "(BIOS)") && (p >= 3))
			snprintf(h[k++], HINT_SIZE, "%.0f%% of the time in the BIOS%s%s (slow ROM code, not cached): keep BIOS "
				"calls out of the game loop (printf, the BIOS memcpy/memset/strcpy, rand...).", p,
				c ? ", mostly called from " : "", c ? c->name : "");
		else if (!strcmp(list[i]->name, "(kernel)") && (p >= 5))
			snprintf(h[k++], HINT_SIZE, "%.0f%% of the time in the kernel: interrupt handlers and callbacks (VSync and "
				"DrawSync callbacks, pads, CD-ROM, root counters, events).", p);
		else if (!list[i]->wait && strstr(list[i]->name, "printf") && (p >= 2))
			snprintf(h[k++], HINT_SIZE, "%s takes %.0f%% of the time: text output is slow, keep it out of the game "
				"loop.", list[i]->name, p);
	}
	if (top && !Special(top) && (Pct(top->total, s) >= 20) && (k < MAX_HINTS))
	{
		c = MainCaller(top, &count);
		snprintf(h[k++], HINT_SIZE, "%s alone takes %.0f%% of the time%s%s: the first place to optimise.", top->name,
			Pct(top->total, s), (c && (count * 2 >= top->total)) ? ", mostly called from " : "",
			(c && (count * 2 >= top->total)) ? c->name : "");
	}
	if (have_ram && (RamFreeKB() < 64) && (k < MAX_HINTS))
		snprintf(h[k++], HINT_SIZE, "Only %u KB of free RAM left (largest free block %u KB).", RamFreeKB(),
			ram_maxrun * 4 / 1024);
	if ((voices_peak >= 24) && (k < MAX_HINTS))
		snprintf(h[k++], HINT_SIZE, "All 24 SPU voices played at once: a new sound takes the voice of another one, "
			"or isn't heard.");
	if ((Pct(total.dropped, total.samples + total.dropped) >= 2) && (k < MAX_HINTS))
		snprintf(h[k++], HINT_SIZE, "%.0f%% of the samples dropped: the USB link can't keep up, use a lower rate "
			"(e.g. 500).", Pct(total.dropped, total.samples + total.dropped));
	return (k);
}

static void	NewLine(void)
{
	if (last_char != '\n')
		putchar('\n');
	last_char = '\n';
}

static void	Report(ProfOptions *o, double t, double dt, FILE *csv)
{
	Bucket		**list, *c;
	int			n, i, k;
	uint32_t	s = period.samples, count;
	double		gpu = Pct(period.flags[0], s), rate = dt > 0 ? (s + period.dropped) / dt : 0.0;
	uint32_t	top = StackTop(o), stack = (top && (period.sp_min < top)) ? top - period.sp_min : 0;
	uint32_t	stack_peak = (top && (total.sp_min < top)) ? top - total.sp_min : 0;
	bool		known = symbol_count != 0;
	Analysis	a;
	char		hints[MAX_HINTS][HINT_SIZE];

	Analyse(&period, dt, true, &a);
	// Averages and peaks of the running game: not while it boots or loads (no frame drawn, the CPU busy all the
	// time), nor the second it draws its first frames (partly loading)
	if (s && (a.fps >= 0.5) && (session.drawing++ >= 1))
	{
		session.seconds++;
		session.fps_sum += a.fps;
		if ((a.fps >= 0.5) && (!session.fps_min || (a.fps < session.fps_min)))	// (not the loading seconds)
			session.fps_min = a.fps;
		if (a.cpu > session.cpu_max)
			session.cpu_max = a.cpu;
		if (gpu > session.gpu_max)
			session.gpu_max = gpu;
	}
	k = Hints(hints);

	if (json)
	{
		NewLine();
		printf("PROF {\"t\":%.1f,\"rate\":%.0f,\"samples\":%u,\"dropped\":%u,\"symbols\":%s,", t, rate, s, period.dropped,
			known ? "true" : "false");
		printf("\"cpu\":%.1f,\"cpu_known\":%s,\"gpu\":%.1f,\"gpu_cmd\":%.1f,\"fps\":%.1f,", a.cpu,
			waits_known ? "true" : "false", gpu, Pct(period.flags[6], s), a.fps);
		printf("\"wait\":{");
		for (i = 1; i < WAITS; i++)
			printf("%s\"%s\":%.1f", (i > 1) ? "," : "", wait_keys[i], Pct(period.wait[i], s));
		printf("},\"frame\":{\"target\":%d,\"ms\":%.2f,\"work_ms\":%.2f,\"cpu_ms\":%.2f,\"budget_ms\":%.2f},",
			a.target, a.frame_ms, a.work_ms, a.cpu_ms, a.budget_ms);
		printf("\"verdict\":{\"kind\":\"%s\",\"text\":", a.kind);
		JsonString(stdout, a.text);
		printf("},\"hints\":[");
		for (i = 0; i < k; i++)
		{
			printf(i ? "," : "");
			JsonString(stdout, hints[i]);
		}
		printf("],");
		if (gpustat)
			printf("\"video\":{\"pal\":%s,\"hz\":%d,\"width\":%d,\"interlaced\":%s,\"display\":%s},",
				(gpustat & (1 << 20)) ? "true" : "false", TargetFps(), VideoWidth(),
				(gpustat & (1 << 22)) ? "true" : "false", (gpustat & (1 << 23)) ? "false" : "true");
		printf("\"dma\":{\"gpu\":%.1f,\"spu\":%.1f,\"cd\":%.1f,\"mdec\":%.1f,\"otc\":%.1f},\"cd\":%.1f,",
			Pct(period.flags[1], s), Pct(period.flags[2], s), Pct(period.flags[3], s), Pct(period.flags[4], s),
			Pct(period.flags[5], s), Pct(period.flags[7], s));
		if (period.sp_min != 0xFFFFFFFF)
			printf("\"sp\":%u,\"stack\":%u,\"stack_peak\":%u,\"stack_top\":%u,", period.sp_min, stack, stack_peak, top);
		if (have_ram)
			printf("\"ram\":{\"lo\":%u,\"hi\":%u,\"used\":%u,\"free\":%u,\"gap\":%u},", ram_lo, ram_hi, ram_dirty * 4,
				ram_hi - ram_lo - ram_dirty * 4, ram_maxrun * 4);
		if (have_voices)
		{
			printf("\"spu\":{\"on\":%s,\"playing\":%d,\"peak\":%d,\"endx\":%u,\"env\":[", (spu_cnt & 0x8000) ? "true" :
				"false", VoicesPlaying(), voices_peak, voice_endx);
			for (i = 0; i < 24; i++)
				printf(i ? ",%u" : "%u", voice_env[i]);
			printf("]},");
		}
		printf("\"session\":{\"seconds\":%u,\"fps_min\":%.1f,\"fps_avg\":%.1f,\"cpu_max\":%.1f,\"gpu_max\":%.1f},",
			session.seconds, session.fps_min, session.seconds ? session.fps_sum / session.seconds : 0.0,
			session.cpu_max, session.gpu_max);
		printf("\"recent\":[");
		n = SortedBuckets(&list, true);
		for (i = 0; (i < n) && (i < TOP_RECENT); i++)
		{
			printf(i ? ",[" : "[");
			JsonString(stdout, list[i]->name);
			printf(",%u,%.1f,%d]", list[i]->period, Pct(list[i]->period, s), list[i]->wait);
		}
		// Since the start: name, samples, %, waiting (WAIT_*), main caller, % of the function's samples from it
		printf("],\"top\":[");
		n = SortedBuckets(&list, false);
		for (i = 0; (i < n) && (i < TOP_TOTAL); i++)
		{
			printf(i ? ",[" : "[");
			JsonString(stdout, list[i]->name);
			printf(",%u,%.1f,%d,", list[i]->total, Pct(list[i]->total, total.samples), list[i]->wait);
			c = MainCaller(list[i], &count);
			JsonString(stdout, c ? c->name : "");
			printf(",%.1f]", Pct(count, list[i]->total));
		}
		printf("],\"total\":%u}\n", total.samples);
	}
	else
	{
		NewLine();
		printf("%5.0fs  CPU %3.0f%%%s  GPU %3.0f%%  %4.1f fps", t, a.cpu, waits_known ? "" : "?", gpu, a.fps);
		if (a.frame_ms > 0)
			printf(" (%.1f ms, %d fps budget %.1f ms)", a.frame_ms, a.target, a.budget_ms);
		if (period.wait[WAIT_VSYNC] || period.wait[WAIT_GPU] || period.wait[WAIT_CD])
			printf("  wait: VSync %.0f%% GPU %.0f%% CD %.0f%%", Pct(period.wait[WAIT_VSYNC], s),
				Pct(period.wait[WAIT_GPU], s), Pct(period.wait[WAIT_CD], s));
		if (have_ram)	// Of the 2MB: kernel and program (below the free RAM) + the free RAM written
			printf("  RAM %u/%u KB, %u KB free (largest block %u KB)", RamUsedKB(), PS1_RAM_KB, RamFreeKB(),
				ram_maxrun * 4 / 1024);
		if (stack)
			printf("  stack %u B (peak %u)", stack, stack_peak);
		if (have_voices)
			printf("  SPU %d/24 voices (%.0f%%, peak %d)", VoicesPlaying(), VoicesPlaying() * 100.0 / 24, voices_peak);
		if (period.dropped)
			printf("  (%u samples dropped)", period.dropped);
		n = SortedBuckets(&list, true);
		printf("  [%s]  |", a.kind);
		for (i = 0; (i < n) && (i < 3); i++)
			printf(" %s %.0f%%", list[i]->name, Pct(list[i]->period, s));
		printf("\n");
	}
	if (csv)
	{
		n = SortedBuckets(&list, true);
		fprintf(csv, "%.1f,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,", t, s, period.dropped, a.cpu, gpu, a.fps,
			Pct(period.flags[1], s), Pct(period.flags[2], s), Pct(period.flags[3], s), Pct(period.flags[4], s),
			Pct(period.flags[5], s), Pct(period.flags[7], s));
		fprintf(csv, have_ram ? "%u,%u," : ",,", ram_dirty * 4, ram_maxrun * 4);
		fprintf(csv, (period.sp_min != 0xFFFFFFFF) ? "0x%08X," : ",", period.sp_min);
		fprintf(csv, have_voices ? "%d," : ",", VoicesPlaying());
		for (i = 0; i < 3; i++)
			if (i < n)
				fprintf(csv, "\"%s\",%.1f,", list[i]->name, Pct(list[i]->period, s));
			else
				fprintf(csv, ",,");
		fprintf(csv, have_ram ? "%u," : ",", ram_hi - ram_lo - ram_dirty * 4);
		fprintf(csv, "%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%d,%s\n", Pct(period.wait[WAIT_VSYNC], s),
			Pct(period.wait[WAIT_GPU], s), Pct(period.wait[WAIT_CD], s), Pct(period.wait[WAIT_SPU], s), a.frame_ms,
			a.work_ms, a.target, a.kind);
		fflush(csv);
	}

	for (i = 0; i < BUCKETS; i++)
		buckets[i].period = 0;
	memset(&period, 0, sizeof(period));
	period.sp_min = 0xFFFFFFFF;
}

static void	FinalReport(ProfOptions *o, double seconds)
{
	Bucket		**list, *c;
	int			n, i, k;
	uint32_t	count;
	Analysis	a;
	char		hints[MAX_HINTS][HINT_SIZE];

	if (json || !total.samples)
		return;
	NewLine();
	Analyse(&total, seconds, false, &a);
	printf("\n%u samples, %u dropped. CPU %.0f%%%s (waiting: VSync %.0f%%, GPU %.0f%%, CD-ROM %.0f%%, SPU %.0f%%, "
		"other %.0f%%), GPU busy %.0f%%\n", total.samples, total.dropped, a.cpu, waits_known ? "" : " (no waiting "
		"function known)", Pct(total.wait[WAIT_VSYNC], total.samples), Pct(total.wait[WAIT_GPU], total.samples),
		Pct(total.wait[WAIT_CD], total.samples), Pct(total.wait[WAIT_SPU], total.samples),
		Pct(total.wait[WAIT_OTHER], total.samples), Pct(total.flags[0], total.samples));
	if (session.seconds)
		printf("Frames (after loading): %.1f fps on average, %.1f at the lowest (%d fps budget %.1f ms%s); peaks: CPU "
			"%.0f%%, GPU %.0f%%\n", session.fps_sum / session.seconds, session.fps_min, a.target, a.budget_ms,
			gpustat ? ((gpustat & (1 << 20)) ? ", PAL" : ", NTSC") : ", video mode not known", session.cpu_max,
			session.gpu_max);
	if ((total.sp_min != 0xFFFFFFFF) && StackTop(o) && (total.sp_min < StackTop(o)))
		printf("Deepest stack: %u bytes (SP 0x%08X, top 0x%08X%s)\n", StackTop(o) - total.sp_min, total.sp_min,
			StackTop(o), o->stack_top ? "" : ": highest SP seen");
	if (have_ram)
		printf("RAM: %u/%u KB used, %u KB free (largest free block %u KB)\n", RamUsedKB(), PS1_RAM_KB, RamFreeKB(),
			ram_maxrun * 4 / 1024);
	if (have_voices)
		printf("SPU: at most %d of the 24 voices played at once (%.0f%%)\n", voices_peak, voices_peak * 100.0 / 24);
	printf("Verdict: %s\n", a.text[0] ? a.text : "-");
	for (i = 0, k = Hints(hints); i < k; i++)
		printf("- %s\n", hints[i]);
	if (!symbol_count)
		printf("No symbols: addresses grouped by 64 bytes (give the .map/.elf with -sym, or run the program with -e)\n");
	printf("\n  Samples      %%  Function                          Called from (when seen in the return address)\n");
	n = SortedBuckets(&list, false);
	for (i = 0; (i < n) && (i < 30); i++)
	{
		c = MainCaller(list[i], &count);
		printf("%9u  %5.1f  %-32s  %s", list[i]->total, Pct(list[i]->total, total.samples), list[i]->name,
			list[i]->wait ? "(waiting) " : "");
		if (c)
			printf("%s %.0f%%", c->name, Pct(count, list[i]->total));
		printf("\n");
	}
}

/****************************************/

// Bytes waiting in the USB FIFO: the program's printf text, profiler packets, PCDRV requests. Read before a command
// (the text would be read as its answer), and in the main loop. False on a USB error.
static bool	Drain(int ms)
{
	double	end = Now() + ms / 1000.0;
	uint8_t	c, type;

	do
	{
		while (USB_ReadByteNoWait(&c))
		{
			if (c == ESC)
			{
				if (!USB_ReadData(&type, 1))
					return (false);
				if (PacketSize(type))
				{
					if (!HandlePacket(type))
						return (false);
				}
				else
					PC_FileServerCmd(type);		// A PCDRV request of the program
			}
			else
			{
				putchar(c);
				last_char = c;
			}
		}
		if (ms)
			SleepMs(1);
	}
	while (Now() < end);
	return (true);
}

static bool	SetProfiler(ProfOptions *o, bool on, uint32_t entry)
{
	uint8_t		p[12];
	uint32_t	ticks = (o->counter == 2) ? SYSCLOCK / 8 / o->rate : SYSCLOCK / o->rate;
	uint16_t	mode = 0x0058 | ((o->counter == 2) ? 0x0200 : 0);	// Reset at target, interrupt, repeat; clock

	if (!Drain(50))
		return (false);
	if (!on)	// The monitor puts the exception vector back, then clears prof_on (or prof_on cleared here after a second)
	{
		double	end = Now() + 1.0;

		p[0] = 3;
		if (!USB_Process('U', p, MON_FIELD(prof_armed), 1))
			return (false);
		do
		{
			if (!Drain(20) || !USB_Process('D', p, MON_FIELD(prof_on), 1))
				return (false);
		}
		while (p[0] && (Now() < end));
		if (!p[0])
			return (true);
		p[0] = 0;
		return (USB_Process('U', p, MON_FIELD(prof_on), 1));
	}
	// The kernel hook calls the monitor entry, which sets the counter up and puts the sampler in RAM (prof_call)
	if (!USB_Process('U', (uint8_t *)&entry, MON_FIELD(prof_call), 4))
		return (false);
	memset(p, 0, sizeof(p));
	p[0] = o->counter + 1;		// prof_on, prof_seq, prof_frames, prof_armed = 0
	p[4] = ticks;
	p[5] = ticks >> 8;
	p[6] = mode;
	p[7] = mode >> 8;			// prof_draw = 0
	return (USB_Process('U', p, MON_FIELD(prof_on), sizeof(p)));
}

int	UsbProfile(ProfOptions *o)
{
	MonitorState	ms;
	FILE			*csv = NULL;
	double			start = Now(), last;
	uint32_t		ticks = (o->counter == 2) ? SYSCLOCK / 8 / o->rate : SYSCLOCK / o->rate;
	int				ret = 1, i;
	uint8_t			type;

	setvbuf(stdout, NULL, _IONBF, 0);
	ps1_program_running = true;		// Only the kernel hook answers
	usb_big_packet_size = 0;
	usb_quiet = true;
	json = o->json;
	user_idle = o->idle;
	if ((o->counter < 0) || (o->counter > 2) || (o->rate < 1) || (o->rate > 10000) || (ticks > 0xFFFF))
	{
		printf("-prof: %d samples a second can't be done with root counter %d (%s to 10000)\n", o->rate, o->counter,
			(o->counter == 2) ? "65" : "517");
		return (1);
	}
	if (!USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)) || (ms.magic != MONITOR_MAGIC) ||
		(ms.version < 3) || !ms.entry)
	{
		printf("-prof needs a running program and the PS1 DEV CART firmware with the profiler (debug monitor 3).\n");
		return (1);
	}
	if (ms.reason != MON_REASON_NONE)
	{
		printf("The program is stopped (debugger or crash).\n");
		return (1);
	}
	if (ms.debug)
	{
		printf("-prof can't be used during a debug session (-gdb).\n");
		return (1);
	}
	mon_version = ms.version;
	ram_lo = ms.ram_lo;
	ram_hi = ms.ram_hi;
	AutoLoadSymbols(o->symfile);
	waits_known = o->idle && o->idle[0];
	for (i = 0; !waits_known && wait_names[i].name; i++)
		waits_known = SymbolExists(wait_names[i].name);
	if (o->csv && !(csv = fopen(o->csv, "w")))
	{
		printf("Can't write %s\n", o->csv);
		return (1);
	}
	if (csv)	// (the columns added later are at the end)
		fprintf(csv, "time,samples,dropped,cpu,gpu,fps,dma_gpu,dma_spu,dma_cd,dma_mdec,dma_otc,cd_busy,ram_used,"
			"ram_gap,sp_min,spu_voices,top1,top1_pct,top2,top2_pct,top3,top3_pct,ram_free,wait_vsync,wait_gpu,"
			"wait_cd,wait_spu,frame_ms,work_ms,target_fps,verdict\n");

	memset(buckets, 0, sizeof(buckets));
	memset(pairs, 0, sizeof(pairs));
	memset(&period, 0, sizeof(period));
	memset(&total, 0, sizeof(total));
	memset(&session, 0, sizeof(session));
	period.sp_min = total.sp_min = 0xFFFFFFFF;
	prof_active = true;
	LinkCatchInterrupt();
	if (!SetProfiler(o, true, ms.entry) || !Drain(300) || !USB_Process('D', &type, MON_FIELD(prof_on), 1))
		goto end;
	if (!type)		// Not started by the monitor
	{
		printf("The profiler couldn't start (debug session?).\n");
		prof_active = false;
		return (1);
	}
	printf("Profiling: %d samples a second (root counter %d)%s%s. Stop with Ctrl+C.\n", o->rate, o->counter,
		symbol_count ? "" : ", no symbols", ram_hi ? "" : ", RAM use not measured (program not executed with -prof)");
	if (symbol_count && !waits_known)
		printf("No waiting function (VSync, DrawSync...) in the symbols: the CPU load includes the waits (-idle).\n");
	if (ram_hi)
		printf("RAM use measured: program %u KB, %u KB free at start\n", (ram_lo - PS1_PROGRAM_START) / 1024,
			(ram_hi - ram_lo) / 1024);
	last_char = '\n';

	start = last = Now();
	while (!LinkStopRequested() && (conflict < 0))
	{
		if (!Drain(0))
			goto end;
		SleepMs(1);
		if (Now() - last >= 1.0)
		{
			// Video mode of the program, kept by the debug monitor (version 7) with the SPU voices
			if ((mon_version >= 7) && !USB_Process('D', (uint8_t *)&gpustat, MON_FIELD(prof_gpustat), 4))
				goto end;
			Report(o, Now() - start, Now() - last, csv);
			last = Now();
		}
	}
	if (conflict >= 0)
	{
		NewLine();
		printf("The program uses root counter %d: profiling stopped, try another one (-prof rate,counter).\n", conflict);
	}
	ret = 0;

end:
	if (!SetProfiler(o, false, 0))
	{
		printf("USB error.\n");
		ret = 1;
	}
	Drain(200);		// Packets already sent
	prof_active = false;
	FinalReport(o, Now() - start);
	if (csv)
		fclose(csv);
	return (ret);
}
