/*
* PS1USB firmware v1.4
* by OrionSoft [2024-2025]
* https://orionsoft.games/
*
* Big thanks to david4599 for the reset feature and kernel hook !
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
// Built with PSn00bSDK (open source) instead of PsyQ
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include <psxcd.h>
#include <psxspu.h>
#include "psyq_compat.h"

#include "monitor/monitor.h"
// Code with no timing (menu screen, cheat screen, disc boot, SD card browser) is optimized for size: the firmware fills
// its ROM area. The USB transfers (bus timing), the flash writes and the disc reads keep -O2.
#pragma GCC push_options
#pragma GCC optimize ("Os")
#include "display.h"
#pragma GCC pop_options
#include "usb.h"
#include "tfs.h"
#include "saver.h"
#include "icons.h"

#define KERNEL_UNUSED_START	0x8000C000
#define KERNEL_UNUSED_SIZE	0x2000
#define KERNEL_HOOK_START	(KERNEL_UNUSED_START + 0x160)

// PicoStation chain boot: set when the disc booted is the PicoStation ODE's menu, which mounts the chosen game then
// restarts the BIOS (the kernel hook is lost). The cartridge's pre-boot vector takes over again, and the menu then
// finds the marker and boots the game itself, the kernel hook kept. RAM isn't cleared by a BIOS restart; past the
// kernel hook's space (it ends at 0x8000DF7F), in the kernel's unused area no program uses.
#define	CHAIN_BOOT_ADRS		0x8000DF80
#define	CHAIN_BOOT_MAGIC	0x4E494843	// "CHIN"
#define	PICOSTATION_MENU_EXE	"SCES_313.37"	// The PicoStation menu disc's executable (SYSTEM.CNF BOOT)
static volatile uint32_t	*chain_boot = (volatile uint32_t *)CHAIN_BOOT_ADRS;	// [0] magic, [1] ~magic

// The PicoStation game the cartridge mounted (sdbrowser.h): the menu names it after a reset, as long as the
// PicoStation's menu disc isn't back (RESET held one second). Kept like the chain boot marker. The kernel's unused area
// after the kernel hook: 0x8000DF80 chain boot, 0x8000DF88 the PicoStation screen's last selection (sdbrowser.h
// SD_LAST_ADRS), 0x8000DF8C this (100 bytes), 0x8000DFF0 the menu's
// buttons from the computer (MENU_PAD_ADRS, 8 bytes), 0x8000DFF8 a disc's BIOS Load (the debug monitor's
// MON_DISC_LOAD_ADRS), 0x8000DFFC free.
#define	PICO_GAME_ADRS		0x8000DF8C
#define	PICO_GAME_MAGIC		0x454D4147	// "GAME"
#define	PICO_GAME_NAME		96
typedef struct
{
	uint32_t	magic;
	char		name[PICO_GAME_NAME];
} PicoGame;
static volatile PicoGame	*pico_game = (volatile PicoGame *)PICO_GAME_ADRS;	// (up to 0x8000DFF0)

extern uint8_t	_binary_kernel_hook_bin_start[];
extern uint32_t	*_binary_kernel_hook_bin_size;
#ifdef MONITOR_IN_RAM	// Firmware test build running from RAM: the debug monitor is copied to RAM (see Makefile)
extern uint8_t	_binary_monitor_ram_bin_start[];
extern uint32_t	*_binary_monitor_ram_bin_size;
#endif
static MonitorState	*mon_state = (MonitorState *)MONITOR_STATE_ADRS;

// Locations are hardcoded at the end of unused kernel space to make sure the original function addresses are valid
// That way, the hooks can be uninstalled when testing a new PS1 DEV CART exe with new hooks (for cart development purposes)
static uint32_t	*p_UnresolvedExceptionOld = (uint32_t *)0x8000DF78;
static uint32_t	*p_ReturnFromExceptionOld = (uint32_t *)0x8000DF7C;

// Menu screen images stored in the ROM file system (cartrom/tim.tfs), missing ones are skipped
static char	*tim_files[] = {"logo.tim", "font.tim"};

#define	FIRMWARE_VERSION	"1.5"	// Firmware version, shown on the menu (the tools': DEVCART_VERSION, devcart_ui.h)
#define	FIRMWARE_VERSION_BCD	0x15	// Same, major << 4 | minor, read by "ps1transfer -info" (MonitorState.fw_version)
// In the ROM as text: the Manager's Firmware page reads the version of a ROM file from it (used by the menu: kept)
static const char	firmware_tag[] = "Firmware: " FIRMWARE_VERSION;

/****************************************/
// Device ID: the USB serial number of the cartridge (FT245R EEPROM, only readable by the computer).
// ps1transfer sends it ('I' command) when it connects to the menu; it is saved in the last flash sector (unused by
// the ROM) when it changes, so it is shown at the next boots. Erased when the cartridge is flashed again.

#define	DEVICE_ID_FLASH		0x1F01F000	// Last 4KB sector of the SST39VF010 flash
#define	DEVICE_ID_LEN		16
#define	FLASH_CHIP_ID		0xD5BF		// SST39VF010

static char		device_id[DEVICE_ID_LEN + 1];
uint8_t			device_id_rx[DEVICE_ID_LEN];	// Received by the 'I' command, see usb.h

// The CPU write buffer can let a following read reach the cartridge before the writes: wait a bit (about 50us)
// after each flash command, like the flasher waits a frame (otherwise the chip ID read returned the ROM data)
static void	FlashDelay(void)
{
	volatile int	i;

	for (i = 0; i < 1000; i++)
		;
}

static void	FlashCommand(uint8_t cmd)
{
	*((volatile uint8_t *)0x1F005555) = 0xAA;
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*((volatile uint8_t *)0x1F005555) = cmd;
	FlashDelay();
}

// Waits for the end of a flash erase/program: the data reads back as written (timeout in loops)
static bool	FlashWait(volatile uint8_t *adrs, uint8_t value, int timeout)
{
	while (timeout--)
		if (*adrs == value)
			return (true);
	return (false);
}

// The screensavers' settings (saver.h) after the device ID: "SSVR", the menu's, the PicoStation screen's, the
// menu's options (OPT_*)
#define	SAVER_FLASH_OFS		32

