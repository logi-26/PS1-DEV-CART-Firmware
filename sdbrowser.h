/*
* PS1 DEV CART: the games of a PicoStation's SD card (the PicoStation ODE, firmware RC5: github.com/megavolt85/PicoStation),
* chosen on the cartridge's PicoStation screen (START + SELECT when the PicoStation's menu disc is in the drive) and
* booted by the cartridge itself (BootDisc): the kernel hook is kept, unlike a game booted by the PicoStation's own
* menu (it restarts the BIOS).
*
* The PicoStation is found passively: its menu disc's volume is "PICO" (sector 16 read as any disc). Only then are
* its commands sent: they go to the drive's mechanism (a real drive would take them as its own commands).
*
* The PicoStation talks through the CD drive, like its menu: a command is the drive's Test command 19h,50h (a byte
* sent to the mechanism, which the PicoStation reads) with F0h | command and a 16-bit argument; the answer is a sector
* of its menu disc (only while that disc is mounted: after a power on, or RESET held one second). A directory page
* is the user data of LBA 100 (read whole, 2340 bytes: 2324 of data after the 12 header bytes):
*   ["LST1" marker entry: 04h 01h "LST1", RC5 release, not counted]
*   [length][flags: 1 directory, 0 a .cue file, its name without ".cue"][name: length bytes] ... then
*   [0][1: more entries][count, big endian: the directory's total on the first page, the page's entries after; FEFEh
*   none]. An entry's number (to open or mount it) is its place in the directory (hidden files left out, the marker
*   not counted).
* LBA 100 is answered once after a command, as the drive reaches it: read from LBA 98 (a seek right to LBA 100 always
* got the plain sector, tested with RC5). Each page is checked, read again, then asked again (Get Next Contents: no
* side effect, unlike entering or leaving a directory).
* Covers: Get Cover (09h, an entry's number) opens the .cov file next to the entry (a .cue: "name.cov", a directory:
* "directory.cov" beside it). LBA 200 to 207 are its 8 parts: "PART", the part number (16 bits), 2114 bytes of the
* file ("DEAD" instead: no cover). The file: a 256-color palette (PS1 colors, 512 bytes), then 128x128 pixels (bytes).
* Tested with the PicoStation RC5 release (its menu: packed, its protocol differs from the RC5 source on GitHub).
*
* The RC5 release's commands (found in its firmware, picostation_PU18.uf2 of the RC5 release; the source on GitHub
* is older): 00h none, 01h root, 02h parent, 03h enter directory, 04h get next contents, 05h mount (no status), 06h
* and 07h nothing, 08h extended (argument: sub-command << 1 | flag: 1 skip bootsector, 2 skip EDC, 3 open config.ini,
* 4 open background.raw, 5 overclock: only in the _OC builds), 09h open an entry's cover, 0Ah bootloader (argument BEEFh), 0Bh open an entry's
* cheats (".cht" next to it). Answers: LBA 100 directory page, 150 config.ini, 200-207 cover, 250-252 cheats,
* 300-333 background.raw (the parts: "PART", part number, data; "DEAD": no file). Its menu disc comes back only at
* power on or with RESET held one second: no command does it (nothing else in the firmware sets it).
*
* The directory is kept in free RAM (the game area, cleared by BootDisc), sorted: directories first, then the names.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _SDBROWSER_H_
#define _SDBROWSER_H_

#define	PICO_GOTO_ROOT		1
#define	PICO_GOTO_PARENT	2
#define	PICO_GOTO_DIR		3
#define	PICO_NEXT			4	// Get Next Contents: the current directory from an entry number
#define	PICO_MOUNT			5
#define	PICO_EXTENDED		8	// Argument: sub-command << 1 | flag
#define	PICO_EXT_SKIP_BOOT	(1 << 1)	// | 1: the bootsector fix off (0: on, as at power on and RESET held one second)
#define	PICO_EXT_SKIP_EDC	(2 << 1)	// | 1: the EDC correction off (0: on, the same)
#define	PICO_COVER			9
#define	PICO_CHEATS			11	// RC5 release: the .cht file of a .cue (not of a folder)
// Extra commands, used only when the PicoStation answers the information request (PICO_EXT_DEVCART_INFO; any other
// firmware, RC5 too, doesn't: sd_devcart stays 0): the cheats then asked with PICO_DEVCART_CHEATS (11 prepares a game
// there), the screen's settings read from and written to devcart.cfg at the SD card's root
#define	PICO_IO_COMMAND		6	// arg = op << 12 | value: PICO_OP_*
#define	PICO_IO_DATA		7	// arg = 2 bytes, the low one first
#define	PICO_DEVCART_CHEATS	14	// (13: PICO_LOG)
#define	PICO_EXT_DEVCART_CFG	(6 << 1)
#define	PICO_EXT_DEVCART_INFO	(7 << 1)
#define	PICO_OP_BEGIN		1
#define	PICO_OP_LENGTH		2
#define	PICO_OP_COMMIT		3
#define	PICO_DEVCART_CFG_LBA	260	// "DCF1", the bytes (16 bits), the text; "DCF0": none
#define	PICO_DEVCART_INFO_LBA	262	// "DCX1", features, the last write's status, the bytes received
#define	PICO_FEATURE_CHEATS		0x01
#define	PICO_FEATURE_CFG_READ	0x02
#define	PICO_FEATURE_CFG_WRITE	0x04
#define	PICO_FEATURE_GAME_CFG	0x08	// A game's .cfg (its settings, beside its .cue) read and written
#define	PICO_DEVCART_GAME_CFG	15		// A game's .cfg, arg the entry: answered as devcart.cfg (PICO_DEVCART_CFG_LBA)
#define	PICO_FILE_CFG		0		// PICO_OP_BEGIN's file: devcart.cfg,
#define	PICO_FILE_GAME		1		// or this + the entry: its .cfg
#define	PICO_FILE_GAME_MAX	4094
#define	PICO_FEATURE_DETAILS	0x10	// The listing's details: each folder's kind and discs, covers and cheats there
#define	PICO_FEATURE_PREPARE	0x20	// A game opened first (PICO_PREPARE), its status read until it is done
#define	PICO_FEATURE_LOG		0x40	// The cartridge's events in the PicoStation's /debug.log (SdLog)
#define	PICO_FEATURE_IDS		0x80	// Covers with the game's ID (from the disc image without one in the .cov)
#define	PICO_EXT_DEVCART_IDS	(9 << 1)	// | 1: covers so (until the PicoStation's own menu starts)
#define	PICO_EXT_DEVCART_DETAILS	(8 << 1)	// Before a listing command: that listing with its details
#define	PICO_PREPARE		11		// A game's .cue opened before the mount (PICO_FEATURE_PREPARE; RC5: PICO_CHEATS)
#define	PICO_LOG			13		// An event in the PicoStation's /debug.log (PICO_FEATURE_LOG)
#define	PICO_MOUNT_STATUS_LBA	110	// "MNT1", the entry (16 bits), FatFs' result (16 bits), flags (1: audio only)
#define	PICO_LOG_EVENT		0x2000	// PICO_LOG's argument: this + event << 8 + detail (8 bits)
#define	PICO_EV_LIST		0		// A listing not read (detail: the command)
#define	PICO_EV_READ		1		// Sectors not read (detail: the LBA / 2)
#define	PICO_EV_COVER		2		// A cover not read
#define	PICO_EV_CHEATS		3		// A .cht not read
#define	PICO_EV_CFG_READ	4		// A settings file not read (detail: 0 devcart.cfg, 1 a game's)
#define	PICO_EV_CFG_WRITE	5		// Not written (detail: the write's status)
#define	PICO_EV_PREPARE		6		// A game not opened (detail: FatFs' result; 255 no answer, 254 audio only)
#define	PICO_EV_BOOT		7		// A game mounted, its boot failed
#define	PICO_DETAIL_FOLDER	0x01	// The listing's details (PICO_FEATURE_DETAILS): flags,
#define	PICO_DETAIL_COVER	0x02	// a .cov (a folder: beside it, or its game's)
#define	PICO_DETAIL_CHEATS	0x04	// a .cht (a folder: its game's)
#define	PICO_DETAIL_KIND(_f_)	(((_f_) >> 4) & 3)	// a folder's: 0 not known, 1 a game, 2 a game's discs, 3 a folder
#define	PICO_WRITE_RECEIVING	1
#define	PICO_WRITE_OK		2
#define	DEVCART_CFG_TEXT	400	// (devcart.cfg read and written: less than that; the PicoStation takes 512)

#define	PICO_LIST_LBA		100
#define	PICO_COVER_LBA		200
#define	PICO_COVER_PARTS	8
#define	PICO_COVER_PART		2114
#define	PICO_CHEAT_LBA		250		// "CHT1", the bytes in it (16 bits), up to 2048 bytes of the file; "CHT0": none
#define	PICO_CHEAT_PARTS	3
#define	PICO_CHEAT_PART		2048
#define	PICO_RAW_SECTOR		2340	// Whole sector but the sync (Setmode 20h)
#define	PICO_DATA_OFS		12		// Header and subheader
#define	PICO_LIST_SIZE		2324
#define	PICO_MAX_ENTRIES	4096	// As the PicoStation
#define	PICO_LEAD			2		// Read from 2 sectors before (a seek right to the sector: the plain one)

#define	COVER_W				128
#define	COVER_H				128

// Free RAM while the menu runs (over the PicoStation's games area, under the debug monitor of a RAM test build)
#define	SD_SECTOR_ADRS		0x80180000	// Raw sectors read (up to PICO_LEAD + PICO_COVER_PARTS)
#define	SD_COVER_ADRS		0x80186000	// The .cov file (PICO_COVER_PARTS * PICO_COVER_PART bytes)
#define	SD_IMAGE_ADRS		0x8018C000	// The cover in 16-bit colors (32KB)
#define	SD_CHT_ADRS			0x80194000	// A .cht file read (PICO_CHEAT_PARTS * PICO_CHEAT_PART bytes, then 0)
#define	SD_DEBUG_ADRS		0x80195F00	// Test build: what the PicoStation answered
#define	SD_POOL_ADRS		0x80196000	// The entries
#define	SD_PROBE_POOL_ADRS	0x801A8000	// A folder looked into (its game: SdFolderCover), its entries
#define	SD_PROBE_TABLE_ADRS	0x801B8000	// and their pointers
#define	SD_TABLE_ADRS		0x801BC000	// PICO_MAX_ENTRIES pointers, sorted (up to 0x801C0000)
// The last covers shown (SD_CACHED, about 16.9 KB each): free RAM while the menu runs, a game booted writes over it.
// From 0x80120000 (after the VRAM saved at boot, 0x80020000) up to the buffers above (SD_SECTOR_ADRS), then from
// 0x801C0000 up to the firmware's stack (0x801E8800, the Makefile's FW_STACK_BOTTOM). A test build (make RAMTEST=1)
// keeps 7 under its uploaded menu images (0x80140000), its cheats and its debug monitor (0x801C0000) using the rest.
#define	SD_CACHE_ADRS		0x80120000
#ifdef TFS_RAM_ADRS
#define	SD_CACHED_1			7			// (up to 0x8013CE8C)
#define	SD_CACHED			SD_CACHED_1
#else
#define	SD_CACHE_ADRS_2		0x801C0000
#define	SD_CACHED_1			((SD_SECTOR_ADRS - SD_CACHE_ADRS) / sizeof(SdCached))	// 23
#define	SD_CACHED			(SD_CACHED_1 + (FW_STACK_BOTTOM - SD_CACHE_ADRS_2) / sizeof(SdCached))	// 32
#endif

// SdEntry.dir: a .cue file, or a folder. A folder is shown as the game it holds (each game in its own folder), until
// it is found to hold something else (looked into for its cover, or opened).
#define	SD_FILE				0
#define	SD_UNKNOWN			1	// A folder not looked into
#define	SD_GAME				2	// A folder holding one game (one .cue, nothing else)
#define	SD_FOLDER			3	// Any other folder: shown as a folder
#define	SD_DISCS			4	// A folder holding .cue files only (a game's discs): shown as a game, its cover the
								// first disc's; opened, the discs to choose from (the lid changes disc in there)


// The root's entry selected when the screen was left (or a game booted), at the next opening (after a reset too):
// 0xA5 << 24 | the entry's place, in the kernel's unused area (see ps1devcart.c PICO_GAME_ADRS)
#define	SD_LAST_ADRS		0x8000DF88
#define	SD_LAST_MAGIC		0xA5000000

// SdEntry.cov: where its cover was found (a cover asked for takes about half a second: asked again only where it is)
#define	SD_COV_UNKNOWN		0
#define	SD_COV_BESIDE		1	// Its own (next to the .cue, or beside the folder)
#define	SD_COV_INSIDE		2	// The one of the game in the folder (about 2 seconds: the folder looked into)
#define	SD_COV_NONE			3	// None

#define	SD_UNKNOWN_COUNT	0xFF	// SdEntry.discs, .cht: not known yet (SdLoadDetails)

typedef struct
{
	uint16_t	index;	// Its number in the directory (PicoStation commands)
	uint8_t		dir;
	uint8_t		cov;
	uint8_t		discs;	// Discs of the game (a folder: once looked into)
	uint8_t		cht;	// Cheats of its .cht file on the SD card (the first disc's for a game's discs)
	uint8_t		cht_on;	// and those ticked at first ("[name]", not "[*name]")
	char		name[];
} SdEntry;

// The last covers shown: the .cov file as read (an image, or only the game's ID) for an entry of a directory
typedef struct
{
	int16_t		dir;	// sd_dir when it was read (-1: none)
	int16_t		index;
	uint8_t		image;
	char		serial[13];
	uint8_t		data[512 + 128 * 128];
} SdCached;

// The cover kept in place k (0 to SD_CACHED - 1): the first block, then the second one
static inline SdCached	*SdCache(int k)
{
#ifdef SD_CACHE_ADRS_2
	if (k >= SD_CACHED_1)
		return ((SdCached *)SD_CACHE_ADRS_2 + (k - SD_CACHED_1));
#endif
	return ((SdCached *)SD_CACHE_ADRS + k);
}
static int		sd_dir, sd_cache_next;	// (sd_dir: counts the directories read, the cache's entries are of one)

// The list being read (Pico_List): the directory shown, or a folder looked into
static SdEntry	**sd_list = (SdEntry **)SD_TABLE_ADRS;
static int		sd_count;
static char		*sd_pool = (char *)SD_POOL_ADRS, *sd_pool_end = (char *)SD_PROBE_POOL_ADRS;
static char		sd_path[96];	// The current directory, "" at the root
static int		sd_view;		// SD_VIEW_*: SELECT, kept with the last selection
static int		sd_devcart;		// PICO_FEATURE_*: the extra commands answered, 0: none (RC5)
#define	SD_VIEW_LIST	0
#define	SD_VIEW_GRID	1
#define	SD_VIEW_FLOW	2	// Cover flow
#define	SD_VIEWS		3

// (MenuPad: the menu's pad, in ps1devcart.c)

// Test build: what the PicoStation answered, read by the computer (ps1transfer -d -a 0x80195F00 -s 32)
#ifdef MONITOR_IN_RAM
#define	SD_DEBUG(_i_, _v_)	(((volatile uint32_t *)SD_DEBUG_ADRS)[_i_] = (_v_))
#define	SD_DEBUG_INC(_i_)	(((volatile uint32_t *)SD_DEBUG_ADRS)[_i_]++)
#define	SD_TIME()			(*(volatile uint16_t *)0x1F801110)	// (test: timer 1, display lines: runs with interrupts off)
#define	SD_TIME_SINCE(_t_)	((uint16_t)(SD_TIME() - (_t_)))
#else
#define	SD_DEBUG(_i_, _v_)
#define	SD_DEBUG_INC(_i_)
#define	SD_TIME()			0
#define	SD_TIME_SINCE(_t_)	0
#endif

// The PicoStation's menu disc in the drive (its volume name "PICO")
static bool	PicoDetect(void)
{
	return (CD_WaitReady() && CD_ReadSectors(16, disc_exe, CD_SECTOR) && !memcmp(disc_exe + 1, "CD001", 5) &&
		!memcmp(disc_exe + 8, "PLAYSTATION ", 12) && !memcmp(disc_exe + 40, "PICO ", 5));
}

static bool	Pico_Command(int cmd, int arg)
{
	uint8_t	param[4] = {0x50, 0xF0 | cmd, arg >> 8, arg}, resp[8];
	int		irq;

	EnterCriticalSection();
	irq = CD_RawCommand(0x19, param, sizeof(param), resp, sizeof(resp));
	ExitCriticalSection();
	SD_DEBUG_INC(0);	// Commands, last: command and argument, interrupt
	SD_DEBUG(1, (cmd << 16) | (arg & 0xFFFF));
	SD_DEBUG(2, irq);
	return (irq == 3);
}

// An event in the PicoStation's /debug.log (PICO_FEATURE_LOG): what failed (PICO_EV_*), a detail
static void	SdLog(int event, int detail)
{
	if (sd_devcart & PICO_FEATURE_LOG)
		Pico_Command(PICO_LOG, PICO_LOG_EVENT | (event << 8) | (detail & 0xFF));
}

// Whole sectors (Setmode 20h, double speed as the PicoStation's menu) from PICO_LEAD sectors before lba: the first
// asked at SD_SECTOR_ADRS + PICO_LEAD * PICO_RAW_SECTOR
static bool	Pico_Read(int lba, int n)
{
	bool	ok;

	cd_read_mode = 0xA0;
	cd_read_sector = PICO_RAW_SECTOR;
	ok = CD_ReadSectors(lba - PICO_LEAD, (uint8_t *)SD_SECTOR_ADRS, (PICO_LEAD + n) * PICO_RAW_SECTOR);
	cd_read_mode = 0x00;
	cd_read_sector = CD_SECTOR;
	if (!ok)
		SdLog(PICO_EV_READ, lba >> 1);
	return (ok);
}

#define	PICO_SECTOR(_i_)	((uint8_t *)SD_SECTOR_ADRS + (PICO_LEAD + (_i_)) * PICO_RAW_SECTOR + PICO_DATA_OFS)

// A directory page added after the sd_count entries (pool: where the next one goes): 1 more pages, 0 the last one,
// -1 not a page (nothing added)
static int	Pico_AddPage(const uint8_t *d, char **pool)
{
	static const uint8_t	kinds[4] = {SD_UNKNOWN, SD_GAME, SD_DISCS, SD_FOLDER};
	char					*p = *pool;
	int						pos = 0, n = 0, details = 0, len, total, flags, i;
	SdEntry					*e;

	// RC5 release: the page starts with a "LST1" directory entry (a marker, not counted). Details asked for
	// (PICO_FEATURE_DETAILS): "DLS1", then each entry has its details (flags, discs) before its name.
	if (!memcmp(d, "\x04\x01LST1", 6))
		pos = 6;
	else if (!memcmp(d, "\x04\x01" "DLS1", 6))
	{
		pos = 6;
		details = 1;
	}
	while (pos + 4 <= PICO_LIST_SIZE)
	{
		if (!(len = d[pos]))
		{
			total = (d[pos + 2] << 8) | d[pos + 3];
			if ((d[pos + 1] > 1) || ((total == 0xFEFE) ? (n != 0) : (!n || (sd_count ? (total != n) : (total < n)))))
				return (-1);
			sd_count += n;
			*pool = p;
			return (d[pos + 1] && n);
		}
		flags = d[pos + 1];
		if ((flags > (details ? 0x3F : 1)) || (pos + 2 + details + len + 4 > PICO_LIST_SIZE))
			return (-1);
		for (i = 0; i < len; i++)
			if ((uint8_t)d[pos + 2 + details + i] < ' ')
				return (-1);
		// Full: the rest of the directory left out
		if ((sd_count + n < PICO_MAX_ENTRIES) && (p + sizeof(SdEntry) + len + 2 < sd_pool_end))
		{
			e = (SdEntry *)p;
			e->index = sd_count + n;
			e->dir = !(flags & PICO_DETAIL_FOLDER) ? SD_FILE : details ? kinds[PICO_DETAIL_KIND(flags)] : SD_UNKNOWN;
			// (no .cov: still asked for with the game IDs, a game's cover answer has its ID)
			e->cov = !details ? SD_COV_UNKNOWN : ((flags & PICO_DETAIL_COVER) || ((sd_devcart & PICO_FEATURE_IDS) &&
				(PICO_DETAIL_KIND(flags) != 3))) ? SD_COV_BESIDE : SD_COV_NONE;
			e->discs = (e->dir == SD_FILE) ? 1 : (e->dir == SD_UNKNOWN) ? SD_UNKNOWN_COUNT :
				(e->dir == SD_FOLDER) ? 0 : d[pos + 2];
			e->cht = (!details || (flags & PICO_DETAIL_CHEATS)) ? SD_UNKNOWN_COUNT : 0;	// (a count: read when shown)
			e->cht_on = 0;
			memcpy(e->name, d + pos + 2 + details, len);
			e->name[len] = 0;
			sd_list[sd_count + n] = e;
			p += (sizeof(SdEntry) + len + 2) & ~1;	// (index: 2-byte aligned)
			n++;
		}
		pos += 2 + details + len;
	}
	return (-1);
}

// Directories first, then the names (case not significant)
static int	SdCompare(const SdEntry *a, const SdEntry *b)
{
	const char	*p = a->name, *q = b->name;
	int			c, d;

	if ((a->dir != SD_FILE) != (b->dir != SD_FILE))	// (a folder's kind may be known: not sorted by it)
		return ((b->dir != SD_FILE) - (a->dir != SD_FILE));
	for (;; p++, q++)
	{
		c = *p - (((*p >= 'a') && (*p <= 'z')) ? 32 : 0);
		d = *q - (((*q >= 'a') && (*q <= 'z')) ? 32 : 0);
		if ((c != d) || !c)
			return (c - d);
	}
}

// A directory command and the whole directory read (sorted): false if the PicoStation doesn't answer
static bool	Pico_List(int cmd, int arg)
{
	char	*pool = sd_pool;
	SdEntry	*e;
	int		ret = 1, tries, gap, i, j;

	sd_count = 0;
	while (ret == 1)
	{
		// The command, then the page read up to 3 times; the page asked again (from its first entry) up to 4 times
		for (ret = -1, tries = 0; (ret < 0) && (tries < 12); tries++)
		{
			if (!(tries % 3))
			{
				if (((sd_devcart & PICO_FEATURE_DETAILS) && !Pico_Command(PICO_EXTENDED, PICO_EXT_DEVCART_DETAILS)) ||
					!Pico_Command(cmd, arg))
					continue;
				cmd = PICO_NEXT;	// Done once: asked again, the same directory (no side effect)
				arg = sd_count;
			}
			if (Pico_Read(PICO_LIST_LBA, 1))
				ret = Pico_AddPage(PICO_SECTOR(0), &pool);
			SD_DEBUG_INC(4);	// Reads, last page result
			SD_DEBUG(5, ret);
		}
		if (ret < 0)
		{
			SdLog(PICO_EV_LIST, cmd);
			return (false);
		}
		cmd = PICO_NEXT;
		arg = sd_count;
	}
	if (sd_pool == (char *)SD_POOL_ADRS)	// (the directory shown: the covers kept are of the one before)
		sd_dir++;

	for (gap = sd_count / 2; gap; gap /= 2)	// Shell sort
		for (i = gap; i < sd_count; i++)
			for (j = i; (j >= gap) && (SdCompare(sd_list[j - gap], sd_list[j]) > 0); j -= gap)
			{
				e = sd_list[j];
				sd_list[j] = sd_list[j - gap];
				sd_list[j - gap] = e;
			}
	return (true);
}

// The game ID found with the last cover (sd_serial[0]: none): the PicoStation's 8 parts carry 16 bytes more than the
// image, where pctools/picostation_ids.py writes "PSID" (or "PSIX": the cover is blank, written for its ID) then the
// game's serial (its executable's name, as BootDisc reads it: the cartridge's cheats are found by it)
#define	COVER_FILE			(512 + COVER_W * COVER_H)
static char	sd_serial[CHEAT_DB_SERIAL + 1];

// Pico_Cover's answers
#define	COV_NONE			0	// No .cov file (or the PicoStation doesn't answer)
#define	COV_IMAGE			1	// A cover
#define	COV_ID				2	// Only a game ID (a blank cover)

// An entry's .cov file at SD_COVER_ADRS (COV_NONE, COV_IMAGE or COV_ID), its game ID in sd_serial
static int	Pico_Cover(int index)
{
	uint8_t	*cov = (uint8_t *)SD_COVER_ADRS, *s;
	int		tries, part, got, i;

	sd_serial[0] = 0;
	for (tries = 0; tries < 3; tries++)
	{
		if (!Pico_Command(PICO_COVER, index) || !Pico_Read(PICO_COVER_LBA, PICO_COVER_PARTS))
			continue;
		for (got = part = 0; part < PICO_COVER_PARTS; part++)
		{
			s = PICO_SECTOR(part);
			if (!memcmp(s, "DEAD", 4))
				return (COV_NONE);
			if (!memcmp(s, "PART", 4) && (s[4] == part) && !s[5])
			{
				memcpy(cov + part * PICO_COVER_PART, s + 6, PICO_COVER_PART);
				got++;
			}
		}
		SD_DEBUG(6, got);
		if (got == PICO_COVER_PARTS)
		{
			s = cov + COVER_FILE;
			if (!memcmp(s, "PSI", 3) && ((s[3] == 'D') || (s[3] == 'X')))
			{
				for (i = 0; (i < CHEAT_DB_SERIAL) && (s[4 + i] >= ' '); i++)
					sd_serial[i] = s[4 + i];
				sd_serial[i] = 0;
				if (s[3] == 'X')	// (blank: only its ID)
					return (COV_ID);
			}
			return (COV_IMAGE);
		}
	}
	SdLog(PICO_EV_COVER, 0);
	return (COV_NONE);
}

// Covers drawn in their shape: the .cov files are 128 x 128, a PAL picture's pixels are 1.2 times as wide as tall
// (240 of its 288 lines): 106 x 128 there (thumbnails 54 x 64), square on NTSC. Set when the screen opens.
static int	sd_cov_w = COVER_W, sd_thumb_w = 64;

// The .cov file at SD_COVER_ADRS in 16-bit colors at SD_IMAGE_ADRS, w x h: each pixel the average of those it covers
static void	SdCoverScaled(int w, int h)
{
	uint8_t		*cov = (uint8_t *)SD_COVER_ADRS + 512;
	uint16_t	*pal = (uint16_t *)SD_COVER_ADRS, *out = (uint16_t *)SD_IMAGE_ADRS;
	int			x, y, u, v;

	for (y = 0; y < h; y++)
	{
		int	y0 = y * COVER_H / h, y1 = (y + 1) * COVER_H / h;

		for (x = 0; x < w; x++)
		{
			int	x0 = x * COVER_W / w, x1 = (x + 1) * COVER_W / w, r = 0, g = 0, b = 0, n = 0;

			for (v = y0; v < y1; v++)
				for (u = x0; u < x1; u++)
				{
					uint16_t	c = pal[cov[v * COVER_W + u]];

					r += c & 31;
					g += (c >> 5) & 31;
					b += (c >> 10) & 31;
					n++;
				}
			*out++ = (r / n) | ((g / n) << 5) | ((b / n) << 10) | (pal[cov[y0 * COVER_W + x0]] & 0x8000);
		}
	}
}

static void	SdCoverImage(void)
{
	SdCoverScaled(sd_cov_w, COVER_H);
}

// A folder shown as a game: looked into (its own list, the directory shown is kept). One .cue file in it: fn for that
// game (its number in the folder), the folder is SD_GAME; .cue files only (a game's discs): fn for the first one (the
// list is sorted: disc 1), SD_DISCS; anything else: SD_FOLDER, 0. The PicoStation goes back up, its answer read (done
// before the next command). Returns what fn returns.
static int	SdInFolder(SdEntry *e, int (*fn)(int))
{
	SdEntry	**list = sd_list;
	char	*pool = sd_pool, *end = sd_pool_end;
	int		count = sd_count, found = 0, i;

	sd_list = (SdEntry **)SD_PROBE_TABLE_ADRS;
	sd_pool = (char *)SD_PROBE_POOL_ADRS;
	sd_pool_end = (char *)SD_PROBE_TABLE_ADRS;
	if (Pico_List(PICO_GOTO_DIR, e->index))
	{
		for (i = 0; (i < sd_count) && (sd_list[i]->dir == SD_FILE); i++)
			;
		if (sd_count && (i == sd_count))
		{
			e->dir = (sd_count == 1) ? SD_GAME : SD_DISCS;
			e->discs = (sd_count < SD_UNKNOWN_COUNT) ? sd_count : SD_UNKNOWN_COUNT - 1;
			found = fn(sd_list[0]->index);
		}
		else
			e->dir = SD_FOLDER;
		Pico_List(PICO_GOTO_PARENT, 0);
	}
	sd_list = list;
	sd_pool = pool;
	sd_pool_end = end;
	sd_count = count;
	return (found);
}

// A game's .cht file (the PicoStation's cheats, next to its .cue) as text at SD_CHT_ADRS: false if it has none
static int	Pico_Cheats(int index)
{
	char	*text = (char *)SD_CHT_ADRS;
	uint8_t	*s;
	int		tries, part, len;

	for (tries = 0; tries < 3; tries++)
	{
		if (!Pico_Command((sd_devcart & PICO_FEATURE_CHEATS) ? PICO_DEVCART_CHEATS : PICO_CHEATS, index) ||
			!Pico_Read(PICO_CHEAT_LBA, PICO_CHEAT_PARTS))
			continue;
		for (part = 0; part < PICO_CHEAT_PARTS; part++)
		{
			s = PICO_SECTOR(part);
			if (!memcmp(s, "CHT0", 4))
				return (false);
			if (memcmp(s, "CHT1", 4))
				break;			// (not answered: asked again)
			len = s[4] | (s[5] << 8);
			len = (len < PICO_CHEAT_PART) ? len : PICO_CHEAT_PART;
			memcpy(text + part * PICO_CHEAT_PART, s + 6, len);
			text[part * PICO_CHEAT_PART + len] = 0;
			if (len < PICO_CHEAT_PART)	// (the file's end)
				return (true);
		}
		if (part == PICO_CHEAT_PARTS)	// (6KB: as much as the PicoStation reads)
			return (true);
	}
	SdLog(PICO_EV_CHEATS, 0);
	return (false);
}

// An entry's .cht (Pico_Cheats): a folder's game's is asked for the folder itself with the details (the PicoStation
// looks into it), else the folder is looked into (SdInFolder: it may turn out to be a folder)
static int	SdEntryCheats(SdEntry *e)
{
	if ((e->dir == SD_FILE) || ((sd_devcart & PICO_FEATURE_DETAILS) && ((e->dir == SD_GAME) || (e->dir == SD_DISCS))))
		return (Pico_Cheats(e->index));
	return ((e->dir != SD_FOLDER) && SdInFolder(e, Pico_Cheats));
}

// The extra commands' information sector (features, the last write's status), false when the PicoStation doesn't
// answer it (RC5: the menu disc's own sector there)
static bool	Pico_DevcartInfo(int *features, int *status)
{
	uint8_t	*s;

	if (!Pico_Command(PICO_EXTENDED, PICO_EXT_DEVCART_INFO) || !Pico_Read(PICO_DEVCART_INFO_LBA, 1))
		return (false);
	s = PICO_SECTOR(0);
	if (memcmp(s, "DCX1", 4))
		return (false);
	*features = s[4] | (s[5] << 8);
	if (status)
		*status = s[6] | (s[7] << 8);
	return (true);
}

static const char * const	sd_view_names[SD_VIEWS] = {"list", "grid", "flow"};

// devcart.cfg's value of key ("key=value" lines) copied to value (at most n - 1 characters), false if not there
static bool	SdCfgValue(const char *text, const char *key, char *value, int n)
{
	const char	*p = text;
	int			len = strlen(key), i;

	while (*p)
	{
		if (!strncmp(p, key, len) && (p[len] == '='))
		{
			p += len + 1;
			for (i = 0; (i < n - 1) && ((uint8_t)p[i] >= ' '); i++)	// (the line: a name has spaces)
				value[i] = p[i];
			while (i && (value[i - 1] == ' '))
				i--;
			value[i] = 0;
			return (true);
		}
		while (*p && (*p != '\n'))
			p++;
		if (*p)
			p++;
	}
	return (false);
}

// devcart.cfg (cmd PICO_EXTENDED, arg PICO_EXT_DEVCART_CFG) or a game's .cfg (PICO_DEVCART_GAME_CFG, its entry) as
// text (up to DEVCART_CFG_TEXT characters): 1 read, 0 no such file, -1 not answered (each asked up to 3 times)
static int	Pico_CfgRead(int cmd, int arg, char *text)
{
	uint8_t	*s;
	int		tries, len;

	for (tries = 0; tries < 3; tries++)
	{
		if (!Pico_Command(cmd, arg) || !Pico_Read(PICO_DEVCART_CFG_LBA, 1))
			continue;
		s = PICO_SECTOR(0);
		if (!memcmp(s, "DCF0", 4))
			return (0);
		if (memcmp(s, "DCF1", 4))
			continue;
		len = s[4] | (s[5] << 8);
		len = (len < DEVCART_CFG_TEXT) ? len : DEVCART_CFG_TEXT;
		memcpy(text, s + 6, len);
		text[len] = 0;
		return (1);
	}
	SdLog(PICO_EV_CFG_READ, cmd != PICO_EXTENDED);
	return (-1);
}

// text written to devcart.cfg (file PICO_FILE_CFG) or a game's .cfg (PICO_FILE_GAME + its entry): 2 bytes a command,
// then the PicoStation's own check and its safe write (a .tmp then renamed). False if the write failed.
static bool	Pico_CfgWrite(int file, char *text)
{
	int		len = strlen(text), sum = 0, status = 0, features, i;

	for (i = 0; i < len; i++)
		sum += (uint8_t)text[i];	// (a last odd byte sent with the 0 after it)
	if (!Pico_Command(PICO_IO_COMMAND, (PICO_OP_BEGIN << 12) | file) ||
		!Pico_Command(PICO_IO_COMMAND, (PICO_OP_LENGTH << 12) | len))
		return (false);
	for (i = 0; i < len; i += 2)
		if (!Pico_Command(PICO_IO_DATA, (uint8_t)text[i] | ((uint8_t)text[i + 1] << 8)))
			return (false);
	if (!Pico_Command(PICO_IO_COMMAND, (PICO_OP_COMMIT << 12) | (sum & 0xFFF)))
		return (false);
	for (i = 0; i < 10; i++)	// (until written: its sector isn't served while the PicoStation writes)
		if (Pico_DevcartInfo(&features, &status) && (status != PICO_WRITE_RECEIVING))
			break;
	if (status != PICO_WRITE_OK)
		SdLog(PICO_EV_CFG_WRITE, status);
	return (status == PICO_WRITE_OK);
}

// The place the screen was left at (devcart.cfg "folder=" and "selected=": the SD card's, they win over the RAM's
// SD_LAST_ADRS): the folder ("" the root, "/A/B"), its entry selected. sd_place_*: as read then as kept,
// sd_cfg_*: as devcart.cfg has them.
#define	SD_PLACE_NAME	128
static char	sd_place_folder[sizeof(sd_path)], sd_place_name[SD_PLACE_NAME];
static char	sd_cfg_folder[sizeof(sd_path)], sd_cfg_name[SD_PLACE_NAME];

// The screen's settings from devcart.cfg on the SD card (they win over the cartridge's): the view, the game settings
// screen (gamescreen=off: X boots at once), the place. Nothing without the file or the extra commands.
static void	SdCfgLoad(void)
{
	char	text[DEVCART_CFG_TEXT + 1], value[8];
	int		i;

	sd_place_folder[0] = sd_place_name[0] = sd_cfg_folder[0] = sd_cfg_name[0] = 0;
	if (!(sd_devcart & PICO_FEATURE_CFG_READ) || (Pico_CfgRead(PICO_EXTENDED, PICO_EXT_DEVCART_CFG, text) <= 0))
		return;
	if (!SdCfgValue(text, "folder", sd_place_folder, sizeof(sd_place_folder)) || (sd_place_folder[0] != '/'))
		sd_place_folder[0] = 0;
	SdCfgValue(text, "selected", sd_place_name, sizeof(sd_place_name));
	strcpy(sd_cfg_folder, sd_place_folder);
	strcpy(sd_cfg_name, sd_place_name);
	if (SdCfgValue(text, "view", value, sizeof(value)))
		for (i = 0; i < SD_VIEWS; i++)
			if (!strcmp(value, sd_view_names[i]))
				sd_view = i;
	if (SdCfgValue(text, "gamescreen", value, sizeof(value)))
		saver_cfg[SAVER_OPTS] = strcmp(value, "off") ? (saver_cfg[SAVER_OPTS] & ~OPT_NO_GAME_SCREEN) :
			(saver_cfg[SAVER_OPTS] | OPT_NO_GAME_SCREEN);
	if (SdCfgValue(text, "autostart", value, sizeof(value)))
		saver_cfg[SAVER_OPTS] = strcmp(value, "on") ? (saver_cfg[SAVER_OPTS] & ~OPT_PICO_START) :
			(saver_cfg[SAVER_OPTS] | OPT_PICO_START);
}

// The settings as devcart.cfg has them (read or written last): written again only when they changed
static int		sd_cfg_view = -1;
static uint8_t	sd_cfg_opts;

#define	SD_CFG_OPTS	(OPT_NO_GAME_SCREEN | OPT_PICO_START)

static void	SdCfgKeep(void)
{
	char	text[DEVCART_CFG_TEXT];

	if (!(sd_devcart & PICO_FEATURE_CFG_WRITE) || ((sd_view == sd_cfg_view) &&
		((saver_cfg[SAVER_OPTS] & SD_CFG_OPTS) == sd_cfg_opts) && !strcmp(sd_place_folder, sd_cfg_folder) &&
		!strcmp(sd_place_name, sd_cfg_name)))
		return;
	sprintf(text, "# PS1 DEV CART: the PicoStation screen's settings (they win over the cartridge's)\n"
		"view=%s\ngamescreen=%s\nautostart=%s\nfolder=%s\nselected=%s\n", sd_view_names[sd_view],
		(saver_cfg[SAVER_OPTS] & OPT_NO_GAME_SCREEN) ? "off" : "on", (saver_cfg[SAVER_OPTS] & OPT_PICO_START) ? "on" : "off",
		sd_place_folder, sd_place_name);
	if (Pico_CfgWrite(PICO_FILE_CFG, text))
	{
		sd_cfg_view = sd_view;
		sd_cfg_opts = saver_cfg[SAVER_OPTS] & SD_CFG_OPTS;
		strcpy(sd_cfg_folder, sd_place_folder);
		strcpy(sd_cfg_name, sd_place_name);
	}
}

// The place kept (the first len characters of sd_path: the folder; name: its entry selected), devcart.cfg written
static void	SdKeepPlace(int len, const char *name)
{
	int		i;

	memcpy(sd_place_folder, sd_path, len);
	sd_place_folder[len] = 0;
	for (i = 0; (i < SD_PLACE_NAME - 1) && name[i]; i++)
		sd_place_name[i] = name[i];
	sd_place_name[i] = 0;
	SdCfgKeep();
}

// A game's settings (SdGameScreen): the PicoStation's EDC correction and bootsector fix, each game its own (its .cfg
// beside its .cue, with PICO_FEATURE_GAME_CFG). Without it: on, as the PicoStation has them.
static bool	sd_game_edc, sd_game_bootfix;

static void	SdGameLoad(const SdEntry *e)
{
	char	text[DEVCART_CFG_TEXT + 1], value[8];

	sd_game_edc = sd_game_bootfix = true;
	if (!(sd_devcart & PICO_FEATURE_GAME_CFG) || (Pico_CfgRead(PICO_DEVCART_GAME_CFG, e->index, text) <= 0))
		return;
	if (SdCfgValue(text, "edc", value, sizeof(value)))
		sd_game_edc = strcmp(value, "off");
	if (SdCfgValue(text, "bootfix", value, sizeof(value)))
		sd_game_bootfix = strcmp(value, "off");
}

static bool	SdGameSave(const SdEntry *e)
{
	char	text[DEVCART_CFG_TEXT];

	if (!(sd_devcart & PICO_FEATURE_GAME_CFG) || (e->index > PICO_FILE_GAME_MAX))
		return (false);
	sprintf(text, "# PS1 DEV CART: this game's settings (the cartridge's PicoStation screen)\nedc=%s\nbootfix=%s\n",
		sd_game_edc ? "on" : "off", sd_game_bootfix ? "on" : "off");
	return (Pico_CfgWrite(PICO_FILE_GAME + e->index, text));
}

// A hex number of n digits in v: false if it isn't one
static bool	SdHex(const char *p, int n, uint32_t *v)
{
	*v = 0;
	while (n--)
	{
		int	c = *p++;

		c = ((c >= '0') && (c <= '9')) ? c - '0' : (((c | 32) >= 'a') && ((c | 32) <= 'f')) ? (c | 32) - 'a' + 10 : -1;
		if (c < 0)
			return (false);
		*v = (*v << 4) | c;
	}
	return (true);
}

// A .cht file's cheats (text, PicoStation RC5: "[name]" or "[*name]" (off at first) then its codes, "XXXXXXXX YYYY"
// lines; lines starting with # ; / " left out) added to cheat_list after its n first ones, as the flash's records
// (cheatmenu.h: name length, name, codes count, codes: 4 + 2 bytes) written from rec. A cheat with a code the code
// engine doesn't do is left out. Returns the cheats listed.
static int	SdCheatText(char *text, int n, uint8_t *rec, uint8_t *rec_end)
{
	static const uint8_t	types[] = {0x80, 0x30, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3, 0x50, 0xC0};
	uint8_t					*r = NULL, *codes = NULL;
	char					*line, *next, *name;
	int						len, i, bad = 0, start = n;
	uint32_t				code, value;

	for (line = text; *line && (n <= CHEATS_SHOWN); line = next)
	{
		for (next = line; *next && (*next != '\n'); next++)
			;
		len = next - line;
		if (*next)
			next++;
		if (len && (line[len - 1] == '\r'))
			len--;
		if (!len || (*line == '#') || (*line == ';') || (*line == '/') || (*line == '"'))
			continue;
		if (*line == '[')		// A cheat (the one before kept if it has codes, all done by the code engine)
		{
			if (r && (bad || !codes[0]))
				rec = r, n--;
			if (n == CHEATS_SHOWN)
				break;
			name = line + 1;
			cheat_on[n] = (*name != '*');
			name += (*name == '*');
			len -= name - line;
			len -= (len && (name[len - 1] == ']'));
			len = (len < 40) ? len : 40;
			if (rec + 2 + len > rec_end)
				break;
			r = rec;
			r[0] = len;
			memcpy(r + 1, name, len);
			codes = r + 1 + len;
			codes[0] = 0;
			rec = codes + 1;
			cheat_list[n++] = r;
			bad = 0;
			continue;
		}
		if (!r || (len < 13) || (line[8] != ' ') || !SdHex(line, 8, &code) || !SdHex(line + 9, 4, &value))
			continue;
		for (i = 0; (i < sizeof(types)) && (types[i] != (code >> 24)); i++)
			;
		if ((i == sizeof(types)) || (codes[0] == 255) || (rec + 6 > rec_end))
		{
			bad = 1;
			continue;
		}
		for (i = 0; i < 4; i++)
			*rec++ = code >> (i * 8);
		*rec++ = value;
		*rec++ = value >> 8;
		codes[0]++;
	}
	if (r && (bad || !codes[0]) && (n > start))
		n--;
	return (n);
}

/****************************************/
// The PicoStation screen (no logo): a title bar, the list on the left, the cover on the right with the game's details
// under it, the buttons at the bottom. Bars and panels share the same 8 pixel margins.

