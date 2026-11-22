/*
* PS1 DEV CART Transfer tool: what its files share (main.c was split by area: usbio.c, pcdrv.c, yaroze.c,
* pstools.c, memcard.c, crash.c, flash.c, info.c, hotreload.c).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_H_
#define _PS1TRANSFER_H_

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <sys/stat.h>
#include "lodepng.h"
#include "gdbserver.h"
#include "synthbridge.h"
#include "usblink.h"
#include "rename.h"
#include "profiler.h"
#include "timlive.h"
#include "../ps1-tools/cardtool/cardserver.h"
#include "../monitor/monitor.h"

uint8_t	CRC8_Compute(uint8_t *data, uint32_t size);	// crc.h, defined in usbio.c

#ifdef _WIN32
#include "ftd2xx.h"
#include "serialport.h"

// The cartridge: an FTDI chip through FTDI's D2XX driver, or a serial port (com: a USB device that isn't an FTDI chip,
// e.g. a Raspberry Pi Pico, or an FTDI chip through its virtual COM port with -com, see serialport.c)
typedef struct
{
	FT_HANDLE	ft;
	HANDLE		com;
	char		serial[64];	// (com: the USB serial number of the port's device, "" if it has none)
} UsbDevice;

extern UsbDevice	*ftdi;
extern const char	*com_name;
#else
#include <ftdi.h>
#include <libusb.h>		// (libusb_get_device: libftdi 1.5's ftdi.h doesn't include it any more)
extern struct ftdi_context *ftdi;

#define	sleep_ms(ms)	usleep(ms * 1000)
#endif

#ifdef _WIN32
#include <direct.h>
#define	MAKE_DIR(_d_)	_mkdir(_d_)
#else
#include <sys/stat.h>
#define	MAKE_DIR(_d_)	mkdir((_d_), 0755)
#endif

/****************************************/
// Constants

// usbio.c
#define	USB_READ_SIZE	2048	// Reads are done by whole packet (USB_ReadData waits for all the bytes)

#define	USB_WRITE_SIZE	65536	// A whole packet in one write (16KB: 585 KB/s upload, 2KB writes: 555), the USB driver handles the chip buffer flow control

#define	USB_LATENCY_MS	1		// FTDI default is 16ms, which delayed each packet CRC/ACK byte sent by the PS1

#define	USB_READ_TIMEOUT	5		// Seconds without data before a read fails (e.g. PS1 running a program, not answering)

#define	USB_READ_POLL_MS	100		// Windows: FT_Read returns after this time so the timeout can be checked

#define	MAX_PACKET_SIZE	2048	// 2k max for reliability

#define	RESET_QUIET_MS	2500	// -r after a stopped transfer: the PS1 quiet this long (its acknowledgment wait ended)

#define	RESET_DRAIN_S	12		// (at most: a program sending all the time, e.g. its printf)

// Execute command param: TTY mode set by the kernel hook when the program starts, see usb.h
#define	EXEC_TTY_MODE_USB	0	// printf/TTY redirected to USB (default)

#define	EXEC_TTY_MODE_BIOS	1	// BIOS TTY device kept untouched

#define	EXEC_TTY_MODE_SIO	2	// Serial port TTY driver installed (like the Net Yaroze boot disc)

#define	EXEC_TTY_MODE_LINK	3	// The same TTY driver on the USB link instead of the serial port (-relay)

// The firmware stores in the kernel hook params: is_enabled (a program runs), big packet size (log2) and features,
// the original firmware leaves 0 in the last 2 bytes
#define	KERNEL_HOOK_PARAMS_ADRS	0x8000DF70

#define	FEATURE_LIVE_ACCESS		(1 << 0)	// 'D'/'U'/'V' also while a program runs, see usb.h

#define	FEATURE_HOOK_BIG_PACKETS	(1 << 2)	// The kernel hook and the debug monitor also use big packets

// The menu shows the device ID (USB serial number, only readable from the computer), sent with the 'I'
// command when the menu runs (the firmware saves it in the cartridge flash when it changes)
#define	FEATURE_DEVICE_ID		(1 << 1)

#define	FEATURE_BOOT_DISC		(1 << 3)	// 'C' command at the menu: the CD drive unlocked and the disc booted

