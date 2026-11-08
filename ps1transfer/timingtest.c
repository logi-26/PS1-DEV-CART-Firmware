/*
* PS1 DEV CART Transfer tool: bus timing test of the cartridge port ("-timingtest [REPORT]")
*
* USB errors ("retry...") on some console revisions (SCPH-7502...) could come from the cartridge port's bus timing,
* which the BIOS sets at boot and the firmware keeps. This test answers it in one run, without changing the firmware:
* 1. at the menu: the console's BIOS read twice and test data written and read back (the firmware's own transfers);
* 2. the test program (ps1-tools/timingtest) started: it reports the console (BIOS, CPU, GPU, the bus timings set at
*    boot), then sets each candidate timing in turn; for each one, test data is read (1MB) and written (512KB) through
*    the kernel hook and every error is looked at: bytes damaged (which bits), a byte too many (written twice), a byte
*    missing, the CRC byte only, data wrong but accepted;
* 3. the console's timing and the busiest ones again with DMA traffic (GPU and SPU transfers all the time, as a game);
* 4. the report: a table of the timings, the errors seen, and the timings with no errors.
* A timing that breaks the link is set back by the test program by itself (no heartbeat from the computer: watchdog),
* then the test goes on with the next one. The console is reset to the menu at the end.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>

#include "timlive.h"
#include "../ps1-tools/timingtest/timingtest.h"

#define	TT_FILENAME			"timingtest.ps-exe"
#define	TT_FIELD(_f_)		(TT_MAILBOX_ADRS + offsetof(TtMailbox, _f_))
#define	EXEC_TTY_MODE_BIOS	1
#define	CHUNK				0x10000		// Transfers of 64KB (the heartbeat written between them)
#define	DOWN_PASSES			4			// 1MB read per timing
#define	UP_PASSES			2			// 512KB written per timing
#define	MAX_EXAMPLES		6
#define	ERRORS_ENOUGH		400			// Packets asked again: the timing is bad enough, the next one tested
#define	BIOS_SIZE			0x80000
#define	MENU_TEST_ADRS		0x80100000	// (free RAM at the menu)

// Provided by main.c
void	*SystemGetFile(char *filename, int *sizeptr);
char	*GetToolPath(char *argv0, char *name);
void	USB_PurgeNow(void);
void	USB_ChipText(char *out, int size);
bool	USB_GetSerial(char *serial);
void	PrintInfoTo(FILE *out);
uint32_t	USB_NowMs(void);
#define	NowMs()		USB_NowMs()
extern bool		usb_test_mode;
extern uint32_t	usb_retry_count;
extern uint32_t	hook_packet_size;
extern void		(*usb_test_packet)(char cmd, uint32_t offset, const uint8_t *got, uint32_t got_n, uint32_t want_n,
								   uint32_t extra);

// Candidate timings: the console's own (bits of EXP1 delay/size cleared then set, bits of COM_DELAY set)
typedef struct
{
	const char	*name;
	uint32_t	exp1_clear, exp1_set, com_set;
	bool		reference;		// (faster than the console's: shows the margin, never recommended)
} Candidate;

static const Candidate	candidates[] =
{
	{"console's own timing (BIOS)",					0,		0,		0,		false},
	{"read delay 7 cycles",							0xF0,	0x70,	0,		false},
	{"read delay 15 cycles",						0xF0,	0xF0,	0,		false},
	{"hold period (COM1)",							0,		0x200,	0,		false},
	{"hold period, COM1 15",						0,		0x200,	0x00F0,	false},
	{"recovery period (COM0)",						0,		0x100,	0,		false},
	{"pre-strobe period (COM3)",					0,		0x800,	0,		false},
	{"no float period",								0x400,	0,		0,		false},
	{"hold + recovery + pre-strobe, read 15",		0xF0,	0xBF0,	0,		false},
	{"the same, COM delays all 15",					0xF0,	0xBF0,	0xFFFF,	false},
	{"write delay 7 cycles (faster: reference)",	0x0F,	0x07,	0,		true},
};
#define	N_CANDIDATES	((int)(sizeof(candidates) / sizeof(candidates[0])))

typedef struct
{
	char		name[64];
	uint32_t	exp1, com;
	bool		load;
	bool		tested, not_set, link_lost, stopped_early;
	uint32_t	down_bytes, up_bytes, down_ms, up_ms;
	uint32_t	load_loops;			// DMA transfers the console made meanwhile (load)
	uint32_t	retries;			// Packets asked again
	uint32_t	failed;				// Transfers given up (the packet asked again too often, no answer)
	uint32_t	pk_bits, pk_extra, pk_short, pk_crc, pk_up, pk_other;
	uint32_t	bytes_damaged;
	uint32_t	silent_down, silent_up;		// Bytes wrong in data accepted (the CRC didn't see it)
	uint32_t	bit_up[8], bit_down[8];		// Bits damaged: 0 read as 1, 1 read as 0
	char		examples[MAX_EXAMPLES][200];
	int			n_examples;
} Result;

static FILE			*report;
static TtMailbox	info;
static uint32_t		boot_exp1, boot_com;
static uint32_t		seq, beat;
static Result		*cur;				// The result counted by the packet callback
static const uint8_t	*cur_expected;	// What the transfer should give (NULL: unknown, e.g. the BIOS)
static uint32_t		cur_expected_size;

static void	Out(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (report)
	{
		va_start(ap, fmt);
		vfprintf(report, fmt, ap);
		va_end(ap);
		fflush(report);
	}
	fflush(stdout);
}

static uint32_t	Crc32(const uint8_t *p, uint32_t n)
{
	uint32_t	crc = 0xFFFFFFFF;
	int			k;

	while (n--)
	{
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
	}
	return (~crc);
}

static void	Hex(char *out, int size, const uint8_t *p, int n)
{
	int	i, o = 0;

	out[0] = 0;
	for (i = 0; (i < n) && (o + 4 < size); i++)
		o += snprintf(out + o, size - o, "%s%02X", i ? " " : "", p[i]);
}

static void	Example(Result *r, const char *fmt, ...)
{
	va_list	ap;

	if (r->n_examples >= MAX_EXAMPLES)
		return;
	va_start(ap, fmt);
	vsnprintf(r->examples[r->n_examples++], sizeof(r->examples[0]), fmt, ap);
	va_end(ap);
}

// A packet that failed (main.c's test mode): what arrived compared with what was sent
static void	PacketFailed(char cmd, uint32_t offset, const uint8_t *got, uint32_t got_n, uint32_t want_n, uint32_t extra)
{
	Result			*r = cur;
	const uint8_t	*exp;
	uint32_t		size = want_n - 1, i, k, n, diffs = 0, cmp;

	(void)cmd;
	char			e_hex[64], g_hex[64];

	if (!r)
		return;
	if (!got)		// Upload: the console's CRC wasn't the one of the data sent (nothing more is known here)
	{
		r->pk_up++;
		return;
	}
	if (!cur_expected || (offset + size > cur_expected_size))
	{
		// Unknown data (the BIOS): only the length tells
		if (got_n < want_n)
			r->pk_short++;
		else if (got_n > want_n)
			r->pk_extra++;
		else
			r->pk_other++;
		Example(r, "packet at +0x%X: %u bytes received for %u sent (data + CRC)", (unsigned)offset, (unsigned)got_n,
				(unsigned)want_n);
		return;
	}
	exp = cur_expected + offset;
	cmp = (got_n < size) ? got_n : size;
	for (k = 0; (k < cmp) && (got[k] == exp[k]); k++)
		;
	Hex(e_hex, sizeof(e_hex), exp + ((k >= 4) ? k - 4 : 0), ((k >= 4) ? 4 : k) + ((k + 4 <= size) ? 4 : size - k));
	Hex(g_hex, sizeof(g_hex), got + ((k >= 4) ? k - 4 : 0), ((k >= 4) ? 4 : k) + ((k + 4 <= got_n) ? 4 : got_n - k));

	if (got_n > want_n)
	{
		// A byte too many: the data from k on is one byte late
		bool	shifted = (k + 17 <= size) && (k + 18 <= got_n) && !memcmp(got + k + 1, exp + k, 16);

		r->pk_extra++;
		Example(r, "%u byte(s) too many in a %u-byte packet; first difference at byte %u%s%s: sent [%s] got [%s]",
				(unsigned)extra, (unsigned)size, (unsigned)k,
				shifted ? ", a byte inserted there" : "",
				(shifted && k && (got[k] == exp[k - 1])) ? " (the byte before, written twice)" : "", e_hex, g_hex);
		return;
	}
	if (got_n < want_n)
	{
		bool	lost = (k + 17 <= size) && (k + 16 <= got_n) && !memcmp(got + k, exp + k + 1, 16);

		r->pk_short++;
		Example(r, "%u byte(s) missing in a %u-byte packet; first difference at byte %u%s: sent [%s] got [%s]",
				(unsigned)(want_n - got_n), (unsigned)size, (unsigned)k, lost ? ", the byte there lost" : "", e_hex, g_hex);
		return;
	}
	// The right length: bytes damaged, or only the CRC byte
	for (i = 0; i < size; i++)
	{
		uint8_t	x = got[i] ^ exp[i];
		int		b;

		if (!x)
			continue;
		diffs++;
		for (b = 0; b < 8; b++)
			if (x & (1 << b))
			{
				if (got[i] & (1 << b))
					r->bit_up[b]++;
				else
					r->bit_down[b]++;
			}
	}
	if (!diffs)
	{
		r->pk_crc++;
		Example(r, "only the CRC byte wrong in a %u-byte packet: got %02X", (unsigned)size, got[size]);
		return;
	}
	r->pk_bits++;
	r->bytes_damaged += diffs;
	n = 0;
	for (i = 0; i < size; i++)
		if (got[i] != exp[i])
		{
			if (!n)
				Example(r, "%u byte(s) damaged in a %u-byte packet, the first at byte %u: sent %02X got %02X "
						"(around it: sent [%s] got [%s])", (unsigned)diffs, (unsigned)size, (unsigned)i, exp[i], got[i],
						e_hex, g_hex);
			n++;
		}
}

/****************************************/
// The test program

