/*
* PS1 DEV CART Transfer tool: files of the disc in the PS1's drive ("-disc", the disc reader ps1-tools/discread)
*
* The disc reader runs on the PS1 (started if it isn't running) and reads the sectors asked through its mailbox
* (discserver.h); the file system is read here: the ISO 9660 directories, each file's XA attributes (Form 2 sectors:
* XA audio, STR movies). Only what is asked is read: listing a disc reads its directories, a file its sectors.
*
* "-disc COMMAND ..." runs one command; "-disc" alone reads commands on the standard input (PS1 Dev Cart Manager):
*	init					the disc identified: "DISCID IRQ HEX" (GetID: IRQ 2 licensed, 5 unlicensed or audio CD),
*							"DISCVOL NAME" (ISO volume, if a data disc)
*	toc						"DISCTRACK N LBA SECTORS" for each track (track 1: the data track), then "DISCTOC N"
*	ls						"DISCFILE LBA SIZE ATTR PATH" for each file (ATTR: XA attributes, hexadecimal: 1000 Form 2,
*							2000 interleaved, 4000 CD audio), "DISCDIR PATH" for each directory, then "DISCLS N"
*	get PATH OUT			a file written to OUT (the rest of the line: spaces allowed): whole 2336-byte sectors for the
*							files with Form 2 sectors (.XA, .STR), the 2048-byte data otherwise ("getxa", "getdata":
*							either one). PATH: "\DIR\FILE.EXT" or "@LBA:BYTES" (sectors of no file)
*	track N SECONDS OUT		CD audio track N written as a WAV file (44100 Hz stereo), its first SECONDS only (0: all):
*							played and taken through the SPU as it plays (Stream: real time), or in parts (CaptureTrack)
*	stop					the drive's motor stopped; while a get or a track runs: it is stopped ("DISCSTOPPED",
*							nothing written)
*	speed 1/2				the read speed (2: default)
*	quit
* Progress: "DISCPROG DONE TOTAL" (sectors), results "DISCSAVED BYTES RETRIES MODE OUT" (MODE: data, xa, audio), errors "DISCERR text", warnings "DISCWARN text".
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

#include "timlive.h"
#include "../ps1-tools/discread/discserver.h"

#define	DISCREAD_FILENAME	"discread.ps-exe"
#define	DS_FIELD(_f_)		(DISCSRV_MAILBOX_ADRS + offsetof(DiscServerMailbox, _f_))
#define	EXEC_TTY_MODE_BIOS	1
#define	XA_FORM2			0x1000
#define	XA_INTERLEAVED		0x2000
#define	XA_CDDA				0x4000
#define	MAX_FILES			8192
#define	MAX_PATH_LEN		256

// Provided by main.c
void	*SystemGetFile(char *filename, int *sizeptr);
char	*GetToolPath(char *argv0, char *name);

typedef struct
{
	uint32_t	lba, size;
	uint16_t	attr;
	bool		dir;
	char		path[MAX_PATH_LEN];
} DiscFile;

static int		read_speed = DISCSRV_DOUBLE;
static bool		live_input;		// Commands on the standard input: "stop" read there while a get or a track runs
static bool		stop_asked;

// "stop" on the input while a long command runs (nothing else is sent then)
static bool	StopAsked(void)
{
	char	*text;
	bool	eof = false;

	if (!stop_asked && live_input && LiveNextLine(&text, &eof) && text && !strncmp(text, "stop", 4))
		stop_asked = true;
	return (stop_asked);
}
static uint32_t	prog_base, prog_total;	// The progress shown while a read runs: sectors before it, in all (0: none)
static clock_t	last_prog;
static uint32_t	last_retries;
static DiscFile	*files;
static int		n_files;

static uint32_t	Le32(const uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}

bool	DiscServerRunning(void)
{
	uint32_t	mv[2] = {0, 0};		// (magic, version: an older disc reader running is started again)

	return (USB_Process('D', (uint8_t *)mv, DS_FIELD(magic), 8) && (mv[0] == DISCSRV_MAGIC) && (mv[1] == DISCSRV_VERSION));
}

// Uploads and executes the disc reader (the PS1 is at the menu)
bool	DiscServerStart(char *argv0)
{
	uint8_t		*exe;
	uint32_t	zero = 0;
	int			size, i;
	bool		ok;

	if (!(exe = SystemGetFile(GetToolPath(argv0, DISCREAD_FILENAME), &size)))
		return (false);
	printf("Starting the disc reader...\n");
	// The magic of a previous run may still be in RAM: cleared, the disc reader sets it once ready
	ok = USB_Process('U', (uint8_t *)&zero, DS_FIELD(magic), 4) &&
		USB_Process('U', exe + 2048, *(uint32_t *)&exe[0x18], size - 2048) &&
		USB_Process('E', exe + 16, EXEC_TTY_MODE_BIOS, 15 * 4);
	free(exe);
	if (!ok)
		return (false);
	ps1_program_running = true;
	usb_big_packet_size = 0;
	for (i = 0; i < 50; i++)
	{
		LiveSleepMs(100);
		if (DiscServerRunning())
			return (true);
	}
	printf("DISCERR The disc reader didn't start.\n");
	return (false);
}

// A command of the disc reader: its result (negative: an error), INT32_MIN if it doesn't answer
static int32_t	Command(uint32_t cmd, uint32_t lba, uint32_t count, uint32_t mode, int timeout_s)
{
	struct
	{
		uint32_t	cmd, lba, count, mode;
	}			f = {cmd, lba, count, mode};
	uint32_t	seq = 0, done = 0;
	int32_t		result = 0;
	time_t		start;

	// The previous command ended first (one given up on may still be running: its fields aren't changed under it)
	for (start = time(NULL); ; LiveSleepMs(20))
	{
		uint32_t	asked = 0;

		if (!USB_Process('D', (uint8_t *)&seq, DS_FIELD(done_seq), 4) ||
			!USB_Process('D', (uint8_t *)&asked, DS_FIELD(cmd_seq), 4))
		{
			if (time(NULL) - start > 3)
				return (INT32_MIN);
			continue;
		}
		if (asked == seq)
			break;
		if (time(NULL) - start > 90)
			return (INT32_MIN);
	}
	if (!USB_Process('U', (uint8_t *)&f, DS_FIELD(cmd), sizeof(f)))
		return (INT32_MIN);
	seq++;
	if (!USB_Process('U', (uint8_t *)&seq, DS_FIELD(cmd_seq), 4))
		return (INT32_MIN);
	// A capture: nothing asked while it runs (each request is served by the kernel hook in an interrupt, for some
	// milliseconds: a half capture buffer could be missed), its end awaited from its length (count frames at 44100 Hz)
	if (cmd == DISCSRV_CAPTURE)
		LiveSleepMs(count / 44 + 300);
	for (start = time(NULL); done != seq; LiveSleepMs((cmd == DISCSRV_CAPTURE) ? 100 : (cmd == DISCSRV_READ) ? 20 : 5))
	{
		if (!USB_Process('D', (uint8_t *)&done, DS_FIELD(done_seq), 4) && (time(NULL) - start > 3))
			return (INT32_MIN);
		if (time(NULL) - start > timeout_s)
			return (INT32_MIN);
		// A long read: its progress (a 4-byte read, served by the kernel hook in a millisecond, 4 times a second)
		if ((cmd == DISCSRV_READ) && prog_total && (done != seq) && (clock() - last_prog > CLOCKS_PER_SEC / 4))
		{
			uint32_t	p = 0;

			last_prog = clock();
			if (USB_Process('D', (uint8_t *)&p, DS_FIELD(progress), 4) && (p <= count))
				printf("DISCPROG %u %u\n", prog_base + p, prog_total);
		}
	}
	if (!USB_Process('D', (uint8_t *)&result, DS_FIELD(result), 4) || !USB_Process('D', (uint8_t *)&last_retries, DS_FIELD(retries), 4))
		return (INT32_MIN);
	return (result);
}

static const char	*ErrorText(int32_t r)
{
	switch (r)
	{
		case DISCSRV_NO_DISC:		return ("no disc, or it can't be read");
		case DISCSRV_READ_ERROR:	return ("read error");
		case DISCSRV_BAD_COMMAND:	return ("command refused by the disc reader");
		case DISCSRV_NO_DRIVE:		return ("the CD drive doesn't answer");
		case INT32_MIN:				return ("the disc reader doesn't answer (USB)");
	}
	return ("error");
}

// count sectors from lba into dst (mode: DISCSRV_DATA/XA/CDDA), *size: bytes per sector. Progress lines if total.
#define	MAX_BAD			20		// Sectors that can't be read at most in a file (then it is given up)

static uint32_t	bad_sectors;		// Sectors that couldn't be read in the last ReadSectors (zeros in their place)

// count sectors from lba into dst (mode: DISCSRV_DATA/XA/CDDA), *size: bytes per sector. Progress lines if total.
// Read in long runs (as many as the disc reader's buffer holds: no seek and pause between them), its progress shown as
// it reads. A sector that can't be read (a scratch): the ones before it kept, it is tried once more alone, then left as
// zeros (a warning) and the reading goes on after it. False if the disc can't be read (no disc, the reader not
// answering, too many bad sectors).
static bool	ReadSectors(uint32_t lba, uint32_t count, int mode, uint8_t *dst, uint32_t *size, uint32_t base, uint32_t total)
{
	uint32_t	ss = (mode == DISCSRV_DATA) ? 2048 : (mode == DISCSRV_XA) ? 2336 : 2352, n, good, err = 0, retries = 0;
	int32_t		r;

	bad_sectors = 0;
	while (count)
	{
		if (total && StopAsked())
		{
			printf("DISCSTOPPED\n");
			return (false);
		}
		n = DISCSRV_BUFFER_SIZE / ss;
		if (n > count)
			n = count;
		prog_base = base;
		prog_total = total;
		r = Command(DISCSRV_READ, lba, n, mode | read_speed, 120);
		prog_total = 0;
		retries += last_retries;
		if (r == (int32_t)n)
			good = n;
		else if ((r == DISCSRV_READ_ERROR) && USB_Process('D', (uint8_t *)&err, DS_FIELD(error_lba), 4) &&
			(err >= lba) && (err < lba + n))
			good = err - lba;
		else
		{
			printf("DISCERR sector %u: %s\n", lba, ErrorText(r));
			return (false);
		}
		if (good && !USB_Process('D', dst, DISCSRV_BUFFER_ADRS, good * ss))
		{
			printf("DISCERR USB error\n");
			return (false);
		}
		dst += good * ss;
		lba += good;
		count -= good;
		base += good;
		if (good < n)	// The sector not read: once more alone, else zeros
		{
			if ((Command(DISCSRV_READ, lba, 1, mode | read_speed, 60) == 1) && USB_Process('D', dst, DISCSRV_BUFFER_ADRS, ss))
				;
			else
			{
				memset(dst, 0, ss);
				printf("DISCWARN sector %u can't be read (a scratch?): zeros in its place\n", lba);
				if (++bad_sectors > MAX_BAD)
				{
					printf("DISCERR more than %d sectors can't be read here: the disc is too scratched or dirty\n", MAX_BAD);
					return (false);
				}
			}
			dst += ss;
			lba++;
			count--;
			base++;
		}
		if (total)
			printf("DISCPROG %u %u\n", base, total);
	}
	last_retries = retries;
	if (size)
		*size = ss;
	return (true);
}

static bool	Init(void)
{
	uint8_t	id[8], pvd[2048];
	int32_t	r = Command(DISCSRV_INIT, 0, 0, 0, 30);
	int		i;

	if ((r == INT32_MIN) || (r == DISCSRV_NO_DRIVE) || (r == DISCSRV_NO_DISC))
	{
		printf("DISCERR %s\n", ErrorText(r));
		return (false);
	}
	if (!USB_Process('D', id, DS_FIELD(id), 8))
		return (false);
	printf("DISCID %d %02X%02X%02X%02X%02X%02X%02X%02X\n", r, id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
	if (!(id[1] & 0x10) && ReadSectors(16, 1, DISCSRV_DATA, pvd, NULL, 0, 0) && !memcmp(pvd + 1, "CD001", 5))
	{
		char	vol[33];

		memcpy(vol, pvd + 40, 32);
		for (i = 32; i && (!vol[i - 1] || (vol[i - 1] == ' ')); i--)
			;
		vol[i] = 0;
		printf("DISCVOL %s\n", vol);
	}
	return (true);
}

typedef struct
{
	int			track;
	uint32_t	lba;
} TocEntry;

static int	ReadToc(TocEntry *toc)
{
	DiscTrack	t[100];
	int32_t		r = Command(DISCSRV_TOC, 0, 0, 0, 10);
	int			i;

	if ((r <= 0) || (r > 99))
	{
		printf("DISCERR TOC: %s\n", ErrorText(r));
		return (0);
	}
	if (!USB_Process('D', (uint8_t *)t, DISCSRV_BUFFER_ADRS, ((r + 1) * sizeof(DiscTrack) + 3) & ~3))
		return (0);
#define	BCD(_b_)	((((_b_) >> 4) * 10) + ((_b_) & 15))
	for (i = 0; i <= r; i++)
	{
		toc[i].track = (t[i].track == 0xAA) ? 0 : BCD(t[i].track);
		toc[i].lba = (BCD(t[i].mm) * 60 + BCD(t[i].ss)) * 75 - 150;
	}
	return (r);
}

static void	Toc(void)
{
	TocEntry	toc[101];
	int			n = ReadToc(toc), i;

	for (i = 0; i < n; i++)
		printf("DISCTRACK %d %u %u\n", toc[i].track, toc[i].lba, toc[i + 1].lba - toc[i].lba);
	if (n)
		printf("DISCTOC %d %u\n", n, toc[n].lba);
}

// The directories from the root (ISO 9660), files[] filled
static bool	ListDir(uint32_t lba, uint32_t size, const char *path, int depth)
{
	uint8_t		*d, *r;
	uint32_t	sectors = (size + 2047) / 2048, off, i;
	int			name_len, su, k;
	bool		ok = true;

	if ((depth > 16) || !sectors || (sectors > 512) || !(d = malloc(sectors * 2048)))
		return (false);
	if (!ReadSectors(lba, sectors, DISCSRV_DATA, d, NULL, 0, 0))
	{
		free(d);
		return (false);
	}
	for (off = 0; off < sectors * 2048; )
	{
		r = d + off;
		if (!r[0] || (off % 2048) + r[0] > 2048)	// Records don't cross sectors
		{
			off = (off / 2048 + 1) * 2048;
			continue;
		}
		off += r[0];
		name_len = r[32];
		if ((r[0] < 33 + name_len) || ((name_len == 1) && (r[33] <= 1)))	// ".", ".."
			continue;
		if (n_files >= MAX_FILES)
			break;
		DiscFile	*f = &files[n_files];

		f->lba = Le32(r + 2);
		f->size = Le32(r + 10);
		f->dir = (r[25] & 2) != 0;
		f->attr = 0;
		su = 33 + name_len + !(name_len & 1);	// System use (XA) after the name, padded to an even offset
		if ((su + 14 <= r[0]) && (r[su + 6] == 'X') && (r[su + 7] == 'A'))
			f->attr = (r[su + 4] << 8) | r[su + 5];
		k = snprintf(f->path, MAX_PATH_LEN, "%s\\", path);
		for (i = 0; (i < (uint32_t)name_len) && (r[33 + i] != ';') && (k < MAX_PATH_LEN - 1); i++)
			f->path[k++] = r[33 + i];
		if ((k > 0) && (f->path[k - 1] == '.'))	// ("NAME." for no extension)
			k--;
		f->path[k] = 0;
		n_files++;
		if (f->dir)
		{
			char	sub[MAX_PATH_LEN];

			strcpy(sub, f->path);
			ok = ListDir(f->lba, f->size, sub, depth + 1) && ok;
		}
	}
	free(d);
	return (ok);
}

static bool	ReadFileSystem(void)
{
	uint8_t	pvd[2048];

	if (files)
		return (true);
	if (!(files = calloc(MAX_FILES, sizeof(DiscFile))))
		return (false);
	n_files = 0;
	if (!ReadSectors(16, 1, DISCSRV_DATA, pvd, NULL, 0, 0) || memcmp(pvd + 1, "CD001", 5))
	{
		printf("DISCERR not a data disc (no ISO 9660 volume)\n");
		free(files);
		files = NULL;
		return (false);
	}
	ListDir(Le32(pvd + 156 + 2), Le32(pvd + 156 + 10), "", 0);
	return (true);
}

static void	List(void)
{
	int	i;

	free(files);
	files = NULL;
	if (!ReadFileSystem())
		return;
	for (i = 0; i < n_files; i++)
	{
		if (files[i].dir)
			printf("DISCDIR %s\n", files[i].path);
		else
			printf("DISCFILE %u %u %04X %s\n", files[i].lba, files[i].size, files[i].attr, files[i].path);
	}
	printf("DISCLS %d\n", n_files);
}

static bool	EndsWith(const char *s, const char *ext)
{
	size_t	a = strlen(s), b = strlen(ext);

	return ((a >= b) && !strcasecmp(s + a - b, ext));
}

static void	Get(char *path, char *out, char *mode_arg)
{
	uint32_t	lba = 0, size = 0, sectors, ss;
	uint16_t	attr = 0;
	int			mode, i;
	uint8_t		*data;
	FILE		*f;

	if (!path || !out)
	{
		printf("DISCERR get PATH OUT\n");
		return;
	}
	if (path[0] == '@')		// @LBA:BYTES
	{
		char	*colon = strchr(path, ':');

		lba = strtoul(path + 1, NULL, 0);
		size = colon ? strtoul(colon + 1, NULL, 0) : 2048;
	}
	else
	{
		if (!ReadFileSystem())
			return;
		for (i = 0; i < n_files; i++)
			if (!files[i].dir && (!strcasecmp(files[i].path, path) || ((path[0] != '\\') && !strcasecmp(files[i].path + 1, path))))
				break;
		if (i == n_files)
		{
			printf("DISCERR %s isn't on the disc\n", path);
			return;
		}
		lba = files[i].lba;
		size = files[i].size;
		attr = files[i].attr;
	}
	sectors = (size + 2047) / 2048;
	if (mode_arg && !strcmp(mode_arg, "getxa"))
		mode = DISCSRV_XA;
	else if (mode_arg && !strcmp(mode_arg, "getdata"))
		mode = DISCSRV_DATA;
	else
		mode = ((attr & (XA_FORM2 | XA_INTERLEAVED)) || EndsWith(path, ".XA") || EndsWith(path, ".STR")) ? DISCSRV_XA :
			DISCSRV_DATA;
	if (!sectors || !(data = malloc(sectors * 2352)))
	{
		printf("DISCERR %s: empty file\n", path);
		return;
	}
	if (!ReadSectors(lba, sectors, mode, data, &ss, 0, sectors))
	{
		free(data);
		return;
	}
	size = (mode == DISCSRV_DATA) ? size : sectors * ss;
	if (!(f = fopen(out, "wb")) || (fwrite(data, 1, size, f) != size) || fclose(f))
		printf("DISCERR can't write %s\n", out);
	else
		printf("DISCSAVED %u %u %s %s\n", size, last_retries, (mode == DISCSRV_XA) ? "xa" : "data", out);
	free(data);
}

// One part of CD audio captured by the SPU (DISCSRV_CAPTURE): frames (L, R 16-bit) into dst, the number captured (0 on
// an error). The disc reader plays it in real time: nothing is asked meanwhile, only its end awaited.
// *first: the frame where the disc's audio starts in dst (the frames before: what the drive gave before playing), as
// the drive's first position report tells (*first_sector: its sector, at *first)
static int32_t	capture_lost;		// Frames the last capture missed (DiscServerMailbox.lost)
static bool		capture_strict;		// A capture that missed frames: refused (UINT32_MAX)
static uint32_t	capture_frames;		// The frames of the last capture (in the part, refused or not)

static uint32_t	Capture(uint32_t lba, uint32_t frames, uint32_t *dst, uint32_t *first, int32_t *first_sector)
{
	int32_t	r = Command(DISCSRV_CAPTURE, lba, frames, 0, frames / 44100 + 15);
	int32_t	sector = -1;

	if (r <= 0)
	{
		printf("DISCERR CD audio capture at sector %u: %s\n", lba, ErrorText(r));
		return (0);
	}
	if (!USB_Process('D', (uint8_t *)&sector, DS_FIELD(error_lba), 4) || (sector < 0))
	{
		printf("DISCERR CD audio capture at sector %u: the drive didn't play it\n", lba);
		return (0);
	}
	*first = last_retries;
	*first_sector = sector;
	{
		int32_t	lost = 0;

		USB_Process('D', (uint8_t *)&lost, DS_FIELD(lost), 4);
		if (getenv("DISC_DEBUG"))
			printf("DISCDBG lost %d\n", lost);
		capture_lost = lost;
	}
	if (!USB_Process('D', (uint8_t *)dst, DISCSRV_BUFFER_ADRS, r * 4))
	{
		printf("DISCERR USB error\n");
		return (0);
	}
	// (the position: to a sector, the frame: to half a buffer)
	capture_frames = r;
	if (capture_strict && ((capture_lost > 588 + 256) || (capture_lost < -588 - 256)))
		return (UINT32_MAX);
	return (r);
}

#define	CAP_PART		DISCSRV_CAPTURE_MAX		// Frames a part (5.2 s)
#define	CAP_OVERLAP		(44100 * 3 / 4)			// The next part starts 0.75 s before the end of the audio so far
#define	CAP_KEY			1024					// Frames of the end looked for in the next part
#define	CAP_NEAR		4096					// ... that far from where the drive's reports put them (about 30 frames off)

// Where the key (CAP_KEY frames) is in part (from frame from, n frames): its index, -1 if not found
static int	FindKey(const uint32_t *key, const uint32_t *part, int from, int n, int near)
{
	int	i, k;

	// Near where the drive's reports put it: music repeats, the same frames may be in the part more than once
	if (from < near - CAP_NEAR)
		from = near - CAP_NEAR;
	if (n > near + CAP_NEAR + CAP_KEY)
		n = near + CAP_NEAR + CAP_KEY;
	for (i = (from > 0) ? from : 0; i + CAP_KEY <= n; i++)
	{
		for (k = 0; (k < CAP_KEY) && (part[i + k] == key[k]); k++)
			;
		if (k == CAP_KEY)
			return (i);
	}
	return (-1);
}

#define	CAP_EARLY		30		// Sectors played before the one wanted (the drive's start)

// The CD audio from lba, total frames, put together from parts captured one after the other. Each part is played a
// bit before where it is needed; its frames are placed by the drive's first position report (the frames before it
// dropped: what the drive gave before playing). The parts after the first one are joined exactly where the end so far
// (its last CAP_KEY frames with sound) is found in them; in silence, by the reports alone. Returns the frames in out.
static uint32_t	CaptureTrack(uint32_t lba, uint32_t total, uint32_t *out, uint32_t floor)
{
	uint32_t	*part = malloc(CAP_PART * 4), have = 0, n, first, key_at, want, play;
	int32_t		first_sector;
	int			at, start_at, i;

	if (!part)
		return (0);
	capture_strict = true;
	while (have < total)
	{
		if (StopAsked())
		{
			have = 0;
			break;
		}
		want = lba + ((have > CAP_OVERLAP) ? (have - CAP_OVERLAP) / 588 : 0);	// The sector needed first
		play = (want >= floor + CAP_EARLY) ? want - CAP_EARLY : floor;	// (not in a data track: the drive won't play it)
		// Frames missed (the drive's position and the frames captured don't agree): captured again. Still after 3
		// times: the drive loses its place there (a scratched or dirty disc), the last one kept, with a warning
		for (i = 0; (i < 3) && ((n = Capture(play, CAP_PART, part, &first, &first_sector)) == UINT32_MAX); i++)
			;
		if (n == UINT32_MAX)
		{
			printf("DISCWARN CD audio near %d:%02d of the track: the drive lost its place there 3 times (a scratched or "
				"dirty disc?), the sound may have gaps\n", (play - lba) / 4500, (play - lba) / 75 % 60);
			n = capture_frames;
		}
		if (!n)
			break;
		// The frame of sector want in part, by the report (the audio of the report's sector at its frame)
		// (the drive plays from sector play: the frames from there are the disc's, before it what it gave meanwhile)
		at = (int)first + ((int)want - first_sector) * 588;
		start_at = (int)first + ((int)play - first_sector) * 588;
		if ((start_at < 0) || (at < start_at))
		{
			printf("DISCERR CD audio: the drive's position at sector %u isn't known\n", play);
			break;
		}
		if (!have)
		{
			if (at >= (int)n)
			{
				printf("DISCERR CD audio: the drive started too late at sector %u\n", play);
				break;
			}
			n -= at;
			if (n > total)
				n = total;
			memcpy(out, part + at, n * 4);
			have = n;
		}
		else
		{
			// The key: the last CAP_KEY frames with sound in the overlap (silence would be found anywhere)
			for (key_at = have - CAP_KEY; (key_at > have - CAP_OVERLAP / 2) && !out[key_at + CAP_KEY - 1]; key_at--)
				;
			for (i = 0; (i < CAP_KEY) && !out[key_at + i]; i++)
				;
			at = (i < CAP_KEY) ? FindKey(out + key_at, part, start_at, n, start_at + (int)key_at - (int)(play - lba) * 588) : -1;
			if (at >= 0)
				at += have - key_at;	// (the frame after the end so far)
			else	// (by the report: the frame of the end so far)
				at = (int)first + ((int)(lba + (have / 588)) - first_sector) * 588 + (int)(have % 588);
			if ((at < start_at) || (at >= (int)n))
			{
				printf("DISCERR CD audio: the part captured at sector %u couldn't be joined\n", play);
				break;
			}
			n -= at;
			if (have + n > total)
				n = total - have;
			memcpy(out + have, part + at, n * 4);
			have += n;
		}
		if (getenv("DISC_DEBUG"))	// (tests: each part's placing, the parts in files)
		{
			char	name[1024];
			FILE	*f;

			printf("DISCDBG play %u want %u frames %u first %u sector %d at %d have %u\n", play, want, n + at, first,
				first_sector, at, have);
			snprintf(name, sizeof(name), "%s/part_%u.raw", getenv("DISC_DEBUG"), play);
			if ((f = fopen(name, "wb")))
			{
				fwrite(part, 4, n + at, f);
				fclose(f);
			}
		}
		printf("DISCPROG %u %u\n", have / 588, total / 588);
	}
	free(part);
	capture_strict = false;
	return (have);
}

static void	Put16(uint8_t *p, uint32_t v)	{p[0] = v; p[1] = v >> 8;}
static void	Put32(uint8_t *p, uint32_t v)	{p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;}

bool	USB_ReadData(uint8_t *data, uint32_t size);	// (main.c: exactly size bytes, or false after its timeout)
bool	USB_SendRawByte(uint8_t c);

// CD audio from sector lba, sectors long, played once and taken as it plays (DISCSRV_STREAM: the disc reader sends each
// half capture buffer as it comes, the computer only reads meanwhile): the frames from lba into out (sectors * 588 at
// most). Returns the frames, 0 on an error or if the stream wasn't whole (*missed: halves the PS1 missed; the caller
// records it another way then).
static uint32_t	Stream(uint32_t lba, uint32_t sectors, uint32_t *out, uint32_t *missed)
{
	uint32_t	cap = (sectors + 75 * 12) * 588, got = 0, seq, asked = 0, done = 0, end[6] = {0}, index, i, at, n = 0;
	uint32_t	*pcm;
	uint8_t		hdr[8], data[STREAM_FRAMES * 4 + 1], sum;
	struct
	{
		uint32_t	cmd, lba, count, mode;
	}			f = {DISCSRV_STREAM, lba, sectors, 0};
	time_t		last = 0;
	bool		ended = false, stop_sent = false;

	*missed = 0;
	if (!(pcm = malloc(cap * 4)))
		return (0);
	for (i = 0; ; i++)	// (the previous command ended)
	{
		if (!USB_Process('D', (uint8_t *)&done, DS_FIELD(done_seq), 4) || !USB_Process('D', (uint8_t *)&asked, DS_FIELD(cmd_seq), 4))
			goto usb;
		if (asked == done)
			break;
		if (i > 4500)
			goto usb;
		LiveSleepMs(20);
	}
	seq = done + 1;
	if (!USB_Process('U', (uint8_t *)&f, DS_FIELD(cmd), sizeof(f)) || !USB_Process('U', (uint8_t *)&seq, DS_FIELD(cmd_seq), 4))
		goto usb;
	while (!ended)
	{
		if (!USB_ReadData(hdr, 8))
			goto usb;
		if ((hdr[0] != 'S') || (hdr[1] != 'D'))
		{
			printf("DISCWARN CD audio stream: lost its place after %u frames\n", got);
			goto out;
		}
		memcpy(&index, hdr + 4, 4);
		n = (hdr[2] == STREAM_END) ? sizeof(end) : STREAM_FRAMES * 4;
		if (!USB_ReadData(data, n + 1))
			goto usb;
		for (sum = 0, i = 0; i < n; i++)
			sum += data[i];
		n = 0;
		if (sum != data[(hdr[2] == STREAM_END) ? sizeof(end) : STREAM_FRAMES * 4])
		{
			printf("DISCWARN CD audio stream: a damaged block at frame %u\n", index);
			goto out;
		}
		if (hdr[2] == STREAM_END)
		{
			memcpy(end, data, sizeof(end));
			ended = true;
		}
		else if (index != got)
		{
			printf("DISCWARN CD audio stream: frame %u expected, %u came\n", got, index);
			goto out;
		}
		else if (got + STREAM_FRAMES <= cap)
		{
			memcpy(pcm + got, data, STREAM_FRAMES * 4);
			got += STREAM_FRAMES;
		}
		if (time(NULL) != last)
		{
			last = time(NULL);
			printf("DISCPROG %u %u\n", (got / 588 < sectors) ? got / 588 : sectors, sectors);
			if (!stop_sent && StopAsked())	// (the PS1 ends the stream, its end block comes)
				stop_sent = USB_SendRawByte(STREAM_STOP);
		}
	}
	for (i = 0; i < 100; i++)	// (its end)
	{
		if (USB_Process('D', (uint8_t *)&done, DS_FIELD(done_seq), 4) && (done == seq))
			break;
		LiveSleepMs(20);
	}
	*missed = end[3];
	if (stop_asked)
		goto out;
	if (end[3])
	{
		printf("DISCWARN CD audio stream: %u half buffers missed (the first at %.1f s)\n", end[3], end[4] / 44100.0);
		goto out;
	}
	if (((int32_t)end[1] < (int32_t)lba) || (end[2] < (end[1] - lba) * 588))
	{
		printf("DISCWARN CD audio stream: the drive's position wasn't known\n");
		goto out;
	}
	// The frame of sector lba: by the drive's first position (sector end[1] starts at frame end[2])
	at = end[2] - (end[1] - lba) * 588;
	n = (got > at) ? got - at : 0;
	if (n > sectors * 588)
		n = sectors * 588;
	memcpy(out, pcm + at, n * 4);
	if (getenv("DISC_DEBUG"))
		printf("DISCDBG stream: %u frames, the drive at sector %u from frame %u, lost %d\n", n, end[1], end[2],
			(int32_t)end[5]);
	goto out;
usb:
	printf("DISCWARN CD audio stream: USB error after %u frames\n", got);
	n = 0;
out:
	free(pcm);
	return (n);
}

// stream LBA SECTORS OUT (tests): Stream's frames written raw (L, R 16-bit) to OUT
static void	StreamTest(char *lba_s, char *sec_s, char *out)
{
	uint32_t	sectors = sec_s ? strtoul(sec_s, NULL, 0) : 0, *buf, n, missed;
	time_t		t0 = time(NULL);
	FILE		*f;

	if (!sectors || !out || !(buf = malloc(sectors * 588 * 4)))
	{
		printf("DISCERR stream LBA SECTORS OUT\n");
		return;
	}
	if ((n = Stream(lba_s ? strtoul(lba_s, NULL, 0) : 0, sectors, buf, &missed)) && (f = fopen(out, "wb")))
	{
		fwrite(buf, 4, n, f);
		fclose(f);
	}
	printf("DISCRAW stream: %u frames (%.2f s) in %d s, %u halves missed\n", n, n / 44100.0, (int)(time(NULL) - t0),
		missed);
	free(buf);
}

static void	Track(char *num, char *seconds, char *out)
{
	TocEntry	toc[101];
	int			n = ReadToc(toc), t = num ? atoi(num) : 0, i;
	uint32_t	lba, sectors, ss, bytes;
	uint8_t		*data, wav[44];
	FILE		*f;

	if (!n || !out)
	{
		if (!out)
			printf("DISCERR track N SECONDS OUT\n");
		return;
	}
	for (i = 0; (i < n) && (toc[i].track != t); i++)
		;
	if (i == n)
	{
		printf("DISCERR no track %d\n", t);
		return;
	}
	lba = toc[i].lba;
	sectors = toc[i + 1].lba - lba;
	if (seconds && (atoi(seconds) > 0) && ((uint32_t)atoi(seconds) * 75 < sectors))
		sectors = atoi(seconds) * 75;
	if (!(data = malloc(sectors * 2352)))
		return;
	// Played and captured by the SPU, in real time: ReadN with the CDDA mode (after SeekP or not, ReadS, any sector
	// size) gave no CD audio sector on the console tested
	ss = 2352;
	// (played from a bit before where each part is needed: not before the track if the one before is the data track)
	// Taken as it plays (real time); if the stream wasn't whole (a half buffer missed, the USB), recorded in parts
	// instead (about twice its length)
	{
		uint32_t	missed;

		bytes = Stream(lba, sectors, (uint32_t *)data, &missed) * 4;
		if ((bytes < sectors * 588 * 4) && !stop_asked)
		{
			printf("DISCWARN CD audio: recorded again in parts (slower)\n");
			bytes = CaptureTrack(lba, sectors * 588, (uint32_t *)data, (i && (toc[i - 1].track > 1)) ? toc[i - 1].lba :
				lba) * 4;
		}
	}
	if (!bytes || stop_asked)
	{
		if (stop_asked)
			printf("DISCSTOPPED\n");
		free(data);
		return;
	}
	memcpy(wav, "RIFF", 4);
	Put32(wav + 4, 36 + bytes);
	memcpy(wav + 8, "WAVEfmt ", 8);
	Put32(wav + 16, 16);
	Put16(wav + 20, 1);
	Put16(wav + 22, 2);
	Put32(wav + 24, 44100);
	Put32(wav + 28, 44100 * 4);
	Put16(wav + 32, 4);
	Put16(wav + 34, 16);
	memcpy(wav + 36, "data", 4);
	Put32(wav + 40, bytes);
	if (!(f = fopen(out, "wb")) || (fwrite(wav, 1, 44, f) != 44) || (fwrite(data, 1, bytes, f) != bytes) || fclose(f))
		printf("DISCERR can't write %s\n", out);
	else
		printf("DISCSAVED %u %u audio %s\n", 44 + bytes, 0, out);
	free(data);
}

// readraw LBA COUNT MODE OUT: sectors as the disc reader gives them (MODE: the mailbox's, hexadecimal: tests)
static void	ReadRaw(char *lba, char *count, char *mode, char *out)
{
	uint32_t	n = count ? strtoul(count, NULL, 0) : 1, ss = 0, m = mode ? strtoul(mode, NULL, 16) : 0;
	uint8_t		*data = malloc(n * 2352 + 4), dbg[4] = {0};
	FILE		*f;
	bool		ok;

	if (!lba || !out || !data)
	{
		printf("DISCERR readraw LBA COUNT MODE OUT\n");
		free(data);
		return;
	}
	ok = ReadSectors(strtoul(lba, NULL, 0), n, m & ~DISCSRV_DOUBLE, data, &ss, 0, n);
	USB_Process('D', dbg, DS_FIELD(status), 4);
	printf("DISCRAW %s sector size %u retries %u debug %02X %02X %02X\n", ok ? "ok" : "failed", ss, last_retries,
		dbg[0], dbg[1], dbg[2]);
	if (ok && (f = fopen(out, "wb")))
	{
		fwrite(data, 1, n * ss, f);
		fclose(f);
	}
	free(data);
}

static void	CaptureRaw(char *lba, char *frames, char *out)
{
	uint32_t	n = frames ? strtoul(frames, NULL, 0) : 44100, first = 0, *buf;
	int32_t		sector = 0;
	FILE		*f;

	if (!lba || !out || !n || (n > CAP_PART) || !(buf = malloc(n * 4)))
	{
		printf("DISCERR capture LBA FRAMES OUT (frames up to %u)\n", CAP_PART);
		return;
	}
	if ((n = Capture(strtoul(lba, NULL, 0), n, buf, &first, &sector)) && (n != UINT32_MAX) && (f = fopen(out, "wb")))
	{
		fwrite(buf, 4, n, f);
		fclose(f);
		printf("DISCRAW ok %u frames, first report: sector %d at frame %u\n", n, sector, first);
	}
	free(buf);
}

static void	Run(char *cmd)
{
	char	*a = strtok(NULL, " \t"), *b = NULL, *rest, *e, name[16];

	snprintf(name, sizeof(name), "%s", cmd);	// (the input's line buffer is read again by a "stop": its name kept)

	if (!strcmp(cmd, "track") || !strcmp(cmd, "readraw") || !strcmp(cmd, "capture") || !strcmp(cmd, "stream"))	// track N SECONDS OUT, ...
		b = strtok(NULL, " \t");
	char	*c = !strcmp(cmd, "readraw") ? strtok(NULL, " \t") : NULL;
	if ((rest = strtok(NULL, "")))	// The output file: the rest of the line
	{
		while ((*rest == ' ') || (*rest == '\t'))
			rest++;
		for (e = rest + strlen(rest); (e > rest) && ((e[-1] == ' ') || (e[-1] == '\r') || (e[-1] == '\n')); )
			*--e = 0;
		if (!*rest)
			rest = NULL;
	}

	if (!strcmp(cmd, "init"))
		Init();
	else if (!strcmp(cmd, "toc"))
		Toc();
	else if (!strcmp(cmd, "ls"))
		List();
	else if (!strcmp(cmd, "get") || !strcmp(cmd, "getxa") || !strcmp(cmd, "getdata"))
		Get(a, rest, cmd);
	else if (!strcmp(cmd, "track"))
		Track(a, b, rest);
	else if (!strcmp(cmd, "readraw"))
		ReadRaw(a, b, c, rest);
	else if (!strcmp(cmd, "capture"))	// capture LBA FRAMES OUT: one part of CD audio (tests)
		CaptureRaw(a, b, rest);
	else if (!strcmp(cmd, "stream"))	// stream LBA SECTORS OUT: CD audio taken as it plays (tests)
		StreamTest(a, b, rest);
	else if (!strcmp(cmd, "stop"))
	{
		int32_t	r = Command(DISCSRV_STOP, 0, 0, 0, 10);

		if (r < 0)
			printf("DISCERR %s\n", ErrorText(r));
	}
	else if (!strcmp(cmd, "speed"))
		read_speed = (a && (atoi(a) == 1)) ? 0 : DISCSRV_DOUBLE;
	else
		printf("DISCERR unknown command %s\n", cmd);
	printf("DISCDONE %s\n", name);
}

// The disc reader runs (started by main.c if needed). words: one command ("get", "\\X.STR", "x.str"), or none: commands
// on the standard input until "quit". Returns the exit code.
int	DiscLive(int count, char **words)
{
	char	line[1024], *cmd, *text;
	bool	eof = false;
	int		i;

	setvbuf(stdout, NULL, _IONBF, 0);
	usb_quiet = true;
	if (count)
	{
		for (line[0] = 0, i = 0; i < count; i++)	// (spaces in paths aren't in ISO names)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i ? " " : "", words[i]);
		if (strcmp(words[0], "init") && !Init())
			return (1);
		if ((cmd = strtok(line, " \t")))
			Run(cmd);
		return (0);
	}
	live_input = true;
	printf("DISCLIVE ready\n");
	while (!eof)
	{
		if (!LiveNextLine(&text, &eof))
		{
			LiveSleepMs(5);
			continue;
		}
		if (!(cmd = strtok(text, " \t")))
			continue;
		if (!strcmp(cmd, "quit"))
			break;
		stop_asked = false;
		Run(cmd);
	}
	return (0);
}