#define	FEATURE_DISC_ID			(1 << 4)	// The booted disc's executable at DISC_ID_ADRS ("game=" of -info)

#define	FEATURE_SCREENSAVER		(1 << 6)	// 'S' command at the menu: the screensavers' settings (-saver)

#define	SAVER_FLASH_ADRS		0x1F01F020	// Where the menu saves them: "SSVR", the menu's, the PicoStation screen's

#define	DISC_ID_ADRS			0x8000DEE0	// (see usb.h: DiscId)

#define	DISC_ID_MAGIC			0x43534944

#define	DEVICE_ID_SIZE			16

// Bytes read ahead: what USB_ReadByteNoWait took in one go (all that was waiting: one byte at a time, a driver call
// each, the profiler and the frame capture couldn't keep up with a program's samples and file server requests, the
// reports never came), and the bytes PC_IsFileServerRequest gave back. Read first.
// One for each cartridge open (the USB link relay switches between two: USB_SetHandle).
#define	AHEAD_SIZE	8192

#define	ahead		(AheadOf()->buf)

#define	ahead_start	(AheadOf()->start)

#define	ahead_end	(AheadOf()->end)

// Escaped answers (see usb.h): 'Q' before each command, the firmware answers "DLE g" / "DLE b" instead of 'G' / 'B', so
// text a program prints on the USB tty just before the answer can't be taken for it. Known per cartridge (the relay
// opens two): asked once with a 'Q' alone, answered "DLE q" by a firmware that does it (the original and older PS1 DEV
// CART firmwares ignore it).
#define	ESC_HANDLES		4

#define	ESC_WAIT_MS		200		// The kernel hook answers at its next interrupt (a frame at most), the menu at once

// pcdrv.c
#define	PCDRV_MAX_FD	16

#define	PCDRV_MAX_TRANSFER	(16 << 20)	// Bytes of one file server request (the PS1 has 2 MB, 8 MB on dev units)

// "@input": a file of the lines given to ps1transfer's standard input (e.g. by the PS1 Dev Cart Manager), which the
// program reads to get commands while the file server runs (the PS1 DEV CART MOVIE TESTER): a read returns what has
// arrived, at once (0 bytes if nothing). A thread reads the input into a ring (one writer, one reader).
#define	INPUT_RING	8192

// yaroze.c
#define	LIBPS_DEFAULT_ADRS	0x80010000

// Accepts the original "libps.exe" from the Net Yaroze CD-ROM (PS-X EXE) or the stripped raw "libps.bin"
// The Net Yaroze boot disc runs the start code of the library (libps.exe + 0x30: callbacks, pads with
// InitPAD on the buffers of GetPadBuf, ...) before the program, which doesn't do it itself (without it, the pads read
// "no controller"). ps1transfer executes a stub that calls it, then jumps to the program entry point.
#define	LIBPS_INIT_OFFSET	0x30

#define	LIBPS_INIT_CODE		0x27BDFFE8		// addiu sp, sp, -24: first instruction of the start code (checked)

#define	LIBPS_STUB_SIZE		24

#define	ECOFF_MAGIC			0x0162		// MIPS little endian

#define	ECOFF_STYP_NOLOAD	(0x80 | 0x400)	// .bss, .sbss: cleared by the program's start code

#define	AUTO_MAX_DATA		256

#define	AUTO_MAX_DEPTH		4

#define	RAM_USER_START		0x80010000

// PS1 DEV CART firmware's stack (8KB), the firmware above it (0x801EB000 from firmware 1.5, 0x801EF000 in 1.0-1.4; it
// receives the files): data must stay below (the Makefile's FW_STACK_BOTTOM)
#define	RAM_FIRMWARE		0x801E8800

// pstools.c
#define	VIEWER_FILENAME		"tmdviewer.ps-exe"

#define	VIEWER_DATA_ADRS	0x80100000	// Above the viewer program and its buffers

#define	VIEWER_DATA_END		0x801E0000	// Below the PS1 DEV CART firmware (0x801E8800, RAM_FIRMWARE) which receives the data

#define	VIEWER_MAGIC		0x56444D54	// "TMDV"