static bool	Read(uint32_t adrs, void *p, uint32_t n)	{return (USB_Process('D', (uint8_t *)p, adrs, n));}
static bool	Write(uint32_t adrs, const void *p, uint32_t n)	{return (USB_Process('U', (uint8_t *)p, adrs, n));}

static bool	Heartbeat(void)
{
	beat++;
	return (Write(TT_FIELD(heartbeat), &beat, 4));
}

// A command of the test program, its result in *result (false: no answer)
static bool	Command(uint32_t cmd, uint32_t arg0, uint32_t arg1, int32_t *result)
{
	uint32_t	f[3] = {cmd, arg0, arg1}, done = 0, start;

	seq++;
	if (!Write(TT_FIELD(cmd), f, sizeof(f)) || !Write(TT_FIELD(cmd_seq), &seq, 4))
		return (false);
	for (start = NowMs(); ; LiveSleepMs(10))
	{
		if (Read(TT_FIELD(done_seq), &done, 4) && (done == seq))
			break;
		if (NowMs() - start > 5000)
			return (false);
	}
	return (!result || Read(TT_FIELD(result), result, 4));
}

static bool	StartProgram(char *argv0)
{
	uint8_t		*exe;
	uint32_t	zero = 0, magic = 0;
	int			size, i;
	bool		ok;

	if (!(exe = SystemGetFile(GetToolPath(argv0, TT_FILENAME), &size)))
		return (false);
	ok = Write(TT_FIELD(magic), &zero, 4) && Write(*(uint32_t *)&exe[0x18], exe + 2048, size - 2048) &&
		USB_Process('E', exe + 16, EXEC_TTY_MODE_BIOS, 15 * 4);
	free(exe);
	if (!ok)
		return (false);
	ps1_program_running = true;
	usb_big_packet_size = 0;
	for (i = 0; i < 50; i++)
	{
		LiveSleepMs(100);
		if (Read(TT_FIELD(magic), &magic, 4) && (magic == TT_MAGIC))
			return (Read(TT_FIELD(done_seq), &seq, 4));
	}
	return (false);
}