#define	SD_MARGIN		8
#define	SD_WIDTH		(SCREEN_WIDTH - 2 * SD_MARGIN)
#define	SD_TITLE_Y		8
#define	SD_TITLE_H		14
#define	SD_HELP_H		14
#define	SD_HELP_Y		(SCREEN_HEIGHT - SD_MARGIN - SD_HELP_H)
#define	SD_BODY_Y		(SD_TITLE_Y + SD_TITLE_H + 4)
#define	SD_BODY_H		(SD_HELP_Y - 4 - SD_BODY_Y)
#define	SD_GAME_CHARS	(SD_WIDTH / FONT_CHAR_W - 2)	// A game's name on a whole line
#define	SD_ROW_H		10
#define	SD_ROWS			((SD_BODY_H - 8) / SD_ROW_H)
#define	SD_LIST_W		172
#define	SD_CHARS		20						// Name characters in a row
#define	SD_COVER_X		(SCREEN_WIDTH - SD_MARGIN - COVER_W)
#define	SD_INFO_Y		(SD_BODY_Y + COVER_H + 4)	// The details panel, under the cover
#define	SD_INFO_H		(SD_BODY_Y + SD_BODY_H - SD_INFO_Y)
#define	SD_INFO_LINE	12
#define	SD_TITLE		"PICOSTATION MENU"
#define	SD_TITLE_END	(12 + (sizeof(SD_TITLE) - 1) * FONT_CHAR_W + 4)	// (where the selected entry's place may start)

