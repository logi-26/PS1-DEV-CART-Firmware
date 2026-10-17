/*
* PS1 DEV CART Transfer tool: profiler ("ps1transfer -prof")
*
* Statistical profiler of the running program, without changing it. The debug monitor (monitor/monitor.c, called by
* the kernel hook) uses a root counter the program doesn't use: at each of its ticks (1000 a second by default) it
* sends where the program was (PC, RA, SP) and the state of the hardware (GPU, DMA channels, CD-ROM), every 32 ticks
* the SPU voices, and the RAM use when the firmware filled the free RAM before executing the program (EXEC_PROF_FILL).
* The packets come between the program's printf text, after a Data Link Escape byte (see monitor/monitor.h).
*
* Every second: CPU load (100% - time in VSync/DrawSync/CdSync... found with the program's symbols), GPU busy, frames
* drawn (draw area swaps), DMA activity, lowest stack pointer, RAM used, SPU voices playing, and the functions where
* the time goes. As text, or as "PROF {json}" lines (-json, PS1 Dev Cart Manager), and in a CSV file (-csv).
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
#define	TOP_TOTAL		25
#define	TOP_RECENT		10

// Provided by main.c
const char	*SymbolAt(uint32_t adrs, uint32_t *start);

bool	prof_active;

typedef struct
{
	uint32_t	key;		// Function start (or address group), 0 = free entry
	char		*name;
	bool		idle;		// Waiting function (VSync...)
	uint32_t	total, period;
} Bucket;

typedef struct
{
	uint32_t	samples, dropped, idle, frames;
	uint32_t	flags[8];
	uint32_t	sp_min, sp_max;
} Stats;

static Bucket		buckets[BUCKETS];
static Stats		period, total;
static bool			have_seq, have_frames;
static uint8_t		last_seq, last_frames;
static uint16_t		voice_env[24];
static uint32_t		voice_endx;
static uint16_t		spu_cnt;
static bool			have_voices, have_ram;
static uint32_t		ram_dirty, ram_maxrun, ram_lo, ram_hi;
static int			conflict = -1;
static char			last_char = '\n';
static bool			json;
static char			*user_idle;		// -idle list

// Names of the functions where a program waits (PSn00bSDK, PsyQ): their time is not CPU load
static const char	*idle_names[] = {"VSync", "DrawSync", "CdSync", "CdReadSync", "SpuIsTransferCompleted",
								"_VSync", "_DrawSync", "VSync_wait", "WaitVSync", "_default_vsync_halt", NULL};

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

static bool	IdleName(const char *name)
{
	const char	*p;
	size_t		n = strlen(name);
	int			i;

	for (i = 0; idle_names[i]; i++)
		if (!strcmp(name, idle_names[i]))
			return (true);
	for (p = user_idle; p && *p; p += strcspn(p, ","), p += (*p == ','))
		if ((strcspn(p, ",") == n) && !strncmp(p, name, n))
			return (true);
	return (false);
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
		buckets[h].idle = IdleName(name);
	}
	return (&buckets[h]);
}

static void	Sample(uint8_t *p)
{
	uint32_t	pc = Get32(p + 2), ra = Get32(p + 6), sp = 0x80000000 | (Get32(p + 10) & 0x1FFFFF);
	uint8_t		f = p[1];
	Bucket		*b = BucketFor(pc);
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
	if (b->idle || BucketFor(ra)->idle)
	{
		period.idle++;
		total.idle++;
	}
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

static int	VoicesPlaying(void)
{
	int	i, n = 0;

	for (i = 0; i < 24; i++)
		if (voice_env[i])
			n++;
	return (n);
}

// Initial stack pointer from the executable header, or the highest one seen (PSn00bSDK executables leave it to 0)
static uint32_t	StackTop(ProfOptions *o)
{
	return (o->stack_top ? o->stack_top : total.sp_max);
}

static void	NewLine(void)
{
	if (last_char != '\n')
		putchar('\n');
	last_char = '\n';
}

static void	Report(ProfOptions *o, double t, double dt, FILE *csv)
{
	Bucket		**list;
	int			n, i;
	uint32_t	s = period.samples;
	double		cpu = s ? 100.0 - Pct(period.idle, s) : 0.0, gpu = Pct(period.flags[0], s);
	double		fps = dt > 0 ? period.frames / dt : 0.0, rate = dt > 0 ? (s + period.dropped) / dt : 0.0;
	uint32_t	top = StackTop(o), stack = (top && (period.sp_min < top)) ? top - period.sp_min : 0;
	bool		known = symbol_count != 0;

	if (json)
	{
		NewLine();
		printf("PROF {\"t\":%.1f,\"rate\":%.0f,\"samples\":%u,\"dropped\":%u,\"symbols\":%s,", t, rate, s, period.dropped,
			known ? "true" : "false");
		printf("\"cpu\":%.1f,\"gpu\":%.1f,\"gpu_cmd\":%.1f,\"fps\":%.1f,", cpu, gpu, Pct(period.flags[6], s), fps);
		printf("\"dma\":{\"gpu\":%.1f,\"spu\":%.1f,\"cd\":%.1f,\"mdec\":%.1f,\"otc\":%.1f},\"cd\":%.1f,",
			Pct(period.flags[1], s), Pct(period.flags[2], s), Pct(period.flags[3], s), Pct(period.flags[4], s),
			Pct(period.flags[5], s), Pct(period.flags[7], s));
		if (period.sp_min != 0xFFFFFFFF)
			printf("\"sp\":%u,\"stack\":%u,\"stack_top\":%u,", period.sp_min, stack, top);
		if (have_ram)
			printf("\"ram\":{\"lo\":%u,\"hi\":%u,\"used\":%u,\"gap\":%u},", ram_lo, ram_hi, ram_dirty * 4, ram_maxrun * 4);
		if (have_voices)
		{
			printf("\"spu\":{\"on\":%s,\"playing\":%d,\"endx\":%u,\"env\":[", (spu_cnt & 0x8000) ? "true" : "false",
				VoicesPlaying(), voice_endx);
			for (i = 0; i < 24; i++)
				printf(i ? ",%u" : "%u", voice_env[i]);
			printf("]},");
		}
		printf("\"recent\":[");
		n = SortedBuckets(&list, true);
		for (i = 0; (i < n) && (i < TOP_RECENT); i++)
		{
			printf(i ? ",[" : "[");
			JsonString(stdout, list[i]->name);
			printf(",%u,%.1f,%d]", list[i]->period, Pct(list[i]->period, s), list[i]->idle);
		}
		printf("],\"top\":[");
		n = SortedBuckets(&list, false);
		for (i = 0; (i < n) && (i < TOP_TOTAL); i++)
		{
			printf(i ? ",[" : "[");
			JsonString(stdout, list[i]->name);
			printf(",%u,%.1f,%d]", list[i]->total, Pct(list[i]->total, total.samples), list[i]->idle);
		}
		printf("],\"total\":%u}\n", total.samples);
	}
	else
	{
		NewLine();
		printf("%5.0fs  CPU %3.0f%%  GPU %3.0f%%  %4.1f fps", t, cpu, gpu, fps);
		if (have_ram)
			printf("  RAM %u/%u KB", ram_dirty * 4 / 1024, (ram_hi - ram_lo) / 1024);
		if (stack)
			printf("  stack %u B", stack);
		if (have_voices)
			printf("  SPU %d voices", VoicesPlaying());
		if (period.dropped)
			printf("  (%u samples dropped)", period.dropped);
		n = SortedBuckets(&list, true);
		printf("  |");
		for (i = 0; (i < n) && (i < 3); i++)
			printf(" %s %.0f%%", list[i]->name, Pct(list[i]->period, s));
		printf("\n");
	}
	if (csv)
	{
		n = SortedBuckets(&list, true);
		fprintf(csv, "%.1f,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,", t, s, period.dropped, cpu, gpu, fps,
			Pct(period.flags[1], s), Pct(period.flags[2], s), Pct(period.flags[3], s), Pct(period.flags[4], s),
			Pct(period.flags[5], s), Pct(period.flags[7], s));
		fprintf(csv, have_ram ? "%u,%u," : ",,", ram_dirty * 4, ram_maxrun * 4);
		fprintf(csv, (period.sp_min != 0xFFFFFFFF) ? "0x%08X," : ",", period.sp_min);
		fprintf(csv, have_voices ? "%d," : ",", VoicesPlaying());
		for (i = 0; i < 3; i++)
			if (i < n)
				fprintf(csv, "\"%s\",%.1f%s", list[i]->name, Pct(list[i]->period, s), (i < 2) ? "," : "\n");
			else
				fprintf(csv, (i < 2) ? ",," : ",\n");
		fflush(csv);
	}

	for (i = 0; i < BUCKETS; i++)
		buckets[i].period = 0;
	memset(&period, 0, sizeof(period));
	period.sp_min = 0xFFFFFFFF;
}

static void	FinalReport(ProfOptions *o)
{
	Bucket	**list;
	int		n, i;

	if (json || !total.samples)
		return;
	NewLine();
	printf("\n%u samples, %u dropped. CPU %.0f%% (waiting %.0f%%), GPU busy %.0f%%\n", total.samples, total.dropped,
		100.0 - Pct(total.idle, total.samples), Pct(total.idle, total.samples), Pct(total.flags[0], total.samples));
	if ((total.sp_min != 0xFFFFFFFF) && StackTop(o) && (total.sp_min < StackTop(o)))
		printf("Deepest stack: %u bytes (SP 0x%08X, top 0x%08X%s)\n", StackTop(o) - total.sp_min, total.sp_min,
			StackTop(o), o->stack_top ? "" : ": highest SP seen");
	if (!symbol_count)
		printf("No symbols: addresses grouped by 64 bytes (give the .map/.elf with -sym, or run the program with -e)\n");
	printf("\n  Samples      %%  Function\n");
	n = SortedBuckets(&list, false);
	for (i = 0; (i < n) && (i < 30); i++)
		printf("%9u  %5.1f  %s%s\n", list[i]->total, Pct(list[i]->total, total.samples), list[i]->name,
			list[i]->idle ? "  (waiting)" : "");
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
	double			start, last;
	uint32_t		ticks = (o->counter == 2) ? SYSCLOCK / 8 / o->rate : SYSCLOCK / o->rate;
	int				ret = 1;
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
	ram_lo = ms.ram_lo;
	ram_hi = ms.ram_hi;
	AutoLoadSymbols(o->symfile);
	if (o->csv && !(csv = fopen(o->csv, "w")))
	{
		printf("Can't write %s\n", o->csv);
		return (1);
	}
	if (csv)
		fprintf(csv, "time,samples,dropped,cpu,gpu,fps,dma_gpu,dma_spu,dma_cd,dma_mdec,dma_otc,cd_busy,ram_used,"
			"ram_gap,sp_min,spu_voices,top1,top1_pct,top2,top2_pct,top3,top3_pct\n");

	memset(buckets, 0, sizeof(buckets));
	memset(&period, 0, sizeof(period));
	memset(&total, 0, sizeof(total));
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
	if (ram_hi)
		printf("RAM use measured from 0x%08X to 0x%08X (%u KB free when the program started)\n", ram_lo, ram_hi,
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
	FinalReport(o);
	if (csv)
		fclose(csv);
	return (ret);
}