// Erases the last flash sector and writes "DVID" + the device ID (when known), "SSVR" + the screensavers' settings
// (runs from RAM, with interrupts disabled). Returns 0 when done, or the step that failed: 1 unknown chip (*chip),
// 2 erase, 3 program.
static int	SaveDeviceId(uint16_t *chip)
{
	volatile uint8_t	*sector = (volatile uint8_t *)DEVICE_ID_FLASH;
	uint8_t				data[SAVER_FLASH_OFS + 7];
	bool				ok = true;
	int					i;

	memset(data, 0xFF, sizeof(data));	// (as erased: not written)
	if (device_id[0])
	{
		memcpy(data, "DVID", 4);
		memcpy(data + 4, device_id, DEVICE_ID_LEN);
	}
	memcpy(data + SAVER_FLASH_OFS, "SSVR", 4);
	data[SAVER_FLASH_OFS + 4] = saver_cfg[SAVER_MENU];
	data[SAVER_FLASH_OFS + 5] = saver_cfg[SAVER_PICO];
	data[SAVER_FLASH_OFS + 6] = saver_cfg[SAVER_OPTS];
	EnterCriticalSection();
	FlashCommand(0x90);	// Chip ID: only write a known flash chip
	*chip = *((volatile uint16_t *)0x1F000000);
	FlashCommand(0xF0);
	if (*chip != FLASH_CHIP_ID)
	{
		ExitCriticalSection();
		return (1);
	}
	FlashCommand(0x80);	// Sector erase (25ms)
	*((volatile uint8_t *)0x1F005555) = 0xAA;
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*sector = 0x30;
	FlashDelay();
	if (!FlashWait(sector, 0xFF, 10000000))
	{
		ExitCriticalSection();
		return (2);
	}
	for (i = 0; ok && (i < sizeof(data)); i++)	// Byte program (20us each)
	{
		if (data[i] == 0xFF)
			continue;
		FlashCommand(0xA0);
		sector[i] = data[i];
		FlashDelay();
		ok = FlashWait(&sector[i], data[i], 100000);
	}
	ExitCriticalSection();
	return (ok ? 0 : 3);
}

// Firmware and Device ID panels of the menu
static void	DrawInfoLine(void)
{
	DrawInfo(0, firmware_tag + 10);	// (after "Firmware: ")
	DrawInfo(1, device_id[0] ? device_id : "--------");
}

static void	LoadDeviceId(void)
{
	volatile uint8_t	*sector = (volatile uint8_t *)DEVICE_ID_FLASH;
	int					i;

	if ((sector[0] == 'D') && (sector[1] == 'V') && (sector[2] == 'I') && (sector[3] == 'D'))
		for (i = 0; i < DEVICE_ID_LEN; i++)
			device_id[i] = sector[4 + i];
	sector += SAVER_FLASH_OFS;
	if ((sector[0] == 'S') && (sector[1] == 'S') && (sector[2] == 'V') && (sector[3] == 'R'))
	{
		saver_cfg[SAVER_MENU] = sector[4];
		saver_cfg[SAVER_PICO] = sector[5];
		saver_cfg[SAVER_OPTS] = (sector[6] == 0xFF) ? 0 : sector[6];	// (saved before the options: none)
	}
}

// 'S' command: the screensavers' settings and the options from the Manager (ps1transfer -saver, -picodetails),
// saved in the flash when they change
void	SaverSettingsReceived(void)
{
	char		text[48];
	int			step;
	uint16_t	chip = 0;

	SaverStop();
	if (!memcmp(saver_cfg, saver_rx, sizeof(saver_cfg)))
		return;
	memcpy(saver_cfg, saver_rx, sizeof(saver_cfg));
	if (!(step = SaveDeviceId(&chip)))
		PrintConsole("Menu settings saved");
	else
	{
		sprintf(text, "Settings not saved (step %d, chip %04X)", step, chip);
		PrintConsole(text);
	}
}

// 'I' command: device ID from ps1transfer (printable characters, padded with zeros)
void	DeviceIdReceived(void)
{
	char		id[DEVICE_ID_LEN + 1], text[48];
	int			i, step;
	uint16_t	chip = 0;

	memset(id, 0, sizeof(id));
	for (i = 0; (i < DEVICE_ID_LEN) && (device_id_rx[i] > ' ') && (device_id_rx[i] < 0x7F); i++)
		id[i] = device_id_rx[i];
	if (!id[0] || !memcmp(id, device_id, sizeof(id)))
		return;
	memcpy(device_id, id, sizeof(id));
	SaverStop();
	DrawInfoLine();
	if (!(step = SaveDeviceId(&chip)))
		PrintConsole("Device ID saved in the cartridge");
	else
	{
		sprintf(text, "Device ID not saved (step %d, chip %04X)", step, chip);
		PrintConsole(text);
	}
}

/****************************************/
// Profiler ("ps1transfer -prof", see monitor/monitor.h): when a program is executed, the profiler is off; with
// EXEC_PROF_FILL, the free RAM above the program (up to the firmware) is filled with PROF_FREE_PATTERN, the debug
// monitor then counts the words the program has written

#ifdef MONITOR_IN_RAM
#define	PROF_FILL_TOP	MONITOR_RAM_ADRS	// (test build: the monitor in RAM is under the firmware)
#else
#define	PROF_FILL_TOP	FW_STACK_BOTTOM		// (under the firmware's stack, itself under the firmware: see the makefile)
#endif

void	ProfFillRam(bool fill)
{
	uint32_t	lo = exe_header.t_addr + exe_header.t_size, bss = exe_header.b_addr + exe_header.b_size;
	uint32_t	*p;

	mon_state->prof_on = 0;
	mon_state->ram_lo = mon_state->ram_hi = 0;
	mon_state->prof_gpustat = 0;
	if (!fill)
		return;
	if (exe_header.b_size && (bss > lo))
		lo = bss;
	lo = (lo + 3) & ~3;
	if ((lo < 0x80010000) || (lo >= PROF_FILL_TOP))
		return;
	for (p = (uint32_t *)lo; p < (uint32_t *)PROF_FILL_TOP; p++)
		*p = PROF_FREE_PATTERN;
	mon_state->ram_lo = mon_state->ram_pos = lo;
	mon_state->ram_hi = PROF_FILL_TOP;
	mon_state->ram_dirty = mon_state->ram_run = mon_state->ram_maxrun = 0;
}

// ps1transfer -pad: the pad of player 1 driven from the computer, set up by the monitor before the program starts
// (it hooks the BIOS InitPAD, see monitor/monitor.h)
void	PadRemoteSetup(void)
{
	if (mon_state->entry && (mon_state->version >= 8))
		((void (*)(int, uint32_t *))mon_state->entry)(MON_REASON_PAD_SETUP, NULL);
}

/****************************************/

bool	PrintUSBStatus(void)
{
	if (USB_STATUS & USB_STATUS_MASK_USB_READY)
	{
		DrawStatus(0, "USB UNPLUGGED");
		PrintConsole("USB socket unplugged");
		return (false);
	}
	DrawStatus(1, "READY: WAITING FOR THE COMPUTER");
	PrintConsole("USB connected");
	return (true);
}