// The title bar (the selected entry's place at the right: SdDrawPos). No folder name (a game's discs: its name)
static void	SdDrawTitle(void)
{
	FillRect(SD_MARGIN, SD_TITLE_Y, SD_WIDTH, SD_TITLE_H, UI_BAR);
	DrawText(12, SD_TITLE_Y + 3, SD_TITLE);
}

// A game's region from its ID's prefix (NULL: not known). LSP (Lightspan: LSP0_, LSP1_, LSP2_, LSP9_, LSP_) by its
// first 3 letters. DTLS (Sony's development/demo discs, no single region): "DEV".
static const struct
{
	char	prefix[5];
	uint8_t	region;		// 0 NTSC-U, 1 PAL, 2 NTSC-J, 3 DEV
} sd_regions[] = {
	{"DTLS", 3},
	{"SCUS", 0}, {"SLUS", 0}, {"SPUS", 0}, {"LSP", 0},
	{"SCES", 1}, {"SLES", 1}, {"SLED", 1}, {"SCED", 1},
	{"SLPS", 2}, {"SLPM", 2}, {"SCPS", 2}, {"SCPM", 2}, {"SCAJ", 2}, {"SLKA", 2}, {"SIPS", 2}, {"ESPM", 2},
	{"SCZS", 2}, {"PCPX", 2}, {"PAPX", 2}, {"PTPX", 2}, {"PBPX", 2},
};