#define	VIEWER_MAX_TIM		8

#define	VIEWER_MAX_TOD		16			// TOD animations (-tod)

#define	VIEWER_TOD_TABLE	0x40		// Number of TODs, then address, size and name (24 bytes) of each

#define	VIEWER_HEADER_SIZE	0x400		// (the shared state of -mdllive at 0x300, see mdllive.c)

// The PS1 tools (model viewer, sound tester) are looked for next to the ps1transfer executable
#define	TOOL_NAME_MAX	64

#define	TESTER_FILENAME		"soundtest.ps-exe"

#define	TESTER_DATA_ADRS	0x80100000

#define	TESTER_DATA_END		0x801E0000	// Below the PS1 DEV CART firmware (0x801E8800, RAM_FIRMWARE) which receives the data

#define	TESTER_MAGIC		0x54444E53	// "SNDT"

#define	TESTER_MAX_FILES	8

#define	TESTER_HEADER_SIZE	0x120	// File table, SoundPos, SoundSync, SoundFx2 (soundtest.c)

#define	TIMVIEW_FILENAME	"timviewer.ps-exe"

#define	TIMVIEW_DATA_ADRS	0x80100000

#define	TIMVIEW_DATA_END	0x801E0000	// Below the PS1 DEV CART firmware (0x801E8800, RAM_FIRMWARE) which receives the data

#define	TIMVIEW_MAGIC		0x564D4954	// "TIMV"

#define	TIMVIEW_MAX_FILES	32

#define	TIMVIEW_NAME_LEN	24

#define	TIMVIEW_HEADER_SIZE	0x800

// memcard.c
#define	CARD_FILENAME		"cardtool.ps-exe"

#define	LINKTEST_FILENAME	"linktest.ps-exe"	// Link cable tester (linktest folder), "-link" option

#define	PADTEST_FILENAME	"padtest.ps-exe"		// Control tester (padtest folder), "-padtest" option

#define	CARD_DATA_ADRS		0x80100000

#define	CARD_MAGIC			0x44524143	// "CARD"

#define	CARD_IMAGE_OFFSET	0x100

#define	CARD_SIZE			(128 * 1024)

#define	CARD_GME_HEADER		3904		// DexDrive .gme files: header before the raw image

#define	CARD_EOT			4			// Printed by the PS1 program at the end: console mode is stopped

#define	MC_BLOCK		8192

#define	MC_FRAME		128

#define	MC_STATE_FREE	0xA0

#define	MC_STATE_FIRST	0x51

#define	MC_STATE_MIDDLE	0x52

#define	MC_STATE_LAST	0x53

#define	CARD_MODE_SERVER	3

#define	CARD_SECTOR			128

#define	CARD_BLOCK_SECTORS	64

#define	CS_FIELD(_f_)		(CARDSRV_MAILBOX_ADRS + offsetof(CardServerMailbox, _f_))

// crash.c
#define	BIOS_TCBH_PTR		0x80000108	// Task control block header pointer, its first word points to the current TCB

#define	CRASH_STACK_SIZE	1024

#define	CRASH_MAX_CALLERS	16

#define	RAM_START			0x80000000

#define	RAM_END				0x80200000

#define	SYMBOL_MAX_OFFSET	0x10000		// Further from a symbol, the address is shown without name

// ECOFF symbol table (Net Yaroze executable, not stripped): the symbolic header (HDRR) gives the external symbols
// (functions, variables, and the Net Yaroze library's functions as absolute symbols: VSync, DrawSync...) and the
// local ones of each file descriptor (static functions and variables)
#define	ECOFF_HDRR_MAGIC	0x7009

#define	ECOFF_ST_GLOBAL		1

#define	ECOFF_ST_STATIC		2

#define	ECOFF_ST_PROC		6

#define	ECOFF_ST_STATICPROC	14

// flash.c
#define	FLASH_ROM_SIZE		(128 * 1024)

#define	FLASH_IMAGE_ADRS	0x80100000		// See ps1-tools/ar_flash/ar_flash.c

#define	FLASH_ID_SECTOR		0x1F000			// Device ID sector, not compared

#define	FLASHER_FILENAME	"ar_flash.ps-exe"