/****************************************/
// Disc boot (Start + Select on the menu, or "ps1transfer -bootcd"): the CD drive is unlocked first ("nocash unlock",
// https://psx-spx.consoledev.net/cdromdrive/ secret unlock commands), so burned and import discs are read without a
// modchip, then the disc is started like the BIOS does: SYSTEM.CNF (BOOT, TCB, EVENT, STACK), kernel set up, PS-X EXE.
// The unlock is kept when the lid is opened (only the drive's Reset command 1Ch forgets it): discs can be swapped.
// The kernel hook stays enabled (reset, memory access and screenshots from the computer while the game runs).

// (CD_REG and IRQ_STAT: PSn00bSDK's hwregs_c.h)
#define	DISC_EXE_TOP	PROF_FILL_TOP	// The game is loaded under the firmware

// Drive regions (test command 19h,22h) and the string of the unlock command 55h
static const char	*cd_region_id[] = {"for Europe", "for U/C", "for NETEU", "for NETNA"};
static const char	*cd_region_unlock[] = {"(Europe)", "of America", "World wide", "World wide"};
static const char	*cd_unlock_strings[] = {NULL, "Licensed by", "Sony", "Computer", "Entertainment"};

// The polled drive commands and the disc's files: cdread.h (shared with the debug monitor). Sector mode and size:
// whole sectors for the PicoStation's directory pages (sdbrowser.h), 2048 bytes otherwise.
#define	CD_DELAY_US(_us_)		DelayUs(_us_)
#define	CD_CRITICAL_ENTER()		EnterCriticalSection()
#define	CD_CRITICAL_EXIT()		ExitCriticalSection()
static uint8_t	cd_read_mode = 0x00;
static int		cd_read_sector = 2048;
#define	CD_READ_MODE			cd_read_mode
#define	CD_READ_SECTOR			cd_read_sector
// From here, no timing: optimized for size (the drive's sectors are polled at single speed: the read loop has time)
#pragma GCC optimize ("Os")
#include "cdread.h"

// After a reset the drive spins the disc up and identifies it (a few seconds): unlocked meanwhile, the unlock was
// ignored (the BIOS then retried reading forever), read meanwhile, the first sectors of the game were wrong. GetID
// (1Ah) answers twice once the disc is identified (INT2 licensed, INT5 unlicensed or no disc); before, an error.
static bool	CD_WaitReady(void)
{
	uint8_t	resp[8];
	int		i;

	for (i = 0; i < 40; i++)	// About 20 seconds
	{
		if ((CD_RawCommand(0x1A, NULL, 0, resp, sizeof(resp)) == 3) && CD_RawResponse(resp, sizeof(resp), 50000))
			return (true);
		DelayUs(500000);
	}
	return (false);
}

// Shows the result (Japanese drives and the first drive version can't be unlocked: licensed discs still boot)
static void	CD_Unlock(void)
{
	uint8_t	test = 0x22, resp[16];
	char	text[40];
	int		i, region;

	PrintConsole("Waiting for the CD drive...");
	EnterCriticalSection();
	// Reset (1Ch): the drive as after a power on, then unlocked again. Without it (or with Init 0Ah only), after the
	// reset of a game that had read the disc, the drive kept seeking: no sector came any more.
	CD_RawCommand(0x1C, NULL, 0, resp, sizeof(resp));
	DelayUs(500000);
	if (!CD_WaitReady())
	{
		ExitCriticalSection();
		PrintConsole("CD drive not ready");
		return;
	}
	if (CD_RawCommand(0x19, &test, 1, resp, sizeof(resp)) != 3)
	{
		ExitCriticalSection();
		PrintConsole("Old CD drive: not unlocked");
		return;
	}
	for (region = 0; region < 4; region++)
		if (!strcmp((char *)resp, cd_region_id[region]))
			break;
	if (region == 4)
	{
		ExitCriticalSection();
		PrintConsole(strcmp((char *)resp, "for US/AEP") ? "Japan CD drive: not unlocked" : "Debug CD drive (no lock)");
		return;
	}
	// 50h to 56h: each one answers INT5 (error 40h), even when it works
	for (i = 0; i < 7; i++)
	{
		const char	*s = (i == 5) ? cd_region_unlock[region] : (i < 5) ? cd_unlock_strings[i] : NULL;

		if (!CD_RawCommand(0x50 + i, s, s ? strlen(s) : 0, NULL, 0))
		{
			ExitCriticalSection();
			PrintConsole("CD drive not answering");
			return;
		}
	}
	ExitCriticalSection();
	sprintf(text, "CD drive unlocked (%s)", cd_region_id[region]);
	PrintConsole(text);
}

// SYSTEM.CNF value after "KEY =" (the line), NULL if the key isn't there
static char	*CnfValue(char *cnf, const char *key)
{
	int		len = strlen(key);
	char	*p;

	for (p = cnf; *p; p++)
	{
		if (((p == cnf) || (p[-1] == '\n') || (p[-1] == '\r')) && !strncmp(p, key, len) &&
			((p[len] == ' ') || (p[len] == '\t') || (p[len] == '=')))
		{
			for (p += len; (*p == ' ') || (*p == '\t') || (*p == '='); p++)
				;
			return (p);
		}
	}
	return (NULL);
}

static uint32_t	CnfHex(char *cnf, const char *key, uint32_t value)
{
	char	*p = CnfValue(cnf, key);
	int		d;

	if (!p)
		return (value);
	for (value = 0; ; p++)
	{
		if ((*p >= '0') && (*p <= '9'))
			d = *p - '0';
		else if (((*p | 0x20) >= 'a') && ((*p | 0x20) <= 'f'))
			d = (*p | 0x20) - 'a' + 10;
		else
			break;
		value = (value << 4) | d;
	}
	return (value);
}

// SYSTEM.CNF of the disc booted: in the free RAM under the VRAM saved at boot (0x80020000), only while BootDisc reads
// it, before the game is loaded (the firmware's own RAM is full)
#define	DISC_CNF_SIZE	(2048 + 1)
static char * const	disc_cnf = (char *)0x8001F000;
// A disc sector read (the PicoStation's menu disc found, a file found, an executable's header): there too, for a moment
static uint8_t * const	disc_exe = (uint8_t *)0x8001E000;

// The menu's pad (the menu, the cheat screen, the PicoStation screen), and buttons pressed from the computer (no
// controller needed): written at MENU_PAD_ADRS (ps1transfer -u -a 0x8000DFF0 -f 8 bytes): "PADR", then the buttons
// (bits 0-15, the pad's bits: 1 pressed) and the frames they are held (bits 16-31, 0: one). Read once (cleared), and
// cleared when the menu starts (the RAM after a power on: anything).
#define	MENU_PAD_ADRS		0x8000DFF0	// (after pico_game, the end of the kernel's unused area)
#define	MENU_PAD_MAGIC		0x52444150	// "PADR"