// After a transfer that failed: the console given time to stop sending, then asked again. False: no answer (the test
// program set its own timing back if the link was broken, *reverted)
static bool	Recover(uint32_t *reverted)
{
	int	i;

	for (i = 0; i < 12; i++)
	{
		LiveSleepMs(i ? 1000 : 3000);	// (the console waits about 2 seconds for an acknowledgment)
		USB_PurgeNow();
		if (Read(TT_FIELD(reverted), reverted, 4))
		{
			Heartbeat();
			return (true);
		}
	}
	return (false);
}

// Test data read (passes of the buffer, each filled by the console with another pattern) or written (then checked by
// the console). False: the console doesn't answer any more.
static bool	Transfers(Result *r, bool down, int passes, uint32_t seed0, uint8_t *exp, uint8_t *buf, uint32_t reverted0)
{
	int			pass;
	uint32_t	off, i, t0, reverted = reverted0;
	int32_t		result;

	for (pass = 0; pass < passes; pass++)
	{
		uint32_t	seed = seed0 + pass;

		TtPattern(exp, TT_BUFFER_SIZE, seed);
		cur_expected = exp;
		cur_expected_size = TT_BUFFER_SIZE;
		if (down && !Command(TT_FILL, seed, 0, NULL))
			goto lost;
		t0 = NowMs();
		for (off = 0; off < TT_BUFFER_SIZE; off += CHUNK)
		{
			cur_expected = exp + off;
			cur_expected_size = TT_BUFFER_SIZE - off;
			if (down ? Read(TT_BUFFER_ADRS + off, buf + off, CHUNK) : Write(TT_BUFFER_ADRS + off, exp + off, CHUNK))
			{
				if (down)
				{
					for (i = 0; i < CHUNK; i++)
						r->silent_down += (buf[off + i] != exp[off + i]);
					r->down_bytes += CHUNK;
				}
				else
					r->up_bytes += CHUNK;
			}
			else
			{
				r->failed++;
				if (!Recover(&reverted))
					goto lost;
				if (reverted != reverted0)
				{
					r->link_lost = true;
					return (true);
				}
			}
			if (!Heartbeat())
			{
				r->failed++;
				if (!Recover(&reverted))
					goto lost;
				if (reverted != reverted0)
				{
					r->link_lost = true;
					return (true);
				}
			}
			if (usb_retry_count > ERRORS_ENOUGH)
			{
				r->stopped_early = true;
				break;
			}
		}
		if (down)
			r->down_ms += NowMs() - t0;
		else
		{
			r->up_ms += NowMs() - t0;
			cur_expected = NULL;
			if (!Command(TT_VERIFY, seed, 0, &result))
				goto lost;
			if (result > 0)
			{
				uint32_t	shift = 0;
				TtBadByte	bad;

				r->silent_up += result;
				if (Read(TT_FIELD(verify_shift), &shift, 4) && Read(TT_FIELD(bad[0]), &bad, sizeof(bad)))
					Example(r, "written data wrong after its CRC was accepted: %d byte(s), the first at %u: sent %02X %02X "
							"got %02X %02X%s", (int)result, (unsigned)bad.offset, bad.expected, bad.next_expected, bad.got,
							bad.next_got, (shift == 1) ? " (a byte missing there)" : (shift == 2) ? " (a byte too many there)" : "");
			}
		}
		if (r->stopped_early)
			break;
	}
	cur_expected = NULL;
	return (true);

lost:
	cur_expected = NULL;
	return (false);
}

