/*
* PS1 DEV CART Transfer tool: -flash: cartridge ROM flashing (backup, checks, keeping the cheats and settings) (from main.c)
*
* PS1USB Transfer tool v1.0
* by OrionSoft [2024]
* https://orionsoft.games/
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*
* Linux version require libftdi1-dev
*/

#include "ps1transfer.h"

/****************************************/
// "-flash rom.rom" flashes a cartridge ROM. The ROM is uploaded to RAM with a header (magic, size, sum),
// the flasher (ar_flash.ps-exe, next to ps1transfer) flashes it and sends its status lines, then the PS1 is reset and
// the flash is read back and compared (except the last sector: the device ID, written again by the new firmware).
// The cartridge's cheats and device ID/settings sector are read first and kept when the ROM's areas are empty
// (-flashcheats: the ROM's cheats area written as it is, -flashall: nothing kept, any cartridge ROM). The whole flash
// is saved as a backup first, and the image and flasher are compared in the console's RAM before the flasher starts.

int		flash_keep = FLASH_KEEP_CHEATS | FLASH_KEEP_SETTINGS;

void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

// The ROM file, checked before anything is done with the PS1 (NULL if it isn't a cartridge ROM)
uint8_t	*LoadRom(char *rom_file)
{
	uint8_t	*rom;
	int		size;

	if (!(rom = SystemGetFile(rom_file, &size)))
		return (NULL);
	if ((size != FLASH_ROM_SIZE) || memcmp(rom + 0x84, "Licensed by Sony Computer Entertainment Inc.", 44))
	{
		printf("'%s' is not a cartridge ROM (128KB with the cartridge boot header), nothing flashed.\n",
			rom_file);
		free(rom);
		return (NULL);
	}
	return (rom);
}

static bool	FlashAreaEmpty(const uint8_t *p, int size)
{
	int	i;

	for (i = 1; (i < size) && (p[i] == p[0]); i++)
		;
	return ((i == size) && ((p[0] == 0x00) || (p[0] == 0xFF)));
}

// "Firmware: x.y" of a ROM image into text (16 bytes), "" if it has none (original or older firmware)
static void	FlashRomVersion(const uint8_t *rom, int size, char *text)
{
	int	i, n;

	text[0] = 0;
	for (i = 0; i + 10 < size; i++)
		if (!memcmp(rom + i, "Firmware: ", 10))
		{
			for (n = 0; (n < 15) && (i + 10 + n < size) && (rom[i + 10 + n] >= ' ') && (rom[i + 10 + n] < 127); n++)
				text[n] = rom[i + 10 + n];
			text[n] = 0;
			return;
		}
}

// The folder of the automatic flash backups: -flashbackup DIR, else the Manager's settings folder (APPDATA or
// ~/.config: ps1devcart-gui/flash_backups), created if needed. NULL: -flashbackup none.
char	*flash_backup_dir;