static void	MenuPadClear(void)
{
	((volatile uint32_t *)MENU_PAD_ADRS)[0] = 0;
	((volatile uint32_t *)MENU_PAD_ADRS)[1] = 0;
}

static uint32_t	MenuPad(void)
{
	volatile uint32_t	*remote = (volatile uint32_t *)MENU_PAD_ADRS;
	uint32_t			pad = PadRead(1) & 0xFFFF;
	static uint32_t		bits;
	static int			end;

	if (remote[0] == MENU_PAD_MAGIC)
	{
		bits = remote[1] & 0xFFFF;
		end = VSync(-1) + 1 + (remote[1] >> 16);
		MenuPadClear();
	}
	if (VSync(-1) < end)
		pad = ((pad == 0xFFFF) ? 0 : pad) | bits;	// (no controller: the computer's buttons alone)
	return (pad);
}

#include "cheatmenu.h"

/****************************************/
// L2 + R2 held on the PicoStation's own menu (booted with L2 + R2 on this menu, or any boot of its menu disc): back
// to this menu, no game mounted. The PicoStation menu (RC5) takes the exception vectors (0x80000040 and 0x80000080)
// and reads the pads itself: no kernel hook, monitor or hardware breakpoint runs meanwhile. Its executable is packed,
// but its start code isn't: it calls the unpacker, FlushCache, then jumps to the menu (lui t0 / addiu t0 / jr t0).
// That jump goes to PicoStubUnpacked first, copied in the cheat table's space (no cheats with this menu, and the code
// engine is gone with the kernel hook anyway): it checks the unpacked menu is the known RC5 one, points the menu's
// two calls of its pad function (both ports' buttons, active high, L2 0x100, R2 0x200) to PicoStubPad, then goes on.
// PicoStubPad: L2 + R2 only counts once they have been released for PICO_RET_ARM reads (they are held to boot the
// menu), then held PICO_RET_HOLD reads: the chain boot marker cleared, the menu's own reboot (interrupts off, BIOS
// restart, as after mounting a game): the cartridge's menu, the PicoStation's menu disc still in the drive.
#define	PICO_STUB_DATA		0x8000D8C0	// 16 words: [0] PicoStubPad's ra, [1] reads released, [2] reads held, [3] ra, [4] jal word, [5-10] trace
#define	PICO_STUB_ADRS		0x8000D900	// (to 0x8000DEDF at most: the cheat table's space)
#define	PICO_RET_ARM		30
#define	PICO_RET_HOLD		30
#define	PICO_EXE_PC0		0x8005C164	// RC5 menu executable: entry, the jump to the unpacked menu (lui t0, addiu t0)
#define	PICO_EXE_LUI_T0		0x8005C190
#define	PICO_MENU_ENTRY		0x80020294
#define	PICO_STR(_x_)		#_x_
#define	PICO_XSTR(_x_)		PICO_STR(_x_)

extern uint32_t	PicoStubStart[], PicoStubUnpacked[], PicoStubPad[], PicoStubEnd[];

__asm__(
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".text\n"
	".balign 4\n"
	".global PicoStubStart\n"
	"PicoStubStart:\n"
	".global PicoStubUnpacked\n"
	"PicoStubUnpacked:\n"			// ra = the executable's (set by the start code), kept
	"	li		$t3, 0x0C007203\n"	// jal 0x8001C80C (the menu's pad function)
	"	lui		$t1, 0x8002\n"
	"	lw		$t2, -0x62EC($t1)\n"	// 0x80019D14
	"	lw		$t4, -0x5C00($t1)\n"	// 0x8001A400
	"	lw		$t5, -0x194C($t1)\n"	// 0x8001E6B4: lui v0, 0xBFC0 (the reboot, 0x8001E69C)
	"	lui		$t0, %hi(0x8000D8C0)\n"	// (trace: the words checked)
	"	sw		$t2, %lo(0x8000D8C0 + 24)($t0)\n"
	"	sw		$t4, %lo(0x8000D8C0 + 28)($t0)\n"
	"	sw		$t5, %lo(0x8000D8C0 + 32)($t0)\n"
	"	li		$t6, 0x3C02BFC0\n"
	"	bne		$t2, $t3, 1f\n"
	"	nop\n"
	"	bne		$t4, $t3, 1f\n"
	"	nop\n"
	"	bne		$t5, $t6, 1f\n"
	"	lui		$t0, %hi(0x8000D8C0)\n"
	"	lw		$t3, %lo(0x8000D8C0 + 16)($t0)\n"	// jal PicoStubPad
	"	sw		$ra, %lo(0x8000D8C0 + 12)($t0)\n"
	"	sw		$t3, -0x62EC($t1)\n"
	"	sw		$t3, -0x5C00($t1)\n"
	"	li		$t2, 2\n"				// (trace: menu patched)
	"	sw		$t2, %lo(0x8000D8C0 + 20)($t0)\n"
	"	li		$t2, 0xA0\n"			// FlushCache A(44h)
	"	jalr	$t2\n"
	"	li		$t1, 0x44\n"
	"	lui		$t0, %hi(0x8000D8C0)\n"
	"	lw		$ra, %lo(0x8000D8C0 + 12)($t0)\n"
	"1:	lui		$t0, 0x8002\n"			// PICO_MENU_ENTRY
	"	addiu	$t0, $t0, 0x0294\n"
	"	jr		$t0\n"
	"	nop\n"
	".global PicoStubPad\n"
	"PicoStubPad:\n"				// Instead of jal 0x8001C80C (a0 kept, no stack arguments)
	"	lui		$t0, %hi(0x8000D8C0)\n"
	"	sw		$ra, %lo(0x8000D8C0)($t0)\n"
	"	.word	0x0C007203\n"			// jal 0x8001C80C
	"	nop\n"
	"	lui		$t0, %hi(0x8000D8C0)\n"
	"	lw		$ra, %lo(0x8000D8C0)($t0)\n"
	"	lw		$t2, %lo(0x8000D8C0 + 40)($t0)\n"	// (trace: the buttons, the reads)
	"	sw		$v0, %lo(0x8000D8C0 + 36)($t0)\n"
	"	addiu	$t2, $t2, 1\n"
	"	sw		$t2, %lo(0x8000D8C0 + 40)($t0)\n"
	"	lw		$t2, %lo(0x8000D8C0 + 4)($t0)\n"	// reads released
	"	andi	$t1, $v0, 0x300\n"
	"	xori	$t1, $t1, 0x300\n"		// 0: L2 + R2 held
	"	bnez	$t1, 2f\n"
	"	sltiu	$t3, $t2, " PICO_XSTR(PICO_RET_ARM) "\n"
	"	bnez	$t3, 1f\n"				// held before being released long enough: released again first
	"	lw		$t3, %lo(0x8000D8C0 + 8)($t0)\n"
	"	nop\n"
	"	addiu	$t3, $t3, 1\n"
	"	sw		$t3, %lo(0x8000D8C0 + 8)($t0)\n"
	"	sltiu	$t3, $t3, " PICO_XSTR(PICO_RET_HOLD) "\n"
	"	bnez	$t3, 9f\n"
	"	lui		$t0, 0x8001\n"
	"	sw		$zero, -0x2080($t0)\n"	// 0x8000DF80: chain boot marker cleared
	"	sw		$zero, -0x207C($t0)\n"
	"	.word	0x080079A7\n"			// j 0x8001E69C (the menu's reboot)
	"	nop\n"
	"1:	b		9f\n"
	"	sw		$zero, %lo(0x8000D8C0 + 4)($t0)\n"
	"2:	sw		$zero, %lo(0x8000D8C0 + 8)($t0)\n"
	"	beqz	$t3, 9f\n"				// (released long enough already)
	"	addiu	$t2, $t2, 1\n"
	"	sw		$t2, %lo(0x8000D8C0 + 4)($t0)\n"
	"9:	jr		$ra\n"
	"	nop\n"
	".global PicoStubEnd\n"
	"PicoStubEnd:\n"
	".set pop\n"
);