// One timing tested. False: the console doesn't answer any more (the test stops).
static bool	TestTiming(Result *r, uint32_t exp1, uint32_t com, bool load, uint32_t seed, uint8_t *exp, uint8_t *buf)
{
	uint32_t	now[2] = {0, 0}, reverted = 0, retries0;
	int32_t		result;

	r->exp1 = exp1;
	r->com = com;
	r->load = load;
	r->tested = true;
	Out("  %-46s %08X %08X%s ... ", r->name, (unsigned)exp1, (unsigned)com, load ? " DMA" : "    ");
	if (!Read(TT_FIELD(reverted), &reverted, 4) || !Command(TT_LOAD, load, 0, NULL) ||
		!Command(TT_SET_TIMING, exp1, com, &result))
	{
		r->link_lost = true;
		Out("LINK LOST (timing set back by the console)\n");
		return (Recover(&reverted));
	}
	if (!Read(TT_FIELD(exp1_now), now, 8))
	{
		r->link_lost = true;
		Out("LINK LOST (timing set back by the console)\n");
		return (Recover(&reverted));
	}
	if ((now[0] != exp1) || (now[1] != com))
		r->not_set = true;

	cur = r;
	retries0 = usb_retry_count;
	usb_retry_count = 0;
	if (!Transfers(r, true, DOWN_PASSES, seed, exp, buf, reverted) ||
		(!r->link_lost && !r->stopped_early && !Transfers(r, false, UP_PASSES, seed + 100, exp, buf, reverted)))
	{
		cur = NULL;
		r->retries = usb_retry_count;
		r->link_lost = true;
		Out("no answer from the console\n");
		return (false);
	}
	cur = NULL;
	r->retries = usb_retry_count;
	usb_retry_count += retries0;
	if (load)
		Read(TT_FIELD(load_loops), &r->load_loops, 4);
	Out("%s\n", r->link_lost ? "LINK LOST (timing set back by the console)" :
		(r->retries || r->failed || r->silent_down || r->silent_up) ? "ERRORS" : "ok");
	// The console's own timing back before the next one (and if the link was lost: it is already)
	Command(TT_SET_TIMING, 0, 0, NULL);
	Command(TT_LOAD, 0, 0, NULL);
	return (true);
}