static const char	*SdRegion(const char *serial)
{
	static const char	*const names[] = {"NTSC-U", "PAL", "NTSC-J", "DEV"};
	int					i;

	for (i = 0; i < sizeof(sd_regions) / sizeof(sd_regions[0]); i++)
		if (!memcmp(serial, sd_regions[i].prefix, strlen(sd_regions[i].prefix)))
			return (names[sd_regions[i].region]);
	return (NULL);
}

// A line of the details panel, centered under the cover: text, then more in another color (NULL: none)
static void	SdInfoLine(int line, const char *text, int r, int g, int b, const char *more)
{
	int	n = strlen(text), m = more ? strlen(more) : 0, x;

	x = SD_COVER_X + (COVER_W - (n + m) * FONT_CHAR_W) / 2;
	DrawTextColor(x, SD_INFO_Y + 6 + line * SD_INFO_LINE, text, r, g, b);
	if (more)
		DrawTextColor(x + n * FONT_CHAR_W, SD_INFO_Y + 6 + line * SD_INFO_LINE, more, TEXT_DIM);
}

// Under the cover: the game's ID (with its cover), its region, its cheats (the cartridge's and its .cht file's, those
// ticked at first in brackets) and its discs. "..." until read from the SD card (SdLoadDetails). Nothing for a folder.
static void	SdDrawInfo(const SdEntry *e)
{
	char		text[CHEAT_DB_TITLE], more[8];
	const char	*region = sd_serial[0] ? SdRegion(sd_serial) : NULL;
	int			n = sd_serial[0] ? CheatFind(sd_serial, cheat_list, text) : 0;

	FillRect(SD_COVER_X, SD_INFO_Y, COVER_W, SD_INFO_H, UI_PANEL);
	if (!e || (e->dir == SD_FOLDER))
		return;
	if (sd_serial[0])
		SdInfoLine(0, sd_serial, TEXT_ACCENT, NULL);
	else
		SdInfoLine(0, "NO GAME ID", TEXT_DIM, NULL);
	if (region)
		SdInfoLine(1, region, TEXT_WHITE, NULL);
	if (saver_cfg[SAVER_OPTS] & OPT_NO_DETAILS)	// (set in the Manager: faster, nothing more read)
		return;
	if (e->cht == SD_UNKNOWN_COUNT)
		sprintf(text, "CHEATS: ...");
	else
	{
		sprintf(text, "CHEATS: %d ", n + e->cht);
		sprintf(more, "(%d)", e->cht_on);
	}
	SdInfoLine(3, text, TEXT_WHITE, (e->cht == SD_UNKNOWN_COUNT) ? NULL : more);
	if (e->discs == SD_UNKNOWN_COUNT)
		sprintf(text, "DISCS: ...");
	else
		sprintf(text, "DISCS: %d", e->discs);
	SdInfoLine(2, text, TEXT_WHITE, NULL);
}

// The buttons bar (centered), or a notice there for a moment
#define	SD_HELP_TEXT	ICON_CROSS "BOOT " ICON_CIRCLE "BACK " ICON_TRIANGLE "CHEATS " ICON_SELECT "VIEW " ICON_START "OPTIONS"

static void	SdDrawHelp(const char *notice)
{
	const char	*text = notice ? notice : SD_HELP_TEXT;

	FillRect(SD_MARGIN, SD_HELP_Y, SD_WIDTH, SD_HELP_H, UI_PANEL);
	if (notice)
		DrawTextColor(SD_MARGIN + (SD_WIDTH - strlen(text) * FONT_CHAR_W) / 2, SD_HELP_Y + 3, text, TEXT_ACCENT);
	else
		DrawTextIcons(SD_MARGIN + (SD_WIDTH - IconTextWidth(text)) / 2, SD_HELP_Y + 3, text, TEXT_DIM);
}

// The list panel, empty, with a message
static void	SdDrawMessage(const char *message)
{
	FillRect(SD_MARGIN, SD_BODY_Y, SD_LIST_W, SD_BODY_H, UI_PANEL);
	if (message)
		DrawText(14, SD_BODY_Y + 5, message);
}

// A row of the list: its name from the character scroll (the selected one: highlighted). A folder holding a game: its
// name only, as a game.
static void	SdDrawRow(int row, int i, int sel, int scroll)
{
	char	text[SD_CHARS + 1];
	SdEntry	*e = sd_list[i];
	int		y = SD_BODY_Y + 4 + row * SD_ROW_H, n;

	FillRect(10, y - 1, SD_LIST_W - 4, SD_ROW_H, (i == sel) ? 50 : 20, (i == sel) ? 70 : 24, (i == sel) ? 130 : 40);
	for (n = 0; (n < SD_CHARS) && e->name[scroll + n]; n++)
		text[n] = e->name[scroll + n];
	if ((e->dir == SD_FOLDER) && (n < SD_CHARS))
		text[n++] = '/';
	text[n] = 0;
	if (e->dir == SD_FOLDER)
		DrawTextColor(14, y, text, TEXT_ACCENT);
	else
		DrawText(14, y, text);
}

static void	SdDrawPos(int sel);

static void	SdDrawList(int sel, int top)
{
	int		i;

	SdDrawMessage(sd_count ? NULL : "No games");
	for (i = 0; (i < SD_ROWS) && (top + i < sd_count); i++)
		SdDrawRow(i, top + i, sel, 0);
	SdDrawPos(sel);
	// Under the cover: nothing until it is shown (SdDrawInfo)
	SdDrawInfo(NULL);
}

static void	SdDrawCover(bool found)
{
	RECT	rect = {SD_COVER_X + (COVER_W - sd_cov_w) / 2, SD_BODY_Y, sd_cov_w, COVER_H};

	if (found)
	{
		FillRect(SD_COVER_X, SD_BODY_Y, COVER_W, COVER_H, UI_BG);	// (the sides of a narrower cover)
		LoadImage(&rect, (uint32_t *)SD_IMAGE_ADRS);
		DrawSync(0);
	}
	else
	{
		FillRect(SD_COVER_X, SD_BODY_Y, COVER_W, COVER_H, UI_PANEL);
		DrawTextColor(SD_COVER_X + (COVER_W - 8 * FONT_CHAR_W) / 2, SD_BODY_Y + (COVER_H - FONT_CHAR_H) / 2, "NO COVER",
			TEXT_DIM);
	}
}

static void	SdDrawScreen(void)
{
	FillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, UI_BG);	// (the menu's logo too)
	SdDrawTitle();
	if (!sd_view)	// (list: the cover's place; the grid draws its own)
	{
		SdDrawCover(false);
		SdDrawInfo(NULL);
	}
	SdDrawHelp(NULL);
}