// The RC5 menu's executable just loaded: its jump to the unpacked menu goes through PicoStubUnpacked
static void	PicoReturnSetup(const struct EXEC *exe)
{
	volatile uint32_t	*code = (volatile uint32_t *)PICO_EXE_LUI_T0;
	volatile uint32_t	*data = (volatile uint32_t *)PICO_STUB_DATA;
	uint32_t			size = (uint32_t)PicoStubEnd - (uint32_t)PicoStubStart, i;
	uint32_t			unpacked = PICO_STUB_ADRS + (uint32_t)PicoStubUnpacked - (uint32_t)PicoStubStart;
	uint32_t			pad = PICO_STUB_ADRS + (uint32_t)PicoStubPad - (uint32_t)PicoStubStart;

	if ((exe->pc0 != PICO_EXE_PC0) || (code[0] != (0x3C080000 | (PICO_MENU_ENTRY >> 16))) ||
		(code[1] != (0x25080000 | (PICO_MENU_ENTRY & 0xFFFF))))
		return;	// (another PicoStation menu: nothing)
	((volatile CheatTable *)CHEAT_TABLE_ADRS)->magic = 0;
	for (i = 0; i < size / 4; i++)
		((volatile uint32_t *)PICO_STUB_ADRS)[i] = PicoStubStart[i];
	for (i = 0; i < 16; i++)
		data[i] = 0;
	data[4] = 0x0C000000 | ((pad >> 2) & 0x03FFFFFF);
	data[5] = 1;	// (trace: set up; 2: the unpacked menu patched. 6-8: the words checked, 9: buttons, 10: reads)
	code[0] = 0x3C080000 | ((unpacked + 0x8000) >> 16);	// lui   t0, hi
	code[1] = 0x25080000 | (unpacked & 0xFFFF);			// addiu t0, t0, lo
}

#define	BOOT_CHEATS		1	// BootDisc: the cheat screen first (Select + Triangle)
#define	BOOT_QUIET		2	// BootDisc: no "Booting the disc..." (L2 + R2: the PicoStation's own menu)