#define	FLASH_TIMEOUT		60				// Seconds for the flasher to report

#define	FLASH_CHEATS		0xB000			// Cheats of games (cheatmenu.h CHEAT_DB_ROM, gui/cheatdb.py ROM_CHEATS)

#define	FLASH_CHEATS_SIZE	0x5000

#define	FLASH_SECTOR_SIZE	0x1000

// What -flash keeps of the cartridge's flash (the ROM file's areas are empty in a release ROM)
#define	FLASH_KEEP_CHEATS	1		// Its cheats (not with -flashcheats: the ROM's own, e.g. the Manager's Cheats page)

#define	FLASH_KEEP_SETTINGS	2		// Its device ID and settings sector (screensavers, options)

// info.c
#define	SAVER_LOGO	0x04

#define	SAVER_OFF	0x08

// Then the menu's options (a third byte): -picodetails off sets OPT_NO_DETAILS (the PicoStation screen's list without
// its discs and cheats counts: faster, nothing more read from the SD card)
#define	OPT_NO_DETAILS	0x01

// hotreload.c
#define	HOT_MAX_FILES	16

// main.c
#define	PS1TRANSFER_VERSION	"1.0"	// PS1 DEV CART release (the same as the firmware and the PS1 tools)

/****************************************/
// Types

// usbio.c
typedef struct
{
	void	*owner;
	uint8_t	buf[AHEAD_SIZE + 64];
	int		start, end;
} Ahead;

// pcdrv.c
// File server activity ("-pcdrvlog": pcdrv=... lines for the GUI and scripts) and the files allowed ("-pcdrvany":
// any path, else only the files of the served folder)
typedef struct
{
	char		name[128];
	uint32_t	read, written;
} PcdrvFile;

// yaroze.c
typedef struct
{
	char		path[1024];
	uint32_t	adrs, size;
} AutoData;

// pstools.c
enum
{
	SOUND_VH = 1,
	SOUND_VB,
	SOUND_VAB,	// VH followed by VB
	SOUND_SEQ,
	SOUND_VAG,
	SOUND_SQ,	// Sony's later format (1999: Ape Escape, Alundra 2...): music "SSsq", bank "SShd" + its samples (BD),
	SOUND_HD,	// played as they are by the tester
	SOUND_BD
};

// memcard.c
enum	{CARD_BACKUP = 1, CARD_RESTORE};

typedef struct
{
	int		slot;
	uint8_t	img[CARD_SIZE];			// Sectors read (or changed by the image functions)
	uint8_t	orig[CARD_SIZE];		// Sectors as read from the card
	bool	have[CARDSRV_SECTORS];
} LiveCard;

// crash.c
typedef struct
{
	uint32_t	adrs;
	char		*name;
} Symbol;

// hotreload.c
typedef struct
{
	char	*name;			// Argument: file[@where]
	time_t	mtime;			// -watch: last sent version
	long	size;
} HotFile;

// main.c
enum
{
	CMD_UNKNOWN,
	CMD_UPLOAD,
	CMD_DOWNLOAD,
	CMD_EXECUTE,
	CMD_FLASH,
	CMD_VRAM,
	CMD_CRASH,
	CMD_PEEK,
	CMD_POKE,
	CMD_MEMLIVE,
	CMD_HOT,
	CMD_INFO,
	CMD_FLASH_ROM,
	CMD_SYNTH,
	CMD_CARD_LIVE,
	CMD_RELAY,
	CMD_VLINK,
	CMD_NET,
	CMD_PROF,
	CMD_SPU,
	CMD_BOOT_DISC,
	CMD_DISC,
	CMD_SNAPSHOT,
	CMD_TIMING_TEST,
	CMD_XSERVE,
	CMD_SAVER
};

/****************************************/
// Globals

// usbio.c
extern uint32_t usb_big_packet_size;
extern bool ps1_program_running;
extern uint8_t fw_features;
extern bool vram_live;
extern bool vram_auto;
extern const char *spu_file;
extern uint32_t hook_packet_size;
extern bool hook_packet_detected;
extern bool machine_progress;
extern bool usb_quiet;