// A game (a .cue of the directory the PicoStation is in) mounted and booted by the cartridge (cheats: the cheat screen
// With PICO_FEATURE_PREPARE: a game's .cue opened (PICO_PREPARE), its status read until it is done (the
// mount is quick then). 0 opened, else why not (FatFs' result; 255 no answer, 254 audio only: nothing to boot).
static int	Pico_Prepare(int index)
{
	uint8_t	*s;
	int		tries, result;

	if (!Pico_Command(PICO_PREPARE, index) && !Pico_Command(PICO_PREPARE, index))
		return (255);
	for (tries = 0; tries < 40; tries++)	// (well under a second: 4 seconds at most)
	{
		DelayUs(100000);
		if (!Pico_Read(PICO_MOUNT_STATUS_LBA, 1))
			return (255);	// (not answering: it stopped, each read already tried for long)
		s = PICO_SECTOR(0);
		if (memcmp(s, "MNT1", 4) || ((s[4] | (s[5] << 8)) != index))
			continue;	// (all 0 until it is opened)
		if ((result = s[6] | (s[7] << 8)))
			return ((result < 254) ? result : 253);
		return ((s[8] & 1) ? 254 : 0);
	}
	return (255);
}

// first), with its settings (sd_game_edc, sd_game_bootfix: SdGameLoad). The PicoStation's menu disc is gone (RESET
// held 1 second brings it back): the game's name is kept for the menu after a reset. This screen stays up meanwhile
// (the drive unlocked, the game read: the log kept, console_hold). False if the game couldn't be opened (PICO_FEATURE_PREPARE:
// still on its menu disc, nothing changed); else returns if the boot failed, on the menu with the log.
static bool	SdBoot(SdEntry *e, bool cheats)
{
	char	text[SD_GAME_CHARS + 1];
	int		i;

	FillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, UI_BG);
	SdDrawTitle();
	FillRect(SD_MARGIN, SD_BODY_Y, SD_WIDTH, SD_BODY_H, UI_PANEL);
	for (i = 0; (i < SD_GAME_CHARS) && e->name[i]; i++)
		text[i] = e->name[i];
	text[i] = 0;
	DrawTextColor(SD_MARGIN + 8, SD_BODY_Y + 8, text, TEXT_ACCENT);
	DrawText(SD_MARGIN + 8, SD_BODY_Y + 30, "Booting the game...");
	FillRect(SD_MARGIN, SD_HELP_Y, SD_WIDTH, SD_HELP_H, UI_PANEL);
	DrawSync(0);
	// The PicoStation's processing as the game has it, both sent each time: it keeps them until power off (with
	// PICO_FEATURE_PREPARE, through the mount too)
	Pico_Command(PICO_EXTENDED, PICO_EXT_SKIP_BOOT | (sd_game_bootfix ? 0 : 1));
	Pico_Command(PICO_EXTENDED, PICO_EXT_SKIP_EDC | (sd_game_edc ? 0 : 1));
	if (sd_devcart & PICO_FEATURE_PREPARE)	// Opened first: the mount is quick, a game that can't be is told
	{
		if ((i = Pico_Prepare(e->index)))
		{
			SdLog(PICO_EV_PREPARE, i);
			return (false);
		}
		Pico_Command(PICO_MOUNT, e->index);
		DelayUs(200000);
	}
	else
	{
		Pico_Command(PICO_MOUNT, e->index);
		DelayUs(500000);
	}
	for (i = 0; (i < PICO_GAME_NAME - 1) && e->name[i]; i++)
		pico_game->name[i] = e->name[i];
	pico_game->name[i] = 0;
	pico_game->name[PICO_GAME_NAME - 1] = 0;
	pico_game->magic = PICO_GAME_MAGIC;
	console_hold = true;
	console_held_n = 0;
	PrintConsole(e->name);
	BootDisc(cheats);
	SdLog(PICO_EV_BOOT, 0);
	console_hold = false;	// Failed: the menu, with the log
	MenuDraw();
	PrintUSBStatus();
	ConsoleReplay();
	return (true);
}

// An entry's cover at SD_COVER_ADRS, its game ID in sd_serial (COV_NONE, COV_IMAGE or COV_ID): one of the last ones
// shown (kept), or asked for where it was found (SdEntry.cov). A folder shown as a game: its cover beside it, or the
// cover of the game in it (the folder may turn out to be a folder: SdEntry.dir).
static int	SdFetchCover(SdEntry *e)
{
	SdCached	*c;
	int			found = COV_NONE, k, t0 = SD_TIME();

	for (k = 0; k < SD_CACHED; k++)
		if ((SdCache(k)->dir == (int16_t)sd_dir) && (SdCache(k)->index == e->index))
		{
			c = SdCache(k);
			memcpy((void *)SD_COVER_ADRS, c->data, COVER_FILE);
			strcpy(sd_serial, c->serial);
			found = c->image ? COV_IMAGE : COV_ID;
			break;
		}
	if (k == SD_CACHED)
	{
		sd_serial[0] = 0;
		if ((e->cov == SD_COV_UNKNOWN) || (e->cov == SD_COV_BESIDE))
			found = Pico_Cover(e->index);
		SD_DEBUG(7, SD_TIME_SINCE(t0));	// (test: display lines, the cover beside it)
		t0 = SD_TIME();
		if ((found == COV_NONE) && (e->cov != SD_COV_NONE) && !(sd_devcart & PICO_FEATURE_DETAILS) &&
			((e->dir == SD_UNKNOWN) || (e->dir == SD_GAME) || (e->dir == SD_DISCS)))	// (with the details: looked into already)
		{
			found = SdInFolder(e, Pico_Cover);
			if (found != COV_NONE)
				e->cov = SD_COV_INSIDE;
		}
		else if (found != COV_NONE)
			e->cov = SD_COV_BESIDE;
		if (found == COV_NONE)
			e->cov = SD_COV_NONE;
		SD_DEBUG(8, SD_TIME_SINCE(t0));	// (the folder looked into)
		if (found != COV_NONE)		// Kept (the oldest one replaced)
		{
			c = SdCache(sd_cache_next);
			sd_cache_next = (sd_cache_next + 1) % SD_CACHED;
			c->dir = sd_dir;
			c->index = e->index;
			c->image = (found == COV_IMAGE);
			strcpy(c->serial, sd_serial);
			memcpy(c->data, (void *)SD_COVER_ADRS, COVER_FILE);
		}
	}
	return (found);
}

// List view: the selected entry's cover drawn, its details under it (a folder found: its row drawn again)
static void	SdShowCover(int sel, int top)
{
	SdEntry	*e = sd_list[sel];
	int		found = SdFetchCover(e), all = SD_TIME();

	if (e->dir == SD_FOLDER)
		SdDrawRow(sel - top, sel, sel, 0);
	if (found == COV_IMAGE)
		SdCoverImage();
	SdDrawCover(found == COV_IMAGE);
	SdDrawInfo(e);
	SD_DEBUG(13, SD_TIME_SINCE(all));	// (test: display lines, the whole cover shown)
	(void)all;
}

/****************************************/
// Grid view (SELECT: list or grid): GRID_COLS x GRID_ROWS covers of GRID_T pixels, the selected one framed, its
// details under them. Covers are read one at a time while no button is pressed (the selected one first), made small
// once and kept in VRAM (SD_SLOTS of them, beside the displayed area, under the font): a page seen again is drawn at
// once.

#define	GRID_COLS		4
#define	GRID_ROWS		2
#define	GRID_N			(GRID_COLS * GRID_ROWS)
#define	GRID_T			64
#define	GRID_X0			(SD_MARGIN + 9)
#define	GRID_STEP_X		74
#define	GRID_Y0			(SD_BODY_Y + 6)
#define	GRID_STEP_Y		70
#define	GRID_PANEL_H	(GRID_ROWS * GRID_STEP_Y + 6)
#define	GRID_INFO_Y		(SD_BODY_Y + GRID_PANEL_H + 4)
#define	GRID_INFO_H		(SD_BODY_Y + SD_BODY_H - GRID_INFO_Y)

// Cover flow: the selected cover in the middle (full size), the one before and the one after smaller (the grid's) at
// its sides, the name under them
#define	FLOW_PANEL_H	(COVER_H + 8)
#define	FLOW_INFO_Y		(SD_BODY_Y + FLOW_PANEL_H + 4)
#define	FLOW_CENTER_Y	(SD_BODY_Y + 4)
#define	FLOW_SIDE_Y		(FLOW_CENTER_Y + (COVER_H - GRID_T) / 2)
#define	FLOW_GAP		20		// Between the middle cover and a side one
#define	FLOW_CENTER_X	((SCREEN_WIDTH - sd_cov_w) / 2)

#define	SD_SLOTS		49			// 7 x 7 covers of 64 x 64 at x 320-767 (the font from 768), y 0-447
#define	SD_SLOT_X(_s_)	(320 + ((_s_) % 7) * GRID_T)
#define	SD_SLOT_Y(_s_)	(((_s_) / 7) * GRID_T)

typedef struct
{
	int16_t	dir;	// sd_dir when it was made (-1: none)
	int16_t	index;
	uint8_t	image;	// 0: no image (only its game ID)
	char	serial[CHEAT_DB_SERIAL + 1];
} SdSlot;

static SdSlot	sd_slots[SD_SLOTS];
static int		sd_slot_next;

static int	SdSlotFind(const SdEntry *e)
{
	int	s;

	for (s = 0; s < SD_SLOTS; s++)
		if ((sd_slots[s].dir == (int16_t)sd_dir) && (sd_slots[s].index == e->index))
			return (s);
	return (-1);
}

// The cover at SD_COVER_ADRS made small (sd_thumb_w x GRID_T) into a slot of VRAM
static void	SdMakeThumb(int s)
{
	RECT	rect = {SD_SLOT_X(s), SD_SLOT_Y(s), sd_thumb_w, GRID_T};

	SdCoverScaled(sd_thumb_w, GRID_T);
	LoadImage(&rect, (uint32_t *)SD_IMAGE_ADRS);
	DrawSync(0);
}

static void	SdGridPos(int i, int *x, int *y)
{
	*x = GRID_X0 + (i % GRID_N % GRID_COLS) * GRID_STEP_X;
	*y = GRID_Y0 + (i % GRID_N / GRID_COLS) * GRID_STEP_Y;
}

// A cell: its frame (selected: highlighted), its cover, or its name (not read yet, no cover, a folder)
static void	SdGridCell(int i, int sel)
{
	SdEntry	*e = sd_list[i];
	int		x, y, s = SdSlotFind(e);

	SdGridPos(i, &x, &y);
	FillRect(x - 3, y - 3, GRID_T + 6, GRID_T + 6, UI_PANEL);
	x += (GRID_T - sd_thumb_w) / 2;	// (a cover's shape: narrower on PAL)
	if (i == sel)
		FillRect(x - 3, y - 3, sd_thumb_w + 6, GRID_T + 6, UI_ACCENT);
	if ((s >= 0) && sd_slots[s].image)
	{
		RECT	rect = {SD_SLOT_X(s), SD_SLOT_Y(s), sd_thumb_w, GRID_T};

		MoveImage(&rect, x, y);
		DrawSync(0);
		return;
	}
	FillRect(x, y, sd_thumb_w, GRID_T, 30, 36, 60);	// (not read yet: empty)
	if (e->dir == SD_FOLDER)
		DrawTextColor(x + (sd_thumb_w - 6 * FONT_CHAR_W) / 2, y + 28, "FOLDER", TEXT_ACCENT);
	else if ((s >= 0) || (e->cov == SD_COV_NONE))
	{
		DrawTextColor(x + (sd_thumb_w - 2 * FONT_CHAR_W) / 2, y + 22, "NO", TEXT_DIM);
		DrawTextColor(x + (sd_thumb_w - 5 * FONT_CHAR_W) / 2, y + 34, "COVER", TEXT_DIM);
	}
}

// Text 1.5 times as big (12 x 12): the font made smooth (each pixel: the share of the font's pixels it covers) once in
// VRAM, under the cover slots (x 320-575, y 448-507: 21 characters a row, in one texture page), drawn as sprites.
// Made in RAM (the game area at 0x80010000: free on this screen, the screensaver's later) and loaded at once.
#define	MID_CHAR		12
#define	MID_CHARS		((SD_WIDTH - 16) / MID_CHAR)	// 24 per line
#define	MID_FONT_X		320
#define	MID_FONT_Y		448
#define	MID_FONT_W		256		// (256 x 60: a whole number of 16-word DMA blocks)
#define	MID_FONT_H		60
#define	MID_FONT_RAM	0x80010000
#define	MID_PER_ROW		21

static SPRT		mid_sprites[MID_CHARS];
static DR_TPAGE	mid_tpage;
static bool		mid_font_ok;

static void	MidFontMake(void)
{
	RECT		font = {FONT_U, FONT_V, 256, FONT_N_FONT_LINE * FONT_CHAR_H};
	RECT		all = {MID_FONT_X, MID_FONT_Y, MID_FONT_W, MID_FONT_H};
	uint16_t	*src = (uint16_t *)SD_IMAGE_ADRS, *out = (uint16_t *)MID_FONT_RAM;
	int			c, x, y, u, v;

	StoreImage(&font, (uint32_t *)src);
	DrawSync(0);
	memset(out, 0, MID_FONT_W * MID_FONT_H * 2);
	for (c = 0; c < FONT_N_CHAR_PER_LINE * FONT_N_FONT_LINE; c++)
	{
		const uint16_t	*g = src + (c / FONT_N_CHAR_PER_LINE) * FONT_CHAR_H * 256 + (c % FONT_N_CHAR_PER_LINE) * FONT_CHAR_W;
		uint16_t		*o = out + (c / MID_PER_ROW) * MID_CHAR * MID_FONT_W + (c % MID_PER_ROW) * MID_CHAR;

		// In 1/24ths: a font pixel is 3 of them, a new one 2: 4 sub-squares each, counted where the font is set
		for (y = 0; y < MID_CHAR; y++)
			for (x = 0; x < MID_CHAR; x++)
			{
				int	n = 0, i;

				for (v = 2 * y; v < 2 * y + 2; v++)
					for (u = 2 * x; u < 2 * x + 2; u++)
						n += (g[(v / 3) * 256 + u / 3] != 0);
				i = n * 31 / 4;
				o[y * MID_FONT_W + x] = n ? (i | (i << 5) | (i << 10)) : 0;
			}
	}
	// One transfer: PSn00bSDK's LoadImage rounds a transfer up to whole DMA blocks (16 words), the words past the
	// image then reach the GPU as commands (one 12 x 12 character at a time hung the GPU, depending on the RAM after it)
	LoadImage(&all, (uint32_t *)out);
	DrawSync(0);
	mid_font_ok = true;
}