void	BootDisc(int flags)
{
	struct EXEC			exe;
	char				boot[64], exe_name[sizeof(((DiscId *)0)->exe)], *p;
	uint32_t			tcb, event, stack, size, file_size;
	int					lba, n, i;
	int					with_cheats = flags & BOOT_CHEATS;

	if (!(flags & BOOT_QUIET))
		PrintConsole("Booting the disc...");
	CD_Unlock();

	// SYSTEM.CNF, or PSX.EXE without it (BIOS defaults). The disc is read while the menu goes on (each read polled with
	// interrupts disabled): a failure only shows a message.
	strcpy(boot, "cdrom:PSX.EXE;1");
	tcb = 4;
	event = 16;
	stack = 0x801FFF00;
	memset(disc_cnf, 0, DISC_CNF_SIZE);
	if ((lba = CD_FindFile("SYSTEM.CNF;1", &file_size, disc_exe)) < 0)
	{
		PrintConsole("No disc, or it can't be read");
		return;
	}
	if (lba)
	{
		if (!CD_ReadSectors(lba, (uint8_t *)disc_cnf, (file_size < 2048) ? file_size : 2048))
		{
			PrintConsole("Can't read SYSTEM.CNF");
			return;
		}
		if ((p = CnfValue(disc_cnf, "BOOT")))
		{
			for (i = 0; (i < sizeof(boot) - 1) && (p[i] > ' '); i++)
				boot[i] = p[i];
			boot[i] = 0;
		}
		tcb = CnfHex(disc_cnf, "TCB", tcb);
		event = CnfHex(disc_cnf, "EVENT", event);
		stack = CnfHex(disc_cnf, "STACK", stack);
	}

	// The executable's name without "cdrom:\" and ";1": the disc's serial number (cheats, ps1transfer -info "game=")
	{
		const char	*name = boot;

		for (p = boot; *p; p++)
			if ((*p == ':') || (*p == '\\'))
				name = p + 1;
		for (i = 0; (i < sizeof(exe_name) - 1) && name[i] && (name[i] != ';'); i++)
			exe_name[i] = name[i];
		exe_name[i] = 0;
	}
	// Select + Triangle: the game's cheats in the cartridge (the cheat screen), set in the code engine's table
	if (with_cheats && !CheatMenu(exe_name))
		return;

	// The PS-X EXE: header (load address and size), then the program under the firmware, free RAM cleared
	if ((lba = CD_FindFile(boot, &file_size, disc_exe)) <= 0)
	{
		PrintConsole(lba ? "Executable not on the disc" : "No disc, or it can't be read");
		return;
	}
	n = (file_size > 2048) && CD_ReadSectors(lba, disc_exe, 2048);
	memcpy(&exe, disc_exe + 0x10, sizeof(exe));
	size = exe.t_size;
	if (n && (size > file_size - 2048))	// (header bigger than the file)
		size = file_size - 2048;
	if (!n || memcmp(disc_exe, "PS-X EXE", 8) || (exe.t_addr < 0x80010000) || (exe.t_addr + size > DISC_EXE_TOP))
	{
		PrintConsole(!n ? "Can't read the executable" : memcmp(disc_exe, "PS-X EXE", 8) ? "Not a PS-X EXE" :
			"Executable over the firmware");
		return;
	}
	memset((void *)0x80010000, 0, DISC_EXE_TOP - 0x80010000);
	if (!CD_ReadSectors(lba + 1, (uint8_t *)exe.t_addr, size))
	{
		PrintConsole("Can't read the executable");
		return;
	}

	// PicoStation menu: the game it mounts is booted by this menu after the BIOS restart (see CHAIN_BOOT_ADRS). Only
	// with its Regular Boot: its Fast Boot skips the cartridge's pre-boot vector (it restarts the BIOS past its start,
	// 0xBFC00390), and its executable is packed (the jump can't be patched before it runs). X on this menu boots the
	// SD card's games directly (sdbrowser.h).
	if (!strcmp(exe_name, PICOSTATION_MENU_EXE))
	{
		chain_boot[0] = CHAIN_BOOT_MAGIC;
		chain_boot[1] = ~CHAIN_BOOT_MAGIC;
		PicoReturnSetup(&exe);	// (L2 + R2 there: back to this menu)
	}

	// PSn00bSDK's interrupt handlers removed (as for a program executed), kernel set up as SYSTEM.CNF asks (as the
	// BIOS does: it re-enables interrupts)
	DrawSync(0);
	PadStop();
	StopCallback();
	EnterCriticalSection();
	IRQ_STAT = 0;
	SetConf(event, tcb, stack);

	// The disc's executable for the computer (ps1transfer -info "game=")
	{
		volatile DiscId	*id = (volatile DiscId *)DISC_ID_ADRS;

		for (i = 0; i < sizeof(exe_name); i++)
			id->exe[i] = exe_name[i];
		id->magic = DISC_ID_MAGIC;
	}

	// The stack of SYSTEM.CNF (as the BIOS does), the kernel hook enabled with the BIOS TTY device kept
	exe.s_addr = stack;
	exe.s_size = 0;
	ProfFillRam(false);
	kernel_hook_params->tty_mode = EXEC_TTY_MODE_BIOS;
	kernel_hook_params->is_enabled = true;

	// The BIOS CD driver isn't there (PSn00bSDK removed it, and the disc was read without it): a game loading another
	// executable from the disc (Load or LoadExec, "cdrom:...", e.g. Fighting Force after its loading screen) has the
	// debug monitor read it, as above (monitor version 13, see monitor/monitor.h: MON_REASON_CD_SETUP)
	if (mon_state->entry && (mon_state->version >= 13))
		((void (*)(int, uint32_t *))mon_state->entry)(MON_REASON_CD_SETUP, NULL);

	// The Manager's Remote Controller for the game (ps1transfer -pad), as for a program executed with it: the debug
	// monitor hooks the BIOS InitPAD (a game reading its pads through the BIOS: PsyQ's PadInit...; one using the pad
	// port itself, e.g. libpad, isn't driven). The monitor's work at each exception only once the computer presses
	// buttons (ps1transfer sets prof_on, see ps1transfer/memlive.c RemotePad): no cost for a game nobody drives.
	// Monitor version 14 (before, its pad_orig kept the disc's BIOS Load: MON_DISC_LOAD_ADRS now)
	if (mon_state->entry && (mon_state->version >= 14))
	{
		PadRemoteSetup();
		mon_state->prof_on = 0;
	}

	// The drive's interrupts on, as the BIOS leaves them for a program: the menu turned them off (see main), and a
	// game's CD library sending its first command then waited for its timeout (about 15 seconds)
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;	// (anything pending acknowledged)
	CD_REG(2) = 0x1F;
	IRQ_STAT = ~(1 << 2);

	EnterCriticalSection();
	FlushCache();
	Exec(&exe, 1, NULL);
}

/****************************************/
// The menu screen: background and panels, then the logo is loaded on top of it (with the font), then the text

static void	MenuDraw(void)
{
	int	size, i;

	DrawMenuFrame();
	// Copy image data from ROM to RAM, because LoadImage doesn't work when loading directly from ROM ?!
	// Images are placed in VRAM at the position stored in each TIM (see cartrom/tfs_tool.py)
	for (i = 0; i < sizeof(tim_files) / sizeof(tim_files[0]); i++)
	{
		volatile uint8_t	*tim = ROM_GetFile(tim_files[i], &size);

		if (tim)
		{
			memcpy((volatile uint8_t *)0x80010000, tim, size);
			LoadTexture((uint32_t *)0x80010000);
		}
	}
	// Menu header: logo (loaded above with the TIMs), firmware version on the left and device ID on the right
	DrawMenuLabels();
	DrawInfoLine();
	InitConsole();
}

// What START + SELECT does: the PicoStation's games when its menu disc is in the drive (sdbrowser.h), the game it
// has mounted, or the disc
static void	MenuHint(bool pico)
{
	char	text[24 + PICO_GAME_NAME], *p, *end;
	int		i;

	if (pico)
		pico_game->magic = 0;	// (its menu disc is back: no game mounted)
	if (pico || (pico_game->magic != PICO_GAME_MAGIC) || pico_game->name[PICO_GAME_NAME - 1])
	{
		PrintConsoleColor(pico ? "START + SELECT: PicoStation games" : "START + SELECT: boot the disc", UI_YELLOW);
		return;
	}
	strcpy(text, "START + SELECT: boot ");
	for (p = text + strlen(text), i = 0; pico_game->name[i]; i++)
		*p++ = pico_game->name[i];
	strcpy(p, " again");
	// Lines of the log, cut after a space when there is one
	for (p = text; *p; p = end)
	{
		end = p + strlen(p);
		if (end - p > CONSOLE_N_CHAR)
		{
			for (end = p + CONSOLE_N_CHAR; (end > p) && (end[-1] != ' '); end--)
				;
			if (end == p)
				end = p + CONSOLE_N_CHAR;
		}
		i = *end;
		*end = 0;
		PrintConsoleColor(p, UI_YELLOW);
		*end = i;
	}
}

// The menu drawn again (after the PicoStation screen)
static void	MenuScreen(bool pico)
{
	MenuDraw();
	MenuHint(pico);
	PrintUSBStatus();
}

/****************************************/
// START + SELECT with a PicoStation: the games of its SD card, booted with the kernel hook

#include "sdbrowser.h"

// The START + SELECT hint after a reset without holding the menu up: the drive spins the disc up and identifies it (a
// few seconds, CD_WaitReady) before its volume name can be read (PicoDetect). Meanwhile the menu works; the drive is
// asked every quarter second (GetID: an error until the disc is identified), then the hint printed. Given up after
// 20 seconds (no disc, lid open): the disc's hint. hint_next: VSync count of the next try, 0 once printed.
static int	hint_next, hint_end;

