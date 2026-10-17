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

#define	FIRMWARE_VERSION	DEVCART_VERSION	// Firmware version, shown on the menu (devcart_ui.h: the same for the tools)
#define	FIRMWARE_VERSION_BCD	0x10	// Same, major << 4 | minor, read by "ps1transfer -info" (MonitorState.fw_version)
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
// I couldn't make this work

char	*cd_region_check[] = {"for Eur", "for U/C", "for NET"};
char	*cd_region_unlock[] = {"(Europe)", "of America", "World wide"};

bool	CD_Unlock(void)
{
	uint8_t	getid_cmd[] = {0x22,0,0,0,0};
	uint8_t	result[16];
	int		i;

	// (raw commands: CdCommand, CdControl only knows the documented ones)
	CdCommand(0x19, getid_cmd, sizeof(getid_cmd), result);
	CdSync(0, NULL);

	for (i = 0; i < 3; i++)
		if (!strncmp((char *)result, cd_region_check[i], 7))
			break;
	if (i == 3)
	{
		PrintConsole("Error: CD Drive not compatible...");
		return (false);
	}

	CdCommand(0x50, NULL, 0, NULL);
	CdSync(0, NULL);
	CdCommand(0x51, "Licensed by", 11, NULL);
	CdSync(0, NULL);
	CdCommand(0x52, "Sony", 4, NULL);
	CdSync(0, NULL);
	CdCommand(0x53, "Computer", 8, NULL);
	CdSync(0, NULL);
	CdCommand(0x54, "Entertainment", 13, NULL);
	CdSync(0, NULL);
	CdCommand(0x55, cd_region_unlock[i], strlen(cd_region_unlock[i]), NULL);
	CdSync(0, NULL);
	CdCommand(0x56, NULL, 0, NULL);
	CdSync(0, NULL);

	return (true);
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
	kernel_hook_params->features = FEATURE_LIVE_ACCESS | FEATURE_DEVICE_ID | FEATURE_HOOK_BIG_PACKETS;

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

	// Enable CD IRQs
	// Otherwise they are not enabled after CdInit() which will hang
	// and timeout the first CD command but they will be enabled afterwards
	// Intended behaviour? Or bug?
	//
	// For registers details and alternative implementation, see:
	// https://problemkaputt.de/psx-spx.htm#cdromcontrollerioports
	// https://github.com/Lameguy64/PSn00bSDK/blob/702bb601fb5712e2ae962a34b89204c646fe98f5/libpsn00b/psxcd/common.c#L217
	*((volatile uint8_t *)0x1F801800) = 1;	// Index Register
	*((volatile uint8_t *)0x1F801803) = 7;	// Interrupt Flag Register
	*((volatile uint8_t *)0x1F801802) = 7;	// Interrupt Enable Register

	pad = PadRead(1);
	if (pad & (PADRdown | PADRright))	// X = Init CD
	{
/*
		// I couldn't make this work
		if (pad & PADRright)			// O = Unlock CD
		{
			PrintConsole("Unlocking CD drive...");
			CdInit();
			CD_Unlock();
		}
*/
		PrintConsole("Initializing CD...");
		CdInit();
		PrintConsole("CD ready!");
	}

	usb_ok = PrintUSBStatus();

	Init_CRC8();	// Copy CRC8 lookup table in Data Cache
	while (1)
	{
		int		ret;

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

/*		// This is only to test the ExceptionHandler crash screen
		if (pad & PADRdown)
		{
			asm("lui $v0,0x8001");
			asm("addiu $v0,0x0001");
			asm("lw $v0,0($v0)");
		}
*/

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
			}
		}
    }

    return (0);
}