static void	DrawTextMid(int x, int y, const char *text, int n, int r, int g, int b)
{
	SPRT	*last = NULL;
	int		k = 0, i;

	if (!mid_font_ok)
		MidFontMake();
	setDrawTPage(&mid_tpage, 1, 0, getTPage(2, 0, MID_FONT_X, 256));
	for (i = 0; (i < n) && (k < MID_CHARS); i++)
	{
		int	c = text[i] - FONT_FIRST_CHAR;

		if ((c >= 0) && (c < FONT_N_CHAR_PER_LINE * FONT_N_FONT_LINE))
		{
			SPRT	*p = &mid_sprites[k++];

			setSprt(p);
			setXY0(p, x + i * MID_CHAR, y);
			setWH(p, MID_CHAR, MID_CHAR);
			setUV0(p, (c % MID_PER_ROW) * MID_CHAR, MID_FONT_Y - 256 + (c / MID_PER_ROW) * MID_CHAR);
			setRGB0(p, r, g, b);
			catPrim(last ? (void *)last : (void *)&mid_tpage, p);
			last = p;
		}
	}
	if (!last)
		return;
	termPrim(last);
	DrawOTag((uint32_t *)&mid_tpage);
	DrawSync(0);
}

// The selected entry's name under the grid, big: up to 2 lines (cut at a space), centered
static void	SdGridInfo(int sel)
{
	const char	*name = sd_list[sel]->name;
	int			len = strlen(name), n, y, line;

	int			info_y = (sd_view == SD_VIEW_FLOW) ? FLOW_INFO_Y : GRID_INFO_Y;
	int			info_h = SD_BODY_Y + SD_BODY_H - info_y;

	FillRect(SD_MARGIN, info_y, SD_WIDTH, info_h, UI_PANEL);
	y = (len <= MID_CHARS) ? info_y + (info_h - MID_CHAR) / 2 : info_y + (info_h - 2 * MID_CHAR - 2) / 2;
	for (line = 0; (line < 2) && *name; line++)
	{
		while (*name == ' ')
			name++;
		len = strlen(name);
		n = (len <= MID_CHARS) ? len : MID_CHARS;
		if ((len > MID_CHARS) && !line)	// (the first line: cut after its last space)
		{
			int	k;

			for (k = MID_CHARS; (k > 0) && (name[k] != ' '); k--)
				;
			n = k ? k : MID_CHARS;
		}
		DrawTextMid(SD_MARGIN + (SD_WIDTH - n * MID_CHAR) / 2, y + line * (MID_CHAR + 2), name, n, TEXT_WHITE);
		name += n;
	}
}

// The selected entry's place, at the right of the title bar
static void	SdDrawPos(int sel)
{
	char	text[12];
	int		x;

	sprintf(text, "%d/%d", sd_count ? sel + 1 : 0, sd_count);
	// From the title's end (more than 5 characters, e.g. 100/250: over its end), at the right of the bar
	x = SCREEN_WIDTH - 12 - strlen(text) * FONT_CHAR_W;
	x = (x < SD_TITLE_END) ? x - 4 : SD_TITLE_END;
	FillRect(x, SD_TITLE_Y, SCREEN_WIDTH - SD_MARGIN - x, SD_TITLE_H, UI_BAR);
	DrawTextColor(SCREEN_WIDTH - 12 - strlen(text) * FONT_CHAR_W, SD_TITLE_Y + 3, text, TEXT_DIM);
}

// The page of the selected entry
static void	SdGridDraw(int sel)
{
	int	i, first = sel - sel % GRID_N;

	FillRect(SD_MARGIN, SD_BODY_Y, SD_WIDTH, GRID_PANEL_H, UI_PANEL);
	FillRect(SD_MARGIN, SD_BODY_Y + GRID_PANEL_H, SD_WIDTH, GRID_INFO_Y - SD_BODY_Y - GRID_PANEL_H, UI_BG);
	if (!sd_count)
		DrawText(SD_MARGIN + 8, SD_BODY_Y + 6, "No games");
	for (i = first; (i < first + GRID_N) && (i < sd_count); i++)
		SdGridCell(i, sel);
	if (sd_count)
		SdGridInfo(sel);
	else
		FillRect(SD_MARGIN, GRID_INFO_Y, SD_WIDTH, GRID_INFO_H, UI_PANEL);
	SdDrawPos(sel);
}

// A cover just read (SdFetchCover) kept small in a slot (the oldest one replaced)
static void	SdSlotAdd(SdEntry *e, int found)
{
	int	s = sd_slot_next;

	if (found == COV_NONE)
		return;
	sd_slot_next = (sd_slot_next + 1) % SD_SLOTS;
	sd_slots[s].dir = sd_dir;
	sd_slots[s].index = e->index;
	sd_slots[s].image = (found == COV_IMAGE);
	strcpy(sd_slots[s].serial, sd_serial);
	if (found == COV_IMAGE)
		SdMakeThumb(s);
}

static bool	flow_center_ok;	// The middle cover drawn (else: read when nothing is pressed)

// The cover before (side -1) or after (1) the middle one: small, its shape (none: nothing)
static void	SdFlowSide(int sel, int side)
{
	int		i = sel + side, x = (side < 0) ? FLOW_CENTER_X - FLOW_GAP - sd_thumb_w : FLOW_CENTER_X + sd_cov_w + FLOW_GAP, s;
	SdEntry	*e;

	FillRect(x, FLOW_SIDE_Y, sd_thumb_w, GRID_T, UI_PANEL);
	if ((i < 0) || (i >= sd_count))
		return;
	e = sd_list[i];
	s = SdSlotFind(e);
	if ((s >= 0) && sd_slots[s].image)
	{
		RECT	rect = {SD_SLOT_X(s), SD_SLOT_Y(s), sd_thumb_w, GRID_T};

		MoveImage(&rect, x, FLOW_SIDE_Y);
		DrawSync(0);
		return;
	}
	FillRect(x, FLOW_SIDE_Y, sd_thumb_w, GRID_T, 30, 36, 60);
	if (e->dir == SD_FOLDER)
		DrawTextColor(x + (sd_thumb_w - 6 * FONT_CHAR_W) / 2, FLOW_SIDE_Y + 28, "FOLDER", TEXT_ACCENT);
	else if ((s >= 0) || (e->cov == SD_COV_NONE))
	{
		DrawTextColor(x + (sd_thumb_w - 2 * FONT_CHAR_W) / 2, FLOW_SIDE_Y + 22, "NO", TEXT_DIM);
		DrawTextColor(x + (sd_thumb_w - 5 * FONT_CHAR_W) / 2, FLOW_SIDE_Y + 34, "COVER", TEXT_DIM);
	}
}

// The middle cover: from the covers kept in RAM (read: load), else empty until read
static void	SdFlowCenter(int sel, bool load)
{
	SdEntry	*e = sd_list[sel];
	int		found = COV_NONE, k;

	for (k = 0; (k < SD_CACHED) && !load; k++)
		if ((SdCache(k)->dir == (int16_t)sd_dir) && (SdCache(k)->index == e->index))
			load = true;
	flow_center_ok = load || (e->dir == SD_FOLDER) || (e->cov == SD_COV_NONE);
	if (load && (e->dir != SD_FOLDER))
	{
		found = SdFetchCover(e);
		if (SdSlotFind(e) < 0)
			SdSlotAdd(e, found);
	}
	if (found == COV_IMAGE)
	{
		RECT	rect = {FLOW_CENTER_X, FLOW_CENTER_Y, sd_cov_w, COVER_H};

		SdCoverImage();
		LoadImage(&rect, (uint32_t *)SD_IMAGE_ADRS);
		DrawSync(0);
		return;
	}
	FillRect(FLOW_CENTER_X, FLOW_CENTER_Y, sd_cov_w, COVER_H, 30, 36, 60);
	if (e->dir == SD_FOLDER)
		DrawTextColor((SCREEN_WIDTH - 6 * FONT_CHAR_W) / 2, FLOW_CENTER_Y + 60, "FOLDER", TEXT_ACCENT);
	else if (flow_center_ok)
		DrawTextColor((SCREEN_WIDTH - 8 * FONT_CHAR_W) / 2, FLOW_CENTER_Y + 60, "NO COVER", TEXT_DIM);
}

static void	SdFlowDraw(int sel)
{
	FillRect(SD_MARGIN, SD_BODY_Y, SD_WIDTH, FLOW_PANEL_H, UI_PANEL);
	FillRect(SD_MARGIN, SD_BODY_Y + FLOW_PANEL_H, SD_WIDTH, FLOW_INFO_Y - SD_BODY_Y - FLOW_PANEL_H, UI_BG);
	if (!sd_count)
	{
		DrawText(SD_MARGIN + 8, SD_BODY_Y + 6, "No games");
		FillRect(SD_MARGIN, FLOW_INFO_Y, SD_WIDTH, SD_BODY_Y + SD_BODY_H - FLOW_INFO_Y, UI_PANEL);
		SdDrawPos(sel);
		return;
	}
	SdFlowSide(sel, -1);
	SdFlowSide(sel, 1);
	SdFlowCenter(sel, false);
	SdGridInfo(sel);
	SdDrawPos(sel);
}

// One cover of the flow read (the middle one first, then its sides): false if they all are
static bool	SdFlowLoadOne(int sel)
{
	SdEntry	*e;
	int		side, i;

	if (!flow_center_ok)
	{
		SdFlowCenter(sel, true);
		return (true);
	}
	for (side = -1; side <= 1; side += 2)
	{
		i = sel + side;
		if ((i < 0) || (i >= sd_count))
			continue;
		e = sd_list[i];
		if ((e->dir == SD_FOLDER) || (e->cov == SD_COV_NONE) || (SdSlotFind(e) >= 0))
			continue;
		SdSlotAdd(e, SdFetchCover(e));
		SdFlowSide(sel, side);
		return (true);
	}
	return (false);
}

// One cover of the page read, made small and drawn (the selected one first): false if they all are
static bool	SdGridLoadOne(int sel)
{
	int		first = sel - sel % GRID_N, k, i, found;
	SdEntry	*e;

	for (k = -1; k < GRID_N; k++)
	{
		i = (k < 0) ? sel : first + k;
		if (i >= sd_count)
			break;
		e = sd_list[i];
		if ((e->dir == SD_FOLDER) || (e->cov == SD_COV_NONE) || (SdSlotFind(e) >= 0))
			continue;
		found = SdFetchCover(e);
		SdSlotAdd(e, found);
		SdGridCell(i, sel);
		if (i == sel)
			SdGridInfo(sel);
		return (true);
	}
	return (false);
}

// The details of a game shown still for a moment (SdDrawInfo): its discs (a folder looked into) and its .cht file's
// cheats (counted as the cheat screen lists them). About half a second for a .cue, two for a folder: once per entry.
static void	SdLoadDetails(int sel, int top)
{
	SdEntry	*e = sd_list[sel];
	int		ok, n = 0, on = 0, i;

	ok = SdEntryCheats(e);
	if (e->dir != SD_FILE)
	{
		if ((e->dir == SD_FOLDER) && !sd_view)	// (looked into: not a game)
			SdDrawRow(sel - top, sel, sel, 0);
		else if ((e->dir == SD_FOLDER) && (sd_view == SD_VIEW_GRID))
			SdGridCell(sel, sel);
	}
	if (ok)
	{
		n = SdCheatText((char *)SD_CHT_ADRS, 0, (uint8_t *)SD_COVER_ADRS, (uint8_t *)SD_IMAGE_ADRS);
		for (i = 0; i < n; i++)
			on += cheat_on[i];
	}
	e->cht = n;
	e->cht_on = on;
	if (e->discs == SD_UNKNOWN_COUNT)	// (a folder that isn't a game)
		e->discs = 0;
	if (sd_view)
		SdGridInfo(sel);
	else
		SdDrawInfo(e);
}

// An entry's first letter (upper case), the first entry with the same one (L1/R1)
static int	SdFirst(int i)
{
	int	c = sd_list[i]->name[0];

	return (((c >= 'a') && (c <= 'z')) ? c - 32 : c);
}

static int	SdLetter(int i)
{
	while (i && (SdFirst(i - 1) == SdFirst(i)))
		i--;
	return (i);
}

/****************************************/
// Options (START): the view, and how the PicoStation serves the games it mounts (sent before each mount: SdBoot), as
// found in its RC5 firmware: EDC correction (its Mode 2 data sectors' error codes made again: for images whose codes
// are wrong, e.g. patched ones) and the bootsector fix (the disc's first 5 sectors, its licence and region, replaced
// by its own: for a Japanese console). Both on at power on (and RESET held one second): the cartridge's default, as
// before (its own menu turns them off unless chosen, the bootsector fix always on a non-Japanese console). Saved in the
// cartridge flash with the screensavers (saver.h OPT_*).

#define	SD_OPTIONS		3	// (EDC correction, bootsector fix: each game's, SdGameScreen)
#define	SD_OPTIONS_HELP	"LEFT/RIGHT:CHANGE " ICON_CIRCLE "DONE"

