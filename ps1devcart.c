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
#include "display.h"
#include "usb.h"
#include "tfs.h"

#define KERNEL_UNUSED_START	0x8000C000
#define KERNEL_UNUSED_SIZE	0x2000
#define KERNEL_HOOK_START	(KERNEL_UNUSED_START + 0x160)

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

#define	FIRMWARE_VERSION	"1.1"	// Firmware version, shown on the menu (the tools': DEVCART_VERSION, devcart_ui.h)
#define	FIRMWARE_VERSION_BCD	0x11	// Same, major << 4 | minor, read by "ps1transfer -info" (MonitorState.fw_version)
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

// Erases the last flash sector and writes "DVID" + the device ID (runs from RAM, with interrupts disabled).
// Returns 0 when done, or the step that failed: 1 unknown chip (*chip), 2 erase, 3 program.
static int	SaveDeviceId(uint16_t *chip)
{
	volatile uint8_t	*sector = (volatile uint8_t *)DEVICE_ID_FLASH;
	uint8_t				data[4 + DEVICE_ID_LEN];
	bool				ok = true;
	int					i;

	memcpy(data, "DVID", 4);
	memcpy(data + 4, device_id, DEVICE_ID_LEN);
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
#define	PROF_FILL_TOP	FW_LOAD_ADDR
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
		PrintConsole("USB socket unplugged !");
		return (false);
	}
	DrawStatus(1, "READY: WAITING FOR THE COMPUTER");
	PrintConsole("USB connected !");
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

// The polled drive commands and the disc's files: cdread.h (shared with the debug monitor)
#define	CD_DELAY_US(_us_)		DelayUs(_us_)
#define	CD_CRITICAL_ENTER()		EnterCriticalSection()
#define	CD_CRITICAL_EXIT()		ExitCriticalSection()
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

static char		disc_cnf[2048 + 1];
static uint8_t	disc_exe[2048];

#include "cheatmenu.h"

void	BootDisc(int with_cheats)
{
	struct EXEC			exe;
	char				boot[64], exe_name[sizeof(((DiscId *)0)->exe)], *p;
	uint32_t			tcb, event, stack, size, file_size;
	int					lba, n, i;

	PrintConsole("Booting the disc...");
	CD_Unlock();

	// SYSTEM.CNF, or PSX.EXE without it (BIOS defaults). The disc is read while the menu goes on (each read polled with
	// interrupts disabled): a failure only shows a message.
	strcpy(boot, "cdrom:PSX.EXE;1");
	tcb = 4;
	event = 16;
	stack = 0x801FFF00;
	memset(disc_cnf, 0, sizeof(disc_cnf));
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

int main(void)
{
	int		size, i;
	bool	usb_ok;
	RECT	vram = {0, 0, 1024, 512};
	uint32_t	pad;
	int		boot_hold = 0;	// Start + Select held since this VSync count + 1, -1 until released

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
	kernel_hook_params->usb_packet_log2 = USB_BIG_PACKET_LOG2;	// Tells ps1transfer that big packets can be used
	// Memory/VRAM access while a program runs, device ID, big packets also while a program runs
	kernel_hook_params->features = FEATURE_LIVE_ACCESS | FEATURE_DEVICE_ID | FEATURE_HOOK_BIG_PACKETS | FEATURE_BOOT_DISC |
		FEATURE_DISC_ID | FEATURE_CHEATS;
	((volatile DiscId *)DISC_ID_ADRS)->magic = 0;		// (no disc booted)
	((volatile CheatTable *)CHEAT_TABLE_ADRS)->magic = 0;	// (no cheats: the computer or the menu sets them)

	// Save entire VRAM in RAM at boot (so the computer can download VRAM data)
	StoreImage(&vram, (uint32_t *)0x80020000);
	DrawSync(0);
	VSync(0);

	// Menu: background and panels, then the logo is loaded on top of it (with the font), then the text
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
	LoadDeviceId();
	DrawInfoLine();

	InitConsole();

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
		PrintConsole("CD ready!");
	}

	PrintConsole("START + SELECT: boot the disc");
	usb_ok = PrintUSBStatus();

	Init_CRC8();	// Copy CRC8 lookup table in Data Cache
	while (1)
	{
		int		ret;

		// Start + Select held half a second: the disc is booted (again after a failure only once released). All the
		// buttons "pressed" is a bad read: the boot started by itself with a single read.
		pad = PadRead(1) & 0xFFFF;
		// Select + Triangle: the same, the game's cheats in the cartridge chosen first
		if ((((pad & (PADstart | PADselect)) == (PADstart | PADselect)) ||
			 ((pad & (PADselect | PADRup)) == (PADselect | PADRup))) && (pad != 0xFFFF))
		{
			if (!boot_hold)
				boot_hold = VSync(-1) + 1;
			else if ((boot_hold > 0) && (VSync(-1) + 1 - boot_hold >= 30))
			{
				BootDisc(!(pad & PADstart));
				boot_hold = -1;
			}
		}
		else
			boot_hold = 0;

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

			switch (ret)
			{
				case USB_STATE_BAD_CRC:
					PrintConsole("BAD Command CRC !");
				break;

				case USB_STATE_TIMEOUT:
					PrintConsole("Timeout !");
				break;

				case USB_STATE_OK:
					PrintConsole("Transfer Successful !");
				break;

				case USB_STATE_BOOT_DISC:	// ps1transfer -bootcd
					BootDisc(0);
				break;
			}
		}
    }

    return (0);
}