static void	HintPoll(void)
{
	uint8_t	resp[8];
	int		now = VSync(-1);

	if (!hint_next || (now - hint_next < 0))
		return;
	hint_next = now + 15;
	if ((now - hint_end < 0) &&
		!((CD_RawCommand(0x1A, NULL, 0, resp, sizeof(resp)) == 3) && CD_RawResponse(resp, sizeof(resp), 50000)))
		return;
	hint_next = 0;
	MenuHint((now - hint_end < 0) && PicoDetect());
}

/****************************************/

void	KernelHooksUninstall(void)
{
	void	**a0table = (void **)0x200;
	void	**b0table = (void **)GetB0Table();	// PSn00bSDK (BIOS B(57h))
	volatile uint32_t	*v80 = (uint32_t *)0x80000080;
	int		i;

	// Exception vector patched for a debug session (jump into the kernel hook code, which is replaced)
	if ((mon_state->magic == MONITOR_MAGIC) && ((v80[0] & 0xFFFF0000) == 0x3C1A0000) &&
		((((v80[0] & 0xFFFF) << 16) + (int16_t)v80[1]) - KERNEL_UNUSED_START < KERNEL_UNUSED_SIZE))
	{
		for (i = 0; i < 4; i++)
			v80[i] = mon_state->chain80[i];
		FlushCache();
		printf("Kernel hooks: Debug exception vector uninstalled\n");
	}

	// Addresses of original functions are less than 0x1000
	// Addresses of hooked functions are in range KERNEL_UNUSED_START...KERNEL_UNUSED_START+KERNEL_UNUSED_SIZE
	if ((uint32_t) a0table[0x40] >= KERNEL_UNUSED_START && (uint32_t) a0table[0x40] < KERNEL_UNUSED_START + KERNEL_UNUSED_SIZE)
	{
		a0table[0x40] = (void *) *p_UnresolvedExceptionOld;
		printf("Kernel hooks: Old UnresolvedException uninstalled\n");
	}

	if ((uint32_t) b0table[0x17] >= KERNEL_UNUSED_START && (uint32_t) b0table[0x17] < KERNEL_UNUSED_START + KERNEL_UNUSED_SIZE)
	{
		b0table[0x17] = (void *) *p_ReturnFromExceptionOld;
		printf("Kernel hooks: Old ReturnFromException uninstalled\n");
	}
}

void	KernelHooksInstall(void)
{
	// Copy kernel hook installer to unused/reserved kernel space and launch it
	memcpy((volatile uint8_t *)KERNEL_HOOK_START, _binary_kernel_hook_bin_start, &_binary_kernel_hook_bin_size);
	// The instruction cache may still hold the code of a previous kernel hook at the same addresses
	// (firmware reloaded from RAM): mixing old cached and new code crashed in the hook
	FlushCache();
	((void (*)())KERNEL_HOOK_START)();
	printf("Kernel hooks: Installed\n");

	// Debug monitor (crash screen, debugger): in the cartridge flash after the menu images, or copied to RAM
	// by a test build. Its header holds a magic number and the entry address.
#ifdef MONITOR_IN_RAM
	memcpy((void *)MONITOR_RAM_ADRS, _binary_monitor_ram_bin_start, (uint32_t)&_binary_monitor_ram_bin_size);
	FlushCache();
	if (((uint32_t *)MONITOR_RAM_ADRS)[0] == MONITOR_MAGIC)
	{
		mon_state->entry = ((uint32_t *)MONITOR_RAM_ADRS)[1];
		mon_state->version = ((uint32_t *)MONITOR_RAM_ADRS)[2];
	}
#else
	if (((volatile uint32_t *)MONITOR_ROM_ADRS)[0] == MONITOR_MAGIC)
	{
		mon_state->entry = ((volatile uint32_t *)MONITOR_ROM_ADRS)[1];
		mon_state->version = ((volatile uint32_t *)MONITOR_ROM_ADRS)[2];
	}
#endif
	printf("Debug monitor: %s\n", mon_state->entry ? "OK" : "not found");
	mon_state->fw_version = FIRMWARE_VERSION_BCD;
}

/****************************************/
// Screensaver: the menu screen is a static image, left on for a long time it can burn into a CRT. With no pad or USB
// activity for a while (saver.h: 3, 5 or 10 minutes, or never, as set from the Manager), the display is turned off
// or the logo bounces on a black screen; any activity puts the screen back.