// pcdrv.c
extern FILE *PCdrvFD[PCDRV_MAX_FD];
extern bool pcdrv_log, pcdrv_any_path;
extern int console_last_char;
extern bool mon_stop_pending;

// yaroze.c
extern uint32_t libps_init;
extern uint32_t libps_adrs, libps_size;
extern AutoData auto_data[AUTO_MAX_DATA];
extern int auto_count;
extern char auto_program[1024];

// memcard.c
extern bool exit_on_eot;

// crash.c
extern Symbol *symbols;

// flash.c
extern int flash_keep;
extern char *flash_backup_dir;

/****************************************/
// Functions

// usbio.c
#ifdef _WIN32	// (libftdi has its own ftdi_* functions on Linux)
uint32_t	ftdi_read_data(UsbDevice *dev, void *data, uint32_t size);
uint32_t	ftdi_write_data(UsbDevice *dev, uint8_t *data, uint32_t size);
void	USB_Purge(UsbDevice *dev);
void	ftdi_tcioflush(UsbDevice *dev);
void	ftdi_free(UsbDevice *dev);
#endif
void	USB_SetLatency(void);
bool	USB_OpenDevice(int deviceid, char *serial);
void	USB_ListDevices(void);
void	USB_DetectPacketSize(void);
bool	USB_GetSerial(char *serial);
void	USB_SendDeviceId(void);
void	USB_Unread(const uint8_t *data, int n);
bool	USB_ReadData(uint8_t *data, uint32_t size);
uint32_t	NowMs(void);
void	PrintUploadTime(uint32_t bytes, uint32_t start_ms);
int	USB_EscapedAnswers(void);
bool	USB_Process(char cmd, uint8_t *data, uint32_t adrs, uint32_t size);
bool	USB_ReadByteNoWait(uint8_t *c);

// pcdrv.c
bool	PC_IsFileServerRequest(uint8_t type);
void	PC_FileServerCmd(int first);
void	PC_FileServer(void);

// yaroze.c
uint32_t	UploadNetYarozeStart(uint32_t exe_adrs, uint32_t entry);
bool	UploadNetYarozeLibrary(char *filename, uint32_t exe_adrs, uint32_t exe_size);
uint8_t	*EcoffToPsxExe(const uint8_t *data, int size, int *exe_size);
bool	ReadAutoScript(const char *script, int depth);
bool	UploadAutoData(uint32_t exe_adrs, uint32_t exe_size, uint32_t lib_adrs, uint32_t lib_size);

// pstools.c
char	*GetToolPath(char *argv0, char *name);
bool	Snapshot(const char *dir, const char *parts);
int	SoundFileType(char *name, uint8_t *data, int size);
bool	UploadTimData(char **files, int count);

// memcard.c
void	PrintCardDirectory(uint8_t *img);
int	CardImageCommand(int argc, char **argv, int i);
bool	UploadCardData(int mode, int slot, char *filename);
bool	UploadSoundData(char **files, int count);
bool	UploadModelData(char *model, char **tims, int tim_count, char **tods, int tod_count);
int	CardSlotParse(const char *arg, bool slot_word);
int	CardSlotArg(char *arg);
bool	CardServerRunning(void);
int	CardLiveCommand(int argc, char **argv, int i, char *argv0, bool start);

// crash.c
void	SaveLastExe(char *exe);
void	AutoLoadSymbols(char *symfile);
bool	ParseAddress(char *str, uint32_t *adrs, char *symfile);
Symbol	*FindSymbol(uint32_t adrs);
bool	VramToPng(const char *filename, const uint16_t *vram, int w, int h);
bool	CrashReport(char *symfile, const char *frame_png);

// flash.c
void	SleepMs(int ms);
uint8_t	*LoadRom(char *rom_file);
bool	FlashRom(char *rom_file, uint8_t *rom, char *argv0);

// info.c
int	SaverParse(const char *text);
void	PrintInfo(void);

// hotreload.c
bool	HotService(char cmd, uint8_t *data, uint32_t param, uint32_t size);
bool	HotReload(HotFile *files, int count, bool watch);

// main.c
void	*SystemGetFile(char *filename, int *sizeptr);
uint32_t	atohex(char *str);

#endif