static bool	FlashBackupPath(char *path, int size)
{
	char		base[512], serial[64], *s;
	time_t		now = time(NULL);
	struct tm	*t = localtime(&now);
	int			i;

	if (flash_backup_dir)
		snprintf(base, sizeof(base), "%s", flash_backup_dir);
	else
	{
#ifdef _WIN32
		if (!(s = getenv("APPDATA")) && !(s = getenv("USERPROFILE")))
			return (false);
		snprintf(base, sizeof(base), "%s\\ps1devcart-gui", s);
		MAKE_DIR(base);
		strncat(base, "\\flash_backups", sizeof(base) - strlen(base) - 1);
#else
		char	cfg[400];

		if ((s = getenv("XDG_CONFIG_HOME")) && s[0])
			snprintf(cfg, sizeof(cfg), "%s", s);
		else if ((s = getenv("HOME")))
			snprintf(cfg, sizeof(cfg), "%s/.config", s);
		else
			return (false);
		MAKE_DIR(cfg);
		snprintf(base, sizeof(base), "%s/ps1devcart-gui", cfg);
		MAKE_DIR(base);
		strncat(base, "/flash_backups", sizeof(base) - strlen(base) - 1);
#endif
	}
	MAKE_DIR(base);
	if (!USB_GetSerial(serial))
		strcpy(serial, "cartridge");
	for (i = 0; serial[i]; i++)		// (a name for a file)
		if (!isalnum((unsigned char)serial[i]) && (serial[i] != '-') && (serial[i] != '_'))
			serial[i] = '_';
	snprintf(path, size, "%s%c%s_%04d%02d%02d-%02d%02d%02d.rom", base,
#ifdef _WIN32
		'\\',
#else
		'/',
#endif
		serial, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
	return (true);
}

// Before anything is erased: the ROM checked (a PS1 DEV CART ROM unless -flashall), the versions shown, the whole
// flash read and saved as a backup (nothing flashed if it can't be saved, unless -flashbackup none), then the
// cartridge's cheats and device ID/settings sector put in the ROM image where the ROM has none (an older ROM may have
// code there: then it is written as it is). False: nothing flashed.
static bool	FlashPrepare(uint8_t *rom)
{
	uint8_t	*cart, state[16];
	char	rom_version[16], path[700];
	FILE	*f;
	bool	ok = false;

	FlashRomVersion(rom, FLASH_ROM_SIZE, rom_version);
	if (memcmp(rom + 4, "PS1 DEV CART", 12) && flash_keep)
	{
		printf("This ROM isn't a PS1 DEV CART ROM: nothing flashed (-flashall writes any cartridge ROM as it is).\n");
		return (false);
	}
	if (USB_Process('D', state, MONITOR_STATE_ADRS, sizeof(state)) && (*(uint32_t *)state == MONITOR_MAGIC) &&
		state[15])
		printf("Cartridge firmware %d.%d, ROM firmware %s.\n", state[15] >> 4, state[15] & 15,
			rom_version[0] ? rom_version : "without version");
	else
		printf("Cartridge firmware unknown (original or older), ROM firmware %s.\n",
			rom_version[0] ? rom_version : "without version");

	if (!(cart = malloc(FLASH_ROM_SIZE)))
		return (false);
	printf("Reading the cartridge's flash (backup)...\n");
	if (!USB_Process('D', cart, 0x1F000000, FLASH_ROM_SIZE))
	{
		printf("\nUSB error: nothing flashed.\n");
		goto out;
	}
	if (!flash_backup_dir || strcmp(flash_backup_dir, "none"))
	{
		if (!FlashBackupPath(path, sizeof(path)) || !(f = fopen(path, "wb")))
		{
			printf("The backup of the flash can't be saved (%s): nothing flashed (-flashbackup DIR, or none).\n",
				flash_backup_dir ? flash_backup_dir : "settings folder");
			goto out;
		}
		if ((fwrite(cart, 1, FLASH_ROM_SIZE, f) != FLASH_ROM_SIZE) | fclose(f))
		{
			printf("The backup of the flash can't be written to %s: nothing flashed.\n", path);
			goto out;
		}
		printf("Backup of the flash: %s\n", path);
	}

	if ((flash_keep & FLASH_KEEP_CHEATS) && FlashAreaEmpty(rom + FLASH_CHEATS, FLASH_CHEATS_SIZE) &&
		!memcmp(cart + FLASH_CHEATS, "CHDB", 4))
	{
		memcpy(rom + FLASH_CHEATS, cart + FLASH_CHEATS, FLASH_CHEATS_SIZE);
		printf("The cartridge's cheats are kept (-flashcheats writes the ROM's instead).\n");
	}
	if ((flash_keep & FLASH_KEEP_SETTINGS) && FlashAreaEmpty(rom + FLASH_ID_SECTOR, FLASH_SECTOR_SIZE) &&
		!memcmp(cart + FLASH_ID_SECTOR, "DVID", 4))
	{
		memcpy(rom + FLASH_ID_SECTOR, cart + FLASH_ID_SECTOR, FLASH_SECTOR_SIZE);
		printf("The cartridge's device ID and settings are kept.\n");
	}
	ok = true;
out:
	free(cart);
	return (ok);
}

bool	FlashRom(char *rom_file, uint8_t *rom, char *argv0)
{
	uint8_t		*image = NULL, *exe = NULL, *readback = NULL, c;
	int			size = FLASH_ROM_SIZE, exe_size, i, len = 0;
	uint32_t	sum = 0, exe_adrs;
	char		line[256];
	time_t		start;
	bool		ok = false, done = false;

	if (!(exe = SystemGetFile(GetToolPath(argv0, FLASHER_FILENAME), &exe_size)) || !(image = calloc(1, 32 + size)))
		goto out;
	if (!FlashPrepare(rom))
		goto out;

	// ROM image in RAM: "PS1 DEV CART ROM", size, sum, padding, ROM
	memcpy(image, "PS1 DEV CART ROM", 16);
	for (i = 0; i < size; i++)
		sum += rom[i];
	((uint32_t *)image)[4] = size;
	((uint32_t *)image)[5] = sum;
	memcpy(image + 32, rom, size);
	printf("Uploading the ROM %s...\n", rom_file);
	if (!USB_Process('U', image, FLASH_IMAGE_ADRS, 32 + size))
		goto usb_error;

	// Flasher, with printf to USB: its status lines end with an EOT character. The ROM image and the flasher are read
	// back from the console's RAM and compared before the flasher starts (nothing is erased if one differs)
	exe_adrs = *(uint32_t *)&exe[0x18];
	if (!USB_Process('U', exe + 2048, exe_adrs, exe_size - 2048) ||
		!(readback = malloc((32 + size > exe_size) ? 32 + size : exe_size)))
		goto usb_error;
	if (!USB_Process('D', readback, FLASH_IMAGE_ADRS, 32 + size))
		goto usb_error;
	if (memcmp(readback, image, 32 + size))
	{
		printf("\nThe ROM image in the console's RAM differs from the one sent: nothing flashed.\n");
		goto out;
	}
	if (!USB_Process('D', readback, exe_adrs, exe_size - 2048))
		goto usb_error;
	if (memcmp(readback, exe + 2048, exe_size - 2048))
	{
		printf("\nThe flasher in the console's RAM differs from the one sent: nothing flashed.\n");
		goto out;
	}
	free(readback);
	readback = NULL;
	printf("ROM image and flasher checked in the console's RAM. Starting the flasher...\n");
	if (!USB_Process('E', exe + 16, EXEC_TTY_MODE_USB, 15 * 4))
		goto usb_error;
	for (start = time(NULL); !done && (time(NULL) - start < FLASH_TIMEOUT); )
	{
		if (!USB_ReadByteNoWait(&c))
		{
			SleepMs(1);
			continue;
		}
		if ((c == 4) || (c == '\n') || (len >= sizeof(line) - 1))
		{
			line[len] = 0;
			if (len && (line[len - 1] == '\r'))
				line[--len] = 0;
			if (!strncmp(line, "FLASH", 5))	// Status lines (not the text of the libraries)
				printf("%s\n", line);
			if (!strncmp(line, "FLASH OK", 8))
				ok = true;
			len = 0;
			done = (c == 4);
		}
		else if (c != '\r')
			line[len++] = c;
	}
	if (!done)
		printf("The flasher didn't answer (look at the TV): the flash may not be written.\n");
	if (!ok)
		goto out;

	// New firmware: reset, then compare the flash with the ROM
	printf("Resetting the PS1...\n");
	USB_Process('R', NULL, 0, 0);
	SleepMs(5000);
#ifdef _WIN32
	USB_Purge(ftdi);
#endif
	ps1_program_running = true;
	USB_DetectPacketSize();
	if (fw_features & FEATURE_DEVICE_ID)
		USB_SendDeviceId();		// The device ID sector was erased
	if (!(readback = malloc(size)))
		goto out;
	printf("Reading the flash back...\n");
	if (!USB_Process('D', readback, 0x1F000000, size))
		goto usb_error;
	for (i = 0; (i < FLASH_ID_SECTOR) && (readback[i] == rom[i]); i++)
		;
	if (i < FLASH_ID_SECTOR)
	{
		printf("\nReadback differs from the ROM at offset 0x%05X: flash again.\n", i);
		ok = false;
	}
	else
		printf("\nFlash written and checked: identical to %s.\n", rom_file);
	goto out;

usb_error:
	printf("\nUSB error.\n");
	ok = false;
out:
	free(image);
	free(exe);
	free(readback);
	return (ok);
}