static bool	Clean(const Result *r)
{
	return (r->tested && !r->link_lost && !r->stopped_early && !r->not_set && !r->retries && !r->failed &&
			!r->silent_down && !r->silent_up && r->down_bytes && r->up_bytes);
}

static int	FirstClean(const Result *results)
{
	int	i;

	for (i = 1; i < N_CANDIDATES; i++)
		if (Clean(&results[i]))
			return (i);
	return (-1);
}

static void	PrintResult(const Result *r)
{
	int	b, i;

	if (!r->tested)
		return;
	Out("\n%s: EXP1 %08X COM %08X", r->name, (unsigned)r->exp1, (unsigned)r->com);
	if (r->load)
		Out(", with DMA traffic (%u GPU+SPU transfers)", (unsigned)r->load_loops);
	Out("\n");
	if (r->not_set)
		Out("  the registers didn't keep these values\n");
	Out("  read %u KB in %u ms, written %u KB in %u ms\n", (unsigned)(r->down_bytes >> 10), (unsigned)r->down_ms,
		(unsigned)(r->up_bytes >> 10), (unsigned)r->up_ms);
	Out("  packets asked again %u, transfers given up %u%s%s\n", (unsigned)r->retries, (unsigned)r->failed,
		r->link_lost ? ", LINK LOST" : "", r->stopped_early ? ", stopped early (too many errors)" : "");
	if (r->retries || r->silent_down || r->silent_up)
	{
		Out("  read packets: %u with bytes damaged (%u bytes), %u with a byte too many, %u with bytes missing, "
			"%u CRC byte only, %u other\n", (unsigned)r->pk_bits, (unsigned)r->bytes_damaged, (unsigned)r->pk_extra,
			(unsigned)r->pk_short, (unsigned)r->pk_crc, (unsigned)r->pk_other);
		Out("  written packets with a wrong CRC: %u\n", (unsigned)r->pk_up);
		Out("  wrong bytes in data accepted: read %u, written %u\n", (unsigned)r->silent_down, (unsigned)r->silent_up);
	}
	if (r->pk_bits)
	{
		Out("  damaged bits (D0..D7), 0 read as 1:");
		for (b = 0; b < 8; b++)
			Out(" %u", (unsigned)r->bit_up[b]);
		Out("; 1 read as 0:");
		for (b = 0; b < 8; b++)
			Out(" %u", (unsigned)r->bit_down[b]);
		Out("\n");
	}
	for (i = 0; i < r->n_examples; i++)
		Out("  - %s\n", r->examples[i]);
}

/****************************************/