static void	SdDrawOptions(int cur)
{
	static const char *const	names[SD_OPTIONS] = {"VIEW", "GAME SETTINGS SCREEN", "OPEN AT POWER ON"};
	static const char *const	views[SD_VIEWS] = {"LIST", "GRID", "COVER FLOW"};
	char						text[20];
	int							i, y;

	FillRect(SD_MARGIN, SD_BODY_Y, SD_WIDTH, SD_BODY_H, UI_PANEL);
	DrawTextColor(SD_MARGIN + 8, SD_BODY_Y + 8, "OPTIONS", TEXT_ACCENT);
	for (i = 0; i < SD_OPTIONS; i++)
	{
		y = SD_BODY_Y + 30 + i * 18;
		if (i == cur)
			FillRect(SD_MARGIN + 4, y - 4, SD_WIDTH - 8, 16, UI_SELECT);
		DrawText(SD_MARGIN + 12, y, names[i]);
		sprintf(text, "< %s >", !i ? views[sd_view] : (i == 1) ? ((saver_cfg[SAVER_OPTS] & OPT_NO_GAME_SCREEN) ? "OFF" : "ON") :
			((saver_cfg[SAVER_OPTS] & OPT_PICO_START) ? "ON" : "OFF"));
		if (i == cur)
			DrawText(SD_MARGIN + SD_WIDTH - 12 - strlen(text) * FONT_CHAR_W, y, text);
		else
			DrawTextColor(SD_MARGIN + SD_WIDTH - 12 - strlen(text) * FONT_CHAR_W, y, text, TEXT_DIM);
	}
	FillRect(SD_MARGIN, SD_HELP_Y, SD_WIDTH, SD_HELP_H, UI_PANEL);
	DrawTextIcons(SD_MARGIN + (SD_WIDTH - IconTextWidth(SD_OPTIONS_HELP)) / 2, SD_HELP_Y + 3, SD_OPTIONS_HELP, TEXT_DIM);
}

// Until O or START (the screen drawn again by the caller); saved when changed (the cartridge's flash, devcart.cfg)
static void	SdOptions(void)
{
	uint32_t	pad, old = 0xFFFF;	// (START, that opened it: released first)
	uint8_t		opts = saver_cfg[SAVER_OPTS];
	uint16_t	chip;
	int			cur = 0, i;

	SdDrawOptions(cur);
	for (;;)
	{
		VSync(0);
		if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
			USB_Process(0, 0, NULL, 0, NULL);
		pad = MenuPad();
		if (pad == 0xFFFF)
			continue;
		i = pad & ~old;
		old = pad;
		if (i & (PADRright | PADstart))
			break;
		if (i & (PADLup | PADLdown))
			cur = (cur + ((i & PADLup) ? SD_OPTIONS - 1 : 1)) % SD_OPTIONS;
		else if ((i & (PADLleft | PADLright | PADRdown)) && !cur)
			sd_view = (sd_view + ((i & PADLleft) ? SD_VIEWS - 1 : 1)) % SD_VIEWS;
		else if (i & (PADLleft | PADLright | PADRdown))
			saver_cfg[SAVER_OPTS] ^= (cur == 1) ? OPT_NO_GAME_SCREEN : OPT_PICO_START;
		else
			continue;
		SdDrawOptions(cur);
	}
	SdDrawHelp("Saving the settings...");
	if (saver_cfg[SAVER_OPTS] != opts)
		SaveDeviceId(&chip);
	SdCfgKeep();	// (and on the SD card, with the extra commands: they win there)
}

// A game's cheats in cheat_list / cheat_on (none ticked), the cheat screen's title in title: the cartridge's (by its
// game ID, sd_serial: SdFetchCover) and its .cht beside its .cue. Returns how many.
static int	SdGameCheats(const SdEntry *e, char *title)
{
	int		n = 0, k;

	strcpy(title, "CHEATS: ");
	if (sd_serial[0])
		n = CheatFind(sd_serial, cheat_list, title + 8);
	memset(cheat_on, 0, sizeof(cheat_on));
	if (!n)
	{
		for (k = 0; (k < CHEAT_DB_TITLE - 1) && e->name[k]; k++)
			title[8 + k] = e->name[k];
		title[8 + k] = 0;
	}
	if (Pico_Cheats(e->index))
		n = SdCheatText((char *)SD_CHT_ADRS, n, (uint8_t *)SD_COVER_ADRS, (uint8_t *)SD_IMAGE_ADRS);
	return (n);
}

// The settings screen before a game is booted (X): boot, its cheats, the PicoStation's EDC correction and bootsector
// fix (each game its own, kept in its .cfg on the SD card with PICO_FEATURE_GAME_CFG)
#define	SD_GAME_ITEMS	4
#define	SD_GAME_HELP	ICON_CROSS "SELECT " ICON_START "BOOT " ICON_CIRCLE "BACK"

static void	SdDrawGame(const SdEntry *e, int cur, int n, const char *note)
{
	static const char *const	names[SD_GAME_ITEMS] = {"BOOT THE GAME", "CHEATS", "EDC CORRECTION", "BOOTSECTOR FIX"};
	char						text[SD_GAME_CHARS + 1];
	int							i, k, on, x, y;

	FillRect(SD_MARGIN, SD_BODY_Y, SD_WIDTH, SD_BODY_H, UI_PANEL);
	for (k = 0; (k < SD_GAME_CHARS) && e->name[k]; k++)
		text[k] = e->name[k];
	text[k] = 0;
	DrawTextColor(SD_MARGIN + 8, SD_BODY_Y + 8, text, TEXT_ACCENT);
	for (i = 0; i < SD_GAME_ITEMS; i++)
	{
		y = SD_BODY_Y + 30 + i * 18;
		if (i == cur)
			FillRect(SD_MARGIN + 4, y - 4, SD_WIDTH - 8, 16, UI_SELECT);
		DrawText(SD_MARGIN + 12, y, names[i]);
		if (!i)
			continue;
		if ((i == 1) && n)
		{
			for (on = k = 0; k < n; k++)
				on += cheat_on[k] ? 1 : 0;
			sprintf(text, "%d OF %d ON", on, n);
		}
		else if (i == 1)
			strcpy(text, "NONE");
		else
			sprintf(text, "< %s >", ((i == 2) ? sd_game_edc : sd_game_bootfix) ? "ON" : "OFF");
		x = SD_MARGIN + SD_WIDTH - 12 - strlen(text) * FONT_CHAR_W;
		if (i == cur)
			DrawText(x, y, text);
		else
			DrawTextColor(x, y, text, TEXT_DIM);
	}
	DrawTextColor(SD_MARGIN + 12, SD_BODY_Y + 30 + SD_GAME_ITEMS * 18 + 10, note, TEXT_DIM);
	FillRect(SD_MARGIN, SD_HELP_Y, SD_WIDTH, SD_HELP_H, UI_PANEL);
	DrawTextIcons(SD_MARGIN + (SD_WIDTH - IconTextWidth(SD_GAME_HELP)) / 2, SD_HELP_Y + 3, SD_GAME_HELP, TEXT_DIM);
}

// e (a .cue of the directory the PicoStation is in): its settings read, then the screen until it is booted (1: the
// cheats ticked in the code engine's table, the settings in sd_game_*) or left (0, O). The settings are saved when
// changed (-1: back, but they couldn't be).
static int	SdGameScreen(SdEntry *e)
{
	char		title[CHEAT_DB_TITLE + 8];
	const char	*note;
	uint32_t	pad, old = 0xFFFF;	// (the button that opened it: released first)
	int			cur = 0, n, ret, i;
	bool		edc, bootfix;

	SdDrawHelp("Reading the game's settings...");
	SdFetchCover(e);	// (its game ID: the cartridge's cheats)
	n = SdGameCheats(e, title);
	SdGameLoad(e);
	edc = sd_game_edc;
	bootfix = sd_game_bootfix;
	note = (sd_devcart & PICO_FEATURE_GAME_CFG) ? "Kept for this game on the SD card" :
		"This PicoStation can't keep them";
	SdDrawGame(e, cur, n, note);
	for (;;)
	{
		VSync(0);
		if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
			USB_Process(0, 0, NULL, 0, NULL);
		pad = MenuPad();
		if (pad == 0xFFFF)
			continue;
		i = pad & ~old;
		old = pad;
		if (i & PADRright)
		{
			ret = 0;
			break;
		}
		if ((i & PADstart) || ((i & PADRdown) && !cur))
		{
			ret = 1;
			break;
		}
		if (i & (PADLup | PADLdown))
			cur = (cur + ((i & PADLup) ? SD_GAME_ITEMS - 1 : 1)) % SD_GAME_ITEMS;
		else if ((i & PADRdown) && (cur == 1) && n)	// The cheat screen: START there boots
		{
			if (CheatScreen(n, title))
			{
				ret = 1;
				break;
			}
			old = 0xFFFF;	// (back: the screen restored, the buttons released first)
		}
		else if ((i & (PADLleft | PADLright | PADRdown)) && (cur == 2))
			sd_game_edc = !sd_game_edc;
		else if ((i & (PADLleft | PADLright | PADRdown)) && (cur == 3))
			sd_game_bootfix = !sd_game_bootfix;
		else
			continue;
		SdDrawGame(e, cur, n, note);
	}
	if ((sd_game_edc != edc) || (sd_game_bootfix != bootfix))
	{
		SdDrawHelp("Saving the game's settings...");
		if (!SdGameSave(e) && !ret)
			ret = -1;
	}
	if (ret == 1)
		CheatApply(n);	// (none ticked: no table)
	return (ret);
}

// The root's entry selected now (in a folder: the folder's), for the next opening
#define	SD_KEEP_SELECTION(_len_, _name_)	(SdKeepPlace(_len_, _name_), \
	*(volatile uint32_t *)SD_LAST_ADRS = SD_LAST_MAGIC | (sd_view << 16) | (depth ? parent_sel[0] : sel))