int main(void)
{
	bool	usb_ok;
	RECT	vram = {0, 0, 1024, 512};
	uint32_t	pad;
	int		boot_hold = 0;	// Start + Select held since this VSync count + 1, -1 until released
	int		pico_hold = 0;	// L2 + R2 held (the PicoStation's own menu), the same
	int		last_activity;
	// Started again by the PicoStation menu (the game mounted): read and cleared first, a failed boot isn't retried
	bool	chain = (chain_boot[0] == CHAIN_BOOT_MAGIC) && (chain_boot[1] == ~CHAIN_BOOT_MAGIC);

	chain_boot[0] = 0;
	chain_boot[1] = 0;
	MenuPadClear();		// (no buttons from the computer before the menu)

	// PSn00bSDK: ResetGraph() (in InitDisplay) calls ResetCallback() and installs its VBlank/DMA handlers only if the
	// callbacks were not set up yet: with ResetCallback() called first (PsyQ order), VSync() would never return
	InitDisplay();
	PadInit(0);
	SpuInit();	// Was SsInit() (libsnd sequencer, several KB of buffers the firmware has no room for)

	// Uninstall kernel hooks to prevent crashes due to overwriting if
	// PS1 DEV CART firmware is ran from RAM after being booted from ROM
	// (normally useful for cartridge development only)
	// No interrupt while the kernel hook code is replaced (an interrupt could run half-copied code)
	EnterCriticalSection();
	KernelHooksUninstall();

	KernelHooksInstall();
	// The menu answers USB itself. The hook params survive a firmware run from RAM (is_enabled was set by
	// the Execute command of the previous firmware): the hook would read the same commands on every interrupt
	kernel_hook_params->is_enabled = false;
	ExitCriticalSection();
#if USB_SEND_TIMING
	// Bus timing of the writes to the USB chip on BIOS 4.x consoles (SCPH-7002/7502, see USB_SEND_TIMING in usb.h),
	// for the kernel hook too: programs and discs started from the menu keep it.
	// After the hook is installed: its startup clears its space, the word for the hook included
	USB_SendTimingSetup();
#endif
	kernel_hook_params->usb_packet_log2 = USB_BIG_PACKET_LOG2;	// Tells ps1transfer that big packets can be used
	// Memory/VRAM access while a program runs, device ID, big packets also while a program runs
	kernel_hook_params->features = FEATURE_LIVE_ACCESS | FEATURE_DEVICE_ID | FEATURE_HOOK_BIG_PACKETS | FEATURE_BOOT_DISC |
		FEATURE_DISC_ID | FEATURE_CHEATS | FEATURE_SCREENSAVER;
	((volatile DiscId *)DISC_ID_ADRS)->magic = 0;		// (no disc booted)
	((volatile CheatTable *)CHEAT_TABLE_ADRS)->magic = 0;	// (no cheats: the computer or the menu sets them)

	// Save entire VRAM in RAM at boot (so the computer can download VRAM data)
	StoreImage(&vram, (uint32_t *)0x80020000);
	DrawSync(0);
	VSync(0);

	LoadDeviceId();
	MenuDraw();

	// Enable CD IRQs (with X held, for CdInit() below)
	// Otherwise they are not enabled after CdInit() which will hang
	// and timeout the first CD command but they will be enabled afterwards
	// Intended behaviour? Or bug?
	//
	// For registers details and alternative implementation, see:
	// https://problemkaputt.de/psx-spx.htm#cdromcontrollerioports
	// https://github.com/Lameguy64/PSn00bSDK/blob/702bb601fb5712e2ae962a34b89204c646fe98f5/libpsn00b/psxcd/common.c#L217
	// Only then: no handler acknowledges the drive otherwise, an answer it gives after the boot (e.g. a disc it can't
	// read on a locked drive, after a power on) could interrupt the menu forever
	*((volatile uint8_t *)0x1F801800) = 1;	// Index Register
	*((volatile uint8_t *)0x1F801803) = 0x1F;	// Interrupt Flag Register
	*((volatile uint8_t *)0x1F801802) = 0;	// Interrupt Enable Register
	IRQ_STAT = ~(1 << 2);

	pad = PadRead(1);
	if (pad & PADRdown)	// X = Init CD
	{
		*((volatile uint8_t *)0x1F801802) = 7;	// Interrupt Enable Register
		PrintConsole("Initializing CD...");
		CdInit();
		PrintConsole("CD ready");
	}

	if (chain)
		MenuHint(false);	// (chain boot: a game is mounted)
	else
	{
		hint_next = VSync(-1) | 1;	// (HintPoll in the menu loop: not 0)
		hint_end = hint_next + 20 * 60;
	}
	usb_ok = PrintUSBStatus();

	// PicoStation chain boot: the game chosen in its menu, booted with the kernel hook (Select held: the menu stays).
	// The BIOS reads the pads at VBlank: a few frames first.
	if (chain)
	{
		int		i;

		for (i = 0; i < 10; i++)
			VSync(0);
		pad = PadRead(1) & 0xFFFF;
		if ((pad != 0xFFFF) && (pad & PADselect))
			PrintConsole("Select held: PicoStation game not booted");
		else
			BootDisc(0);
	}

	Init_CRC8();	// Copy CRC8 lookup table in Data Cache
	last_activity = VSync(-1);
	while (1)
	{
		int		ret = USB_STATE_NONE;

		// Start + Select held half a second: the disc is booted (again after a failure only once released). All the
		// buttons "pressed" is a bad read: the boot started by itself with a single read.
		pad = MenuPad();
		HintPoll();
		// Select + Triangle: the same, the game's cheats in the cartridge chosen first. With the PicoStation's menu disc
		// in the drive: its games (the cheats chosen there, Triangle)
		if ((((pad & (PADstart | PADselect)) == (PADstart | PADselect)) ||
			 ((pad & (PADselect | PADRup)) == (PADselect | PADRup))) && (pad != 0xFFFF))
		{
			if (!boot_hold)
				boot_hold = VSync(-1) + 1;
			else if ((boot_hold > 0) && (VSync(-1) + 1 - boot_hold >= 30))
			{
				if (PicoDetect())
				{
					hint_next = 0;	// (its screen prints the hint when left)
					SdBrowser();
					usb_ok = !(USB_STATUS & USB_STATUS_MASK_USB_READY);
				}
				else
					BootDisc(!(pad & PADstart));
				boot_hold = -1;
			}
		}
		else
			boot_hold = 0;

		// L2 + R2 held half a second: the PicoStation's own menu (its menu disc booted as it is, for those who prefer
		// it to the PicoStation screen). Not in the menu's hints. Booted like any disc: its Regular Boot then has the
		// game it mounts booted by this menu (chain boot, the kernel hook kept). Nothing without its menu disc.
		if (((pad & (PADL2 | PADR2)) == (PADL2 | PADR2)) && (pad != 0xFFFF) && !(pad & (PADstart | PADselect)))
		{
			if (!pico_hold)
				pico_hold = VSync(-1) + 1;
			else if ((pico_hold > 0) && (VSync(-1) + 1 - pico_hold >= 30))
			{
				if (PicoDetect())
					BootDisc(BOOT_QUIET);
				pico_hold = -1;
			}
		}
		else
			pico_hold = 0;


		// Handle USB cable plug/unplug status
		if (!usb_ok)
		{
			if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
				usb_ok = PrintUSBStatus();
		}
		else
		{
			if (USB_STATUS & USB_STATUS_MASK_USB_READY)
				usb_ok = PrintUSBStatus();
		}

		if (usb_ok)
		{
			ret = USB_Process(0, 0, NULL, 0, NULL);
			if (ret != USB_STATE_NONE)	// (the screen put back before the log is written)
				SaverStop();

			switch (ret)
			{
				case USB_STATE_BAD_CRC:
					PrintConsole("BAD Command CRC");
				break;

				case USB_STATE_TIMEOUT:
					PrintConsole("Timeout");
				break;

				case USB_STATE_OK:
					PrintConsole("Transfer Successful");
				break;

				case USB_STATE_BOOT_DISC:	// ps1transfer -bootcd
					BootDisc(0);
				break;
			}
		}

		// Screensaver (saver.h, as the Manager set it): any pad or USB activity (not just idle polling,
		// USB_STATE_NONE) ends it and resets the idle timer; idle long enough, it starts
		if (((pad != 0) && (pad != 0xFFFF)) || (ret != USB_STATE_NONE))
		{
			last_activity = VSync(-1);
			SaverStop();
		}
		else if (saver_on)
			SaverFrame();
		else if (SaverFrames(SAVER_MENU) && (VSync(-1) - last_activity >= SaverFrames(SAVER_MENU)))
			SaverStart(SAVER_MENU);
    }

    return (0);
}