int	TimingTest(char *argv0, const char *report_path)
{
	static Result	menu_bios, menu_data, results[N_CANDIDATES + 11];
	uint8_t			*bios[2], *exp, *buf;
	char			text[256];
	time_t			now = time(NULL);
	int				i, n_results = 0, best = -1, alive = 1, again = -1;
	uint32_t		mism;

	if (!(report = fopen(report_path, "w")))
	{
		printf("Can't write %s\n", report_path);
		return (EXIT_FAILURE);
	}
	bios[0] = malloc(BIOS_SIZE);
	bios[1] = malloc(BIOS_SIZE);
	exp = malloc(TT_BUFFER_SIZE);
	buf = malloc(TT_BUFFER_SIZE);
	if (!bios[0] || !bios[1] || !exp || !buf)
		return (EXIT_FAILURE);

	Out("PS1 DEV CART bus timing test (ps1transfer -timingtest), %s", ctime(&now));
	Out("Send this whole file back. It takes a few minutes; nothing is written to the cartridge's flash.\n\n");
	USB_GetSerial(text);
	Out("Cartridge USB serial: %s\n", text);
	USB_ChipText(text, sizeof(text));
	Out("USB chip: %s\n", text);
	Out("ps1transfer -info at the menu:\n");
	PrintInfoTo(stdout);
	PrintInfoTo(report);

	usb_test_mode = true;
	usb_test_packet = PacketFailed;

	// 1. The firmware's own transfers at the menu
	Out("\n1. Transfers of the menu (firmware, the console's own timing)\n");
	strcpy(menu_bios.name, "menu: BIOS read twice");
	strcpy(menu_data.name, "menu: test data written then read");
	menu_bios.tested = menu_data.tested = true;
	cur = &menu_bios;
	usb_retry_count = 0;
	for (i = 0; i < 2; i++)
	{
		uint32_t	t0 = NowMs();

		if (!Read(0xBFC00000, bios[i], BIOS_SIZE))
		{
			menu_bios.failed++;
			USB_PurgeNow();
			LiveSleepMs(3000);
			USB_PurgeNow();
			memset(bios[i], 0, BIOS_SIZE);
		}
		else
		{
			menu_bios.down_bytes += BIOS_SIZE;
			menu_bios.down_ms += NowMs() - t0;
		}
	}
	menu_bios.retries = usb_retry_count;
	for (i = 0, mism = 0; i < BIOS_SIZE; i++)
		mism += (bios[0][i] != bios[1][i]);
	menu_bios.silent_down = mism;
	Out("  BIOS: CRC32 %08X and %08X (%s), %u packets asked again, %u reads given up\n", (unsigned)Crc32(bios[0], BIOS_SIZE),
		(unsigned)Crc32(bios[1], BIOS_SIZE), mism ? "the two reads differ" : "the same", (unsigned)menu_bios.retries,
		(unsigned)menu_bios.failed);

	cur = &menu_data;
	usb_retry_count = 0;
	TtPattern(exp, TT_BUFFER_SIZE, 77);
	{
		uint32_t	t0 = NowMs();

		if (Write(MENU_TEST_ADRS, exp, TT_BUFFER_SIZE))
		{
			menu_data.up_bytes = TT_BUFFER_SIZE;
			menu_data.up_ms = NowMs() - t0;
		}
		else
			menu_data.failed++;
		cur_expected = exp;
		cur_expected_size = TT_BUFFER_SIZE;
		t0 = NowMs();
		if (Read(MENU_TEST_ADRS, buf, TT_BUFFER_SIZE))
		{
			menu_data.down_bytes = TT_BUFFER_SIZE;
			menu_data.down_ms = NowMs() - t0;
			for (i = 0; i < TT_BUFFER_SIZE; i++)
				menu_data.silent_down += (buf[i] != exp[i]);	// (written or read wrong)
		}
		else
			menu_data.failed++;
		cur_expected = NULL;
	}
	menu_data.retries = usb_retry_count;
	Out("  test data: %u packets asked again, %u transfers given up, %u bytes wrong after\n", (unsigned)menu_data.retries,
		(unsigned)menu_data.failed, (unsigned)menu_data.silent_down);
	cur = NULL;
	if (menu_data.failed || menu_bios.failed)
	{
		USB_PurgeNow();
		LiveSleepMs(3000);
		USB_PurgeNow();
	}

	// 2. The test program
	Out("\n2. The console (test program)\n");
	if (!StartProgram(argv0) || !Read(TT_MAILBOX_ADRS, &info, sizeof(info)))
	{
		Out("The test program didn't start: the report ends here.\n");
		goto end;
	}
	boot_exp1 = info.memctrl[2];
	boot_com = info.memctrl[8];
	info.bios_version[sizeof(info.bios_version) - 1] = 0;
	Out("  BIOS \"%s\", date %08X, region byte '%c'\n", info.bios_version, (unsigned)info.bios_date,
		(bios[0][0x7FF52] >= 32) ? bios[0][0x7FF52] : '?');
	Out("  CPU PRId %08X, GPU version %08X, RAM size register %08X\n", (unsigned)info.cop0_prid,
		(unsigned)info.gpu_version, (unsigned)info.ram_size);
	Out("  memory control: EXP1 base %08X, EXP2 base %08X, EXP1 delay %08X, EXP3 %08X, BIOS %08X, SPU %08X, CD %08X, "
		"EXP2 delay %08X, COM_DELAY %08X\n", (unsigned)info.memctrl[0], (unsigned)info.memctrl[1],
		(unsigned)info.memctrl[2], (unsigned)info.memctrl[3], (unsigned)info.memctrl[4], (unsigned)info.memctrl[5],
		(unsigned)info.memctrl[6], (unsigned)info.memctrl[7], (unsigned)info.memctrl[8]);
	Out("  kernel hook packets: %u bytes\n", (unsigned)hook_packet_size);

	// 3. Each timing
	Out("\n3. Each timing of the cartridge port (EXP1 delay/size, COM_DELAY); 1MB read, 512KB written each\n");
	for (i = 0; (i < N_CANDIDATES) && alive; i++)
	{
		Result	*r = &results[n_results++];

		snprintf(r->name, sizeof(r->name), "%s", candidates[i].name);
		alive = TestTiming(r, (boot_exp1 & ~candidates[i].exp1_clear) | candidates[i].exp1_set,
						   boot_com | candidates[i].com_set, false, 1000 + i * 10, exp, buf);
	}
	// Timings asked for besides (TIMINGTEST_EXTRA="EXP1:COM EXP1:COM ...", hexadecimal: a fix to check)
	{
		const char	*extra = getenv("TIMINGTEST_EXTRA");
		unsigned	e, c;
		int			used;

		while (extra && alive && (n_results < N_CANDIDATES + 8) && (sscanf(extra, " %x:%x%n", &e, &c, &used) == 2))
		{
			Result	*r = &results[n_results++];

			snprintf(r->name, sizeof(r->name), "asked: %08X %08X", e, c);
			alive = TestTiming(r, e, c, false, 5000 + n_results, exp, buf);
			extra += used;
		}
	}
	// The console's own timing again (errors that come and go), then with DMA traffic, and the best one with it
	if (alive)
	{
		Result	*r = &results[n_results++];

		again = n_results - 1;
		strcpy(r->name, "console's own timing, again");
		alive = TestTiming(r, boot_exp1, boot_com, false, 2000, exp, buf);
	}
	if (alive)
	{
		Result	*r = &results[n_results++];

		strcpy(r->name, "console's own timing, DMA traffic");
		alive = TestTiming(r, boot_exp1, boot_com, true, 3000, exp, buf);
	}
	for (i = 1; i < N_CANDIDATES; i++)
		if (!candidates[i].reference && Clean(&results[i]))
		{
			best = i;
			break;
		}
	if (alive && (best > 0) && !Clean(&results[0]))
	{
		Result	*r = &results[n_results++];

		snprintf(r->name, sizeof(r->name), "%s, DMA traffic", candidates[best].name);
		alive = TestTiming(r, results[best].exp1, results[best].com, true, 4000, exp, buf);
	}

end:
	usb_test_mode = false;
	usb_test_packet = NULL;

	// 4. The report
	Out("\n4. Details\n");
	PrintResult(&menu_bios);
	PrintResult(&menu_data);
	for (i = 0; i < n_results; i++)
		PrintResult(&results[i]);

	Out("\nSUMMARY\n");
	{
		bool	own_clean = n_results && Clean(&results[0]) && ((again < 0) || Clean(&results[again])) &&
							!menu_bios.retries && !menu_data.retries && !menu_bios.silent_down && !menu_data.silent_down;

		if (!n_results)
			Out("The test didn't run (see above).\n");
		else if (own_clean)
			Out("No USB errors with the console's own timing: the problem isn't the bus timing, or it didn't show "
				"in this run.\n");
		else
		{
			Out("USB errors with the console's own timing.\n");
			for (i = 1; i < N_CANDIDATES; i++)
				if (Clean(&results[i]))
					Out("%s  %-46s EXP1 %08X COM %08X%s\n", (i == FirstClean(results)) ? "Timings with no errors at all:\n" : "",
						results[i].name, (unsigned)results[i].exp1,
						(unsigned)results[i].com, candidates[i].reference ? " (reference only)" : "");
			if (best > 0)
				Out("Suggested: %s (EXP1 %08X, COM_DELAY %08X)\n", results[best].name, (unsigned)results[best].exp1,
					(unsigned)results[best].com);
			else
				Out("None of the timings removed the errors.\n");
		}
		if (!alive)
			Out("The console stopped answering during the test: reset it.\n");
	}
	Out("\nReport written to %s\n", report_path);
	fclose(report);
	report = NULL;

	// The console back at the menu
	if (alive && ps1_program_running)
	{
		printf("Resetting the PS1...\n");
		USB_Process('R', NULL, 0, 0);
	}
	free(bios[0]);
	free(bios[1]);
	free(exp);
	free(buf);
	return (EXIT_SUCCESS);
}