// START + SELECT on the menu with the PicoStation's menu disc in the drive. Returns after a boot that failed (its
// message in the menu's log) or when closed: the menu is drawn again (MenuScreen).
void	SdBrowser(void)
{
	uint32_t	pad, old = 0xFFFF;	// (the buttons that opened it: released first)
	int			sel = 0, top = 0, held = 0, depth = 0, still = 0, scroll = 0, cover = -1, notice = 0, idle = 0, i;
	int			grid_page = -1, grid_sel = 0;	// (grid: the page and the selection drawn)
	uint16_t	parent_sel[16];
	bool		redraw = true, chosen;	// (chosen: booted without the settings screen: the cheats chosen with Triangle,
										// or the screen set off in the options)
	SdEntry		*e;

	sd_path[0] = 0;
	for (i = 0; i < SD_CACHED; i++)	// (no covers kept: the RAM after a power on is anything)
		SdCache(i)->dir = -1;
	for (i = 0; i < SD_SLOTS; i++)
		sd_slots[i].dir = -1;
	mid_font_ok = false;	// (made again: VRAM may have been used since)
	icons_ok = false;
	sd_cov_w = (GetVideoMode() == MODE_PAL) ? 106 : COVER_W;
	sd_thumb_w = (GetVideoMode() == MODE_PAL) ? 54 : GRID_T;
	sd_view = ((*(volatile uint32_t *)SD_LAST_ADRS & 0xFF000000) == SD_LAST_MAGIC) ?
		((*(volatile uint32_t *)SD_LAST_ADRS >> 16) & 3) : 0;
	sd_view = (sd_view < SD_VIEWS) ? sd_view : 0;
#ifdef MONITOR_IN_RAM
	*(volatile uint16_t *)0x1F801114 = 0x0100;	// (test: timer 1 counts display lines, SD_TIME)
#endif
	// With the extra commands: the settings on the SD card win (before the list: the
	// PicoStation's answers share the sector buffer)
	sd_devcart = 0;
	sd_cfg_view = -1;
	sd_place_folder[0] = sd_place_name[0] = 0;	// (none without devcart.cfg: the RAM's selection)
	if (Pico_DevcartInfo(&sd_devcart, NULL))
	{
		uint8_t		opts = saver_cfg[SAVER_OPTS];
		uint16_t	chip;

		if (sd_devcart & PICO_FEATURE_IDS)	// (the covers with their game's ID)
			Pico_Command(PICO_EXTENDED, PICO_EXT_DEVCART_IDS | 1);
		SdCfgLoad();
		sd_cfg_view = sd_view;
		sd_cfg_opts = saver_cfg[SAVER_OPTS] & SD_CFG_OPTS;
		if ((saver_cfg[SAVER_OPTS] ^ opts) & OPT_PICO_START)	// (the SD card's wins: the flash has it from now)
			SaveDeviceId(&chip);
	}
	SdDrawScreen();
	SdDrawHelp("Reading the SD card...");
	if (!Pico_List(PICO_GOTO_ROOT, 0) ||
		((sd_count == 1) && sd_list[0]->dir && !strcmp(sd_list[0]->name, "No SD Card")))
	{
		MenuScreen(true);
		PrintConsole(sd_count ? "No SD card in the PicoStation" : "PicoStation not answering");
		return;
	}
	SdDrawHelp(NULL);
	// The place kept in devcart.cfg (it wins over the RAM's): its folders opened again, its entry selected
	if (sd_place_folder[0] || sd_place_name[0])
	{
		const char	*p = sd_place_folder;
		int			n, len;

		while ((*p == '/') && (depth < sizeof(parent_sel) / sizeof(parent_sel[0])))
		{
			for (n = 1; p[n] && (p[n] != '/'); n++)
				;
			for (i = 0; i < sd_count; i++)
				if ((sd_list[i]->dir != SD_FILE) && (strlen(sd_list[i]->name) == n - 1) &&
					!strncmp(sd_list[i]->name, p + 1, n - 1))
					break;
			if ((i == sd_count) || ((len = strlen(sd_path)) + n >= sizeof(sd_path)))
				break;		// (gone: as far as it is)
			memcpy(sd_path + len, p, n);
			sd_path[len + n] = 0;
			parent_sel[depth++] = i;
			if (!Pico_List(PICO_GOTO_DIR, sd_list[i]->index))
			{
				depth = 0;	// (back at the root)
				sd_path[0] = 0;
				Pico_List(PICO_GOTO_ROOT, 0);
				break;
			}
			p += n;
		}
		for (i = 0; i < sd_count; i++)
			if (!strncmp(sd_list[i]->name, sd_place_name, SD_PLACE_NAME - 1))
			{
				sel = i;
				break;
			}
		top = (sel > SD_ROWS / 2) ? sel - SD_ROWS / 2 : 0;
		top = (top + SD_ROWS > sd_count) ? ((sd_count > SD_ROWS) ? sd_count - SD_ROWS : 0) : top;
		SdDrawTitle();
	}
	// The entry selected last time (after a reset too)
	else if (((*(volatile uint32_t *)SD_LAST_ADRS & 0xFF000000) == SD_LAST_MAGIC) &&
		((*(volatile uint32_t *)SD_LAST_ADRS & 0xFFFF) < sd_count))
	{
		sel = *(volatile uint32_t *)SD_LAST_ADRS & 0xFFFF;
		top = (sel > SD_ROWS / 2) ? sel - SD_ROWS / 2 : 0;
		top = (top + SD_ROWS > sd_count) ? ((sd_count > SD_ROWS) ? sd_count - SD_ROWS : 0) : top;
	}

	for (;;)
	{
		if (redraw)
		{
			if (sel < top)
				top = sel;
			else if (sel >= top + SD_ROWS)
				top = sel - SD_ROWS + 1;
			if (sd_view == SD_VIEW_LIST)
				SdDrawList(sel, top);
			else if (sd_view == SD_VIEW_FLOW)
				SdFlowDraw(sel);
			else if (sel / GRID_N != grid_page)	// Grid: another page drawn whole, or the frame moved
			{
				SdGridDraw(sel);
				grid_page = sel / GRID_N;
			}
			else
			{
				SdGridCell(grid_sel, sel);
				SdGridCell(sel, sel);
				SdGridInfo(sel);
				SdDrawPos(sel);
			}
			grid_sel = sel;
			redraw = false;
			still = scroll = 0;
		}
		VSync(0);

		// The computer is still answered (screenshots, memory), a boot it asks for is left for the menu
		if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
			USB_Process(0, 0, NULL, 0, NULL);

		// Screensaver running (saver.h): nothing else drawn
		if (saver_on)
			SaverFrame();
		// Grid, cover flow: the covers read while no button is pressed
		else if (sd_count && sd_view)
		{
			e = sd_list[sel];
			still++;
			if ((still >= 4) && (sd_view == SD_VIEW_GRID))	// (no details: only its name is shown)
				SdGridLoadOne(sel);
			else if (still >= 4)
				SdFlowLoadOne(sel);
		}
		// List: the selection still: its cover after a moment, its long name scrolled
		else if (sd_count)
		{
			e = sd_list[sel];
			still++;
			if ((still == 20) && (cover != e->index))	// (a third of a second: not while scrolling)
			{
				cover = e->index;
				SdShowCover(sel, top);
			}
			if ((still == 60) && (cover == e->index) && (e->cht == SD_UNKNOWN_COUNT) && (e->dir != SD_FOLDER) &&
				!(saver_cfg[SAVER_OPTS] & OPT_NO_DETAILS))
				SdLoadDetails(sel, top);	// (a second still: its discs and cheats, unless set off in the Manager)
			if ((still > 60) && !(still & 7) && (strlen(e->name) + (e->dir == SD_FOLDER) > SD_CHARS))
			{
				scroll = (scroll + SD_CHARS < strlen(e->name) + (e->dir == SD_FOLDER)) ? scroll + 1 : 0;
				still = scroll ? still : 0;	// (back to the start: a pause)
				SdDrawRow(sel - top, sel, sel, scroll);
			}
		}

		if (notice && !saver_on && !--notice)	// (the notice shown long enough)
			SdDrawHelp(NULL);

		// Buttons pressed (the D-pad repeats when held)
		pad = MenuPad();
		if (pad == 0xFFFF)
			continue;
		// Screensaver (saver.h, as the Manager set it for this screen): no button for a while (a still picture can
		// burn into a CRT); a button puts the screen back (and does nothing else)
		if (pad)
		{
			idle = 0;
			if (saver_on)
			{
				SaverStop();
				old = pad;
				continue;
			}
		}
		else if (!saver_on && SaverFrames(SAVER_PICO) && (++idle > SaverFrames(SAVER_PICO)))
			SaverStart(SAVER_PICO);
		if (pad & (PADLup | PADLdown | PADLleft | PADLright))
			held++;
		else
			held = 0;
		i = pad & ~old;
		if ((held > 20) && !(held & 3))
			i |= pad & (PADLup | PADLdown | PADLleft | PADLright);
		old = pad;
		if (!i)
			continue;
		chosen = false;
		if (sd_count && (i & PADRup) && (sd_list[sel]->dir == SD_DISCS))	// A game's discs: chosen first (their
			i = PADRdown;													// cheats: each disc's)

		if (sd_count && (i & (PADLup | PADLdown | PADLleft | PADLright)))
		{
			int	was = sel;

			if (sd_view == SD_VIEW_GRID)	// Grid: up and down a row, left and right an entry
				sel += (i & PADLup) ? -GRID_COLS : (i & PADLdown) ? GRID_COLS : (i & PADLleft) ? -1 : 1;
			else if (sd_view == SD_VIEW_FLOW)	// Cover flow: an entry
				sel += (i & (PADLup | PADLleft)) ? -1 : 1;
			else
				sel += (i & PADLup) ? -1 : (i & PADLdown) ? 1 : (i & PADLleft) ? -SD_ROWS : SD_ROWS;
			sel = (sel < 0) ? 0 : (sel >= sd_count) ? sd_count - 1 : sel;
			redraw = (sel != was);	// (at the first or last entry: nothing, its cover and details stay)
		}
		else if (sd_count && (i & (PADL1 | PADR1)))	// L1/R1: the previous/next first letter
		{
			int	k = SdLetter(sel), was = sel;

			if (i & PADR1)
			{
				for (k = sel; (k < sd_count) && (SdFirst(k) == SdFirst(sel)); k++)
					;
				sel = (k < sd_count) ? k : sd_count - 1;
			}
			else
				sel = ((k == sel) && sel) ? SdLetter(sel - 1) : k;
			redraw = (sel != was);
		}
		else if (i & PADstart)	// START: the options (the view, the game settings screen)
		{
			SdOptions();
			old = 0xFFFF;	// (the buttons released first)
			SdDrawScreen();
			grid_page = -1;
			cover = -1;
			redraw = true;
		}
		else if (i & PADselect)	// SELECT: list, grid, cover flow
		{
			sd_view = (sd_view + 1) % SD_VIEWS;
			SdDrawScreen();
			grid_page = -1;
			cover = -1;
			redraw = true;
		}
		else if (i & PADRright)	// Circle: the parent directory, or closed at the root
		{
			if (!depth)
				break;
			SdDrawHelp("Reading the SD card...");
			if (!Pico_List(PICO_GOTO_PARENT, 0))
				break;
			for (i = strlen(sd_path); i && (sd_path[--i] != '/'); )	// The last directory taken off
				;
			sd_path[i] = 0;
			sel = parent_sel[--depth];
			sel = (sel < sd_count) ? sel : 0;
			top = (sel > SD_ROWS / 2) ? sel - SD_ROWS / 2 : 0;
			SdDrawTitle();
			SdDrawHelp(NULL);
			redraw = true;
			cover = -1;	// (entry numbers: of this directory)
			grid_page = -1;
			continue;	// (i: not the buttons any more)
		}
		else if (sd_count && (i & PADRup))	// Triangle: the game's cheats: the cartridge's (by its ID, with its cover)
		{									// and the SD card's (its .cht file, next to its .cue)
			char	title[CHEAT_DB_TITLE + 8];
			int		n = 0, k;

			e = sd_list[sel];
			if (sd_view)	// (grid: its game ID only)
				SdFetchCover(e);
			else if (cover != e->index)
			{
				cover = e->index;
				SdShowCover(sel, top);
			}
			strcpy(title, "CHEATS: ");
			if (sd_serial[0])
				n = CheatFind(sd_serial, cheat_list, title + 8);
			memset(cheat_on, 0, sizeof(cheat_on));
			if (!n)
			{
				for (k = 0; (k < CHEAT_DB_TITLE - 1) && e->name[k]; k++)
					title[8 + k] = e->name[k];
				title[8 + k] = 0;
			}
			SdDrawHelp("Reading the SD card's cheats...");
			if (SdEntryCheats(e))
				n = SdCheatText((char *)SD_CHT_ADRS, n, (uint8_t *)SD_COVER_ADRS, (uint8_t *)SD_IMAGE_ADRS);
			if ((e->dir == SD_FOLDER) && !sd_view)	// (looked into: not a game)
				SdDrawRow(sel - top, sel, sel, 0);
			else if ((e->dir == SD_FOLDER) && (sd_view == SD_VIEW_GRID))
				SdGridCell(sel, sel);
			if (e->dir == SD_DISCS)		// (a game's discs, found now: chosen first, as with X)
			{
				SdDrawHelp(NULL);
				i = PADRdown;
			}
			else
			{
				SdDrawHelp(n ? NULL : "No cheats available for this game");
				if (!n)
				{
					notice = 150;
					continue;
				}
				old = 0xFFFF;	// (back: the buttons released first)
				if (!CheatScreen(n, title))
					continue;	// (O: the screen as it was)
				// START: the cheats chosen are in the code engine's table, the game booted (as X, with its saved
				// settings: no settings screen)
				chosen = true;
				i = PADRdown;
			}
		}
		if (sd_count && (i & PADRdown))	// X: open, or the game's settings screen then boot
		{
			int		r = 1;

			chosen |= (saver_cfg[SAVER_OPTS] & OPT_NO_GAME_SCREEN) != 0;	// (set off: booted at once)

			e = sd_list[sel];
			if (e->dir)
			{
				bool	game = (e->dir != SD_FOLDER);	// (shown as a game: booted if it holds one)
				int		len = strlen(sd_path);

				if (depth >= sizeof(parent_sel) / sizeof(parent_sel[0]))
					continue;
				// Its name in the path first: the list read next replaces the entries
				if (len + strlen(e->name) < sizeof(sd_path) - 1)
				{
					sd_path[len] = '/';
					strcpy(sd_path + len + 1, e->name);
				}
				SdDrawHelp("Reading the SD card...");
				if (!Pico_List(PICO_GOTO_DIR, e->index))
					break;
				if (game && (sd_count == 1) && (sd_list[0]->dir == SD_FILE))
				{
					if (chosen)
						SdGameLoad(sd_list[0]);
					if (chosen || ((r = SdGameScreen(sd_list[0])) > 0))
					{
						SD_KEEP_SELECTION(len, (sd_path[len] == '/') ? sd_path + len + 1 : "");
						if (SdBoot(sd_list[0], false))
							return;	// (failed: its message is in the log)
						r = -2;		// (not opened: back)
					}
					// O: back in the directory, as it was
					sd_path[len] = 0;
					SdDrawHelp("Reading the SD card...");
					if (!Pico_List(PICO_GOTO_PARENT, 0))
						break;
					SdDrawScreen();
					old = 0xFFFF;	// (the button that closed it: released first)
					if (r < 0)
					{
						SdDrawHelp((r == -2) ? "This game can't be opened" : "The settings couldn't be saved");
						notice = 150;
					}
					redraw = true;
					cover = -1;
					grid_page = -1;
					continue;
				}
				// A game's discs (.cue files only): each one of them shows the game's count
				for (i = 0; (i < sd_count) && (sd_list[i]->dir == SD_FILE); i++)
					;
				if (i == sd_count)
					for (i = 0; i < sd_count; i++)
						sd_list[i]->discs = (sd_count < SD_UNKNOWN_COUNT) ? sd_count : SD_UNKNOWN_COUNT - 1;
				parent_sel[depth++] = sel;
				sel = top = 0;
				SdDrawTitle();
				SdDrawHelp(NULL);
				redraw = true;
				cover = -1;
				grid_page = -1;
			}
			else
			{
				if (chosen)
					SdGameLoad(e);
				if (chosen || ((r = SdGameScreen(e)) > 0))
				{
					SD_KEEP_SELECTION(strlen(sd_path), e->name);
					if (SdBoot(e, false))
						return;	// (failed: its message is in the log)
					r = -2;		// (not opened: the screen again)
				}
				SdDrawScreen();	// (O: the screen as it was)
				old = 0xFFFF;	// (the button that closed it: released first)
				if (r < 0)
				{
					SdDrawHelp((r == -2) ? "This game can't be opened" : "The settings couldn't be saved");
					notice = 150;
				}
				redraw = true;
				cover = -1;
				grid_page = -1;
			}
		}
	}
	SD_KEEP_SELECTION(strlen(sd_path), sd_count ? sd_list[sel]->name : "");
	MenuScreen(true);
}

#endif
