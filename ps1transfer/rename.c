/*
* PS1 DEV CART Transfer tool: the cartridge's name (the USB serial number in the FT245R's EEPROM)
*
*	-n SERIAL -renameinfo				what the EEPROM holds (key=value lines, read only)
*	-n SERIAL -rename NEWNAME			the serial number changed
*	-n SERIAL -renamerestore FILE		the EEPROM programmed again from a backup
*
* Only the FT245R's EEPROM is written (the PS1 and the cartridge flash aren't used), and only with FTDI's own
* EEPROM programming function (FT_EEPROM_Program, as FT_Prog). Words written one by one (FT_WriteEE) are not used:
* on this chip they erased the whole EEPROM instead (measured). Each step is checked:
* - the cartridge given by its serial number (-n): never "the first one found";
* - an FT245R/FT232R with FTDI's IDs (0403:6001), its configuration read by FTDI's library and the raw words agreeing
*   (VID/PID, the serial number, the checksum);
* - the name 1 to 15 letters or digits, not used by another device, the strings fitting in the EEPROM;
* - the configuration saved to a file first (every setting and string, and the raw words), read back and compared;
* - the configuration as read, only the serial number changed, programmed;
* - read back afterwards: every setting and the other strings as before, the new name there. If not: the backup
*   programmed again (and checked).
* Then the chip is reconnected with its new name, and opened by it.
*
* FTDI's function writes the FT232R's standard values for two descriptor fields that FT_Prog doesn't show (device
* release 0x0600, USB version 0x0200, which some PS1USB cartridges have at 0), and clears the bytes after the strings:
* the first rename of such a cartridge changes these too (nothing uses them: the chip and the driver work the same).
*
* An FT245R with a wrong EEPROM is still reachable over USB (FTDI's FT_Prog can program it again).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rename.h"

#ifdef _WIN32
#include <windows.h>
#include "ftd2xx.h"

// FT232R/FT245R internal EEPROM: 1024 bits
#define	EE_WORDS		64
#define	EE_VID			1
#define	EE_PID			2
#define	EE_CHECKSUM		63
#define	NAME_MAX_LEN	15
#define	STRINGS_MAX		48			// Manufacturer + description + serial number (FT232R string area)

typedef struct
{
	FT_EEPROM_232R	ee;
	char			manufacturer[64], manufacturer_id[64], description[64], serial[64];
} Config;

// The configuration's settings, by name (the backup file: name=value lines)
typedef struct
{
	const char	*name;
	size_t		offset, size;
} Field;

#define	FIELD(_n_, _f_)	{_n_, offsetof(FT_EEPROM_232R, _f_), sizeof(((FT_EEPROM_232R *)0)->_f_)}
static const Field	fields[] =
{
	FIELD("vid", common.VendorId), FIELD("pid", common.ProductId), FIELD("serial_enable", common.SerNumEnable),
	FIELD("max_power", common.MaxPower), FIELD("self_powered", common.SelfPowered),
	FIELD("remote_wakeup", common.RemoteWakeup), FIELD("pull_down", common.PullDownEnable),
	FIELD("high_current", IsHighCurrent), FIELD("ext_osc", UseExtOsc), FIELD("invert_txd", InvertTXD),
	FIELD("invert_rxd", InvertRXD), FIELD("invert_rts", InvertRTS), FIELD("invert_cts", InvertCTS),
	FIELD("invert_dtr", InvertDTR), FIELD("invert_dsr", InvertDSR), FIELD("invert_dcd", InvertDCD),
	FIELD("invert_ri", InvertRI), FIELD("cbus0", Cbus0), FIELD("cbus1", Cbus1), FIELD("cbus2", Cbus2),
	FIELD("cbus3", Cbus3), FIELD("cbus4", Cbus4), FIELD("driver_type", DriverType)
};
#define	FIELDS	((int)(sizeof(fields) / sizeof(fields[0])))

static unsigned int	GetField(const Config *c, const Field *f)
{
	const uint8_t	*p = (const uint8_t *)&c->ee + f->offset;

	return ((f->size == 2) ? *(const uint16_t *)p : *p);
}

static void	SetField(Config *c, const Field *f, unsigned int v)
{
	uint8_t	*p = (uint8_t *)&c->ee + f->offset;

	if (f->size == 2)
		*(uint16_t *)p = v;
	else
		*p = v;
}

static uint16_t	Checksum(const uint16_t *raw)
{
	uint16_t	c = 0xAAAA;
	int			i;

	for (i = 0; i < EE_CHECKSUM; i++)
	{
		c ^= raw[i];
		c = (c << 1) | (c >> 15);
	}
	return (c);
}

static bool	ReadConfig(FT_HANDLE h, Config *c)
{
	memset(c, 0, sizeof(*c));
	c->ee.common.deviceType = FT_DEVICE_232R;
	return (FT_EEPROM_Read(h, &c->ee, sizeof(c->ee), c->manufacturer, c->manufacturer_id, c->description,
		c->serial) == FT_OK);
}

static bool	ReadRaw(FT_HANDLE h, uint16_t *raw)
{
	WORD	w;
	int		i;

	for (i = 0; i < EE_WORDS; i++)
	{
		if (FT_ReadEE(h, i, &w) != FT_OK)
			return (false);
		raw[i] = w;
	}
	return (true);
}

// The settings and the other strings the same (the serial number aside)
static bool	SameConfig(const Config *a, const Config *b)
{
	int	i;

	for (i = 0; i < FIELDS; i++)
	{
		if (GetField(a, &fields[i]) != GetField(b, &fields[i]))
			return (false);
	}
	return (!strcmp(a->manufacturer, b->manufacturer) && !strcmp(a->description, b->description));
}

// c programmed with this serial number, then read back and checked: the same settings, the serial number there
static bool	Program(FT_HANDLE h, const Config *c, const char *serial)
{
	Config		w = *c, check;
	uint16_t	raw[EE_WORDS];
	char		s[64];

	snprintf(s, sizeof(s), "%s", serial);
	w.ee.common.deviceType = FT_DEVICE_232R;
	w.ee.common.SerNumEnable = 1;
	if (FT_EEPROM_Program(h, &w.ee, sizeof(w.ee), w.manufacturer, w.manufacturer_id, w.description, s) != FT_OK)
		return (false);
	return (ReadConfig(h, &check) && SameConfig(&check, &w) && !strcmp(check.serial, serial) && ReadRaw(h, raw) &&
		(raw[EE_VID] == 0x0403) && (raw[EE_PID] == 0x6001) && (raw[EE_CHECKSUM] == Checksum(raw)));
}

// The cartridge by its serial number, checked to be an FT245R/FT232R with FTDI's IDs
static FT_HANDLE	Open(const char *serial)
{
	FT_HANDLE	h;
	FT_DEVICE	type;
	DWORD		id;
	char		s[64], desc[64];

	if (!serial)
	{
		printf("The cartridge is given by its serial number: -n SERIAL (see -l).\n");
		return (NULL);
	}
	if (FT_OpenEx((PVOID)serial, FT_OPEN_BY_SERIAL_NUMBER, &h) != FT_OK)
	{
		printf("Can't open the cartridge %s: not connected, or in use by another program (close it first).\n", serial);
		return (NULL);
	}
	if ((FT_GetDeviceInfo(h, &type, &id, s, desc, NULL) != FT_OK) || (type != FT_DEVICE_232R) || (id != 0x04036001))
	{
		printf("%s isn't an FT245R with FTDI's USB IDs (0403:6001): left as it is.\n", serial);
		FT_Close(h);
		return (NULL);
	}
	return (h);
}

// The EEPROM read both ways and agreeing
static bool	Inspect(FT_HANDLE h, const char *serial, uint16_t *raw, Config *c)
{
	if (!ReadRaw(h, raw) || !ReadConfig(h, c))
	{
		printf("Can't read the EEPROM of %s.\n", serial);
		return (false);
	}
	if ((raw[EE_VID] != 0x0403) || (raw[EE_PID] != 0x6001) || (c->ee.common.VendorId != 0x0403) ||
		(c->ee.common.ProductId != 0x6001) || !c->ee.common.SerNumEnable || strcmp(c->serial, serial) ||
		(raw[EE_CHECKSUM] != Checksum(raw)))
	{
		printf("The EEPROM of %s doesn't read as expected: left as it is (FTDI's FT_Prog can rename it).\n", serial);
		return (false);
	}
	return (true);
}

static int	NameRoom(const Config *c)
{
	int	room = STRINGS_MAX - (int)strlen(c->manufacturer) - (int)strlen(c->description);

	return ((room > NAME_MAX_LEN) ? NAME_MAX_LEN : room);
}

static void	WriteConfig(FILE *f, const Config *c, const uint16_t *raw)
{
	int	i;

	fprintf(f, "serial=%s\nmanufacturer=%s\nmanufacturer_id=%s\ndescription=%s\n", c->serial, c->manufacturer,
		c->manufacturer_id, c->description);
	for (i = 0; i < FIELDS; i++)
		fprintf(f, "%s=%u\n", fields[i].name, GetField(c, &fields[i]));
	if (raw)
	{
		fprintf(f, "raw=");
		for (i = 0; i < EE_WORDS; i++)
			fprintf(f, "%04X", raw[i]);
		fprintf(f, "\n");
	}
}

// A backup: every line of WriteConfig needed (the raw words are only a record)
static bool	ReadBackup(const char *path, Config *c)
{
	char	line[512], *v;
	FILE	*f;
	int		i, found = 0, strings = 0;

	memset(c, 0, sizeof(*c));
	if (!(f = fopen(path, "r")))
		return (false);
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!(v = strchr(line, '=')))
			continue;
		*v++ = 0;
		if (strlen(v) >= 64)
			continue;
		if (!strcmp(line, "serial"))
			strings |= 1, strcpy(c->serial, v);
		else if (!strcmp(line, "manufacturer"))
			strings |= 2, strcpy(c->manufacturer, v);
		else if (!strcmp(line, "manufacturer_id"))
			strings |= 4, strcpy(c->manufacturer_id, v);
		else if (!strcmp(line, "description"))
			strings |= 8, strcpy(c->description, v);
		for (i = 0; i < FIELDS; i++)
		{
			if (!strcmp(line, fields[i].name))
			{
				SetField(c, &fields[i], (unsigned int)strtoul(v, NULL, 10));
				found++;
			}
		}
	}
	fclose(f);
	return ((strings == 15) && (found == FIELDS) && (c->ee.common.VendorId == 0x0403) &&
		(c->ee.common.ProductId == 0x6001) && c->serial[0]);
}

// Backups: %LOCALAPPDATA%\PS1-DEV-CART\eeprom-backups\SERIAL-date.txt, read back and compared
static bool	SaveBackup(const Config *c, const uint16_t *raw, char *path, size_t size)
{
	char		dir[MAX_PATH], stamp[32];
	const char	*base = getenv("LOCALAPPDATA");
	time_t		t = time(NULL);
	Config		check;
	FILE		*f;

	snprintf(dir, sizeof(dir), "%s\\PS1-DEV-CART", base ? base : ".");
	CreateDirectoryA(dir, NULL);
	strncat(dir, "\\eeprom-backups", sizeof(dir) - strlen(dir) - 1);
	CreateDirectoryA(dir, NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(path, size, "%s\\%s-%s.txt", dir, c->serial, stamp);
	if (!(f = fopen(path, "w")))
		return (false);
	fprintf(f, "# PS1 DEV CART: EEPROM of the cartridge's USB chip (FT245R), before -rename.\n"
		"# Put back with: ps1transfer -n NAME -renamerestore \"%s\"\n", path);
	WriteConfig(f, c, raw);
	if (fclose(f))
		return (false);
	return (ReadBackup(path, &check) && SameConfig(&check, c) && !strcmp(check.serial, c->serial) &&
		!strcmp(check.manufacturer_id, c->manufacturer_id));
}

// After a change: the chip reconnected (its new descriptors read by Windows), then opened by the name it should have
static bool	Reconnect(FT_HANDLE h, const char *serial)
{
	FT_HANDLE	h2;
	int			i;

	FT_CyclePort(h);
	FT_Close(h);
	for (i = 0; i < 40; i++)		// (up to 10 seconds)
	{
		Sleep(250);
		if (FT_OpenEx((PVOID)serial, FT_OPEN_BY_SERIAL_NUMBER, &h2) == FT_OK)
		{
			FT_Close(h2);
			return (true);
		}
	}
	return (false);
}

static bool	NameValid(const char *name, int room, const char *serial)
{
	DWORD		i, n = 0, flags, type, id, loc;
	char		s[64], desc[64];
	FT_HANDLE	other;
	size_t		len = strlen(name), k;

	if ((len < 1) || ((int)len > room))
	{
		printf("The name has 1 to %d characters.\n", room);
		return (false);
	}
	for (k = 0; k < len; k++)
	{
		if (!isalnum((unsigned char)name[k]))
		{
			printf("The name has only letters A-Z and digits 0-9 ('%c' isn't).\n", name[k]);
			return (false);
		}
	}
	if (FT_CreateDeviceInfoList(&n) == FT_OK)
	{
		for (i = 0; i < n; i++)
		{
			if ((FT_GetDeviceInfoDetail(i, &flags, &type, &id, &loc, s, desc, &other) == FT_OK) &&
				!_stricmp(s, name) && _stricmp(s, serial))
			{
				printf("Another device connected is already named %s.\n", name);
				return (false);
			}
		}
	}
	return (true);
}

// After a failed programming: the configuration as it was, programmed again
static int	PutBack(FT_HANDLE h, const Config *c, const char *backup)
{
	printf("The EEPROM didn't read back as expected: programming it back as it was...\n");
	if (Program(h, c, c->serial))
	{
		printf("The EEPROM is as it was (%s): not renamed.\n", c->serial);
		Reconnect(h, c->serial);
		return (EXIT_FAILURE);
	}
	printf("Couldn't program it back. The chip is still reachable: unplug the cartridge and plug it back in, then "
		"ps1transfer -n SERIAL -renamerestore \"%s\" (SERIAL as -l shows it), or use FTDI's FT_Prog.\n", backup);
	FT_Close(h);
	return (EXIT_FAILURE);
}

int	CartRename(const char *serial, const char *name)
{
	FT_HANDLE	h;
	Config		before;
	uint16_t	raw[EE_WORDS];
	char		backup[MAX_PATH + 64];

	if (!(h = Open(serial)))
		return (EXIT_FAILURE);
	if (!Inspect(h, serial, raw, &before))
	{
		FT_Close(h);
		return (EXIT_FAILURE);
	}
	if (!name)
	{
		WriteConfig(stdout, &before, raw);
		printf("name_max=%d\n", NameRoom(&before));
		FT_Close(h);
		return (EXIT_SUCCESS);
	}
	if (!strcmp(name, serial))
	{
		printf("The cartridge is already named %s.\n", name);
		FT_Close(h);
		return (EXIT_SUCCESS);
	}
	if (!NameValid(name, NameRoom(&before), serial))
	{
		FT_Close(h);
		return (EXIT_FAILURE);
	}
	if (!SaveBackup(&before, raw, backup, sizeof(backup)))
	{
		printf("Can't save the EEPROM backup (%s): left as it is.\n", backup);
		FT_Close(h);
		return (EXIT_FAILURE);
	}
	printf("backup=%s\n", backup);
	if (!Program(h, &before, name))
		return (PutBack(h, &before, backup));
	printf("Renamed %s to %s (checked). Reconnecting...\n", serial, name);
	if (!Reconnect(h, name))
	{
		printf("The cartridge doesn't answer as %s yet: unplug it and plug it back in.\n", name);
		return (EXIT_FAILURE);
	}
	printf("rename=ok\nserial=%s\n", name);
	return (EXIT_SUCCESS);
}

int	CartRenameRestore(const char *serial, const char *file)
{
	FT_HANDLE	h;
	Config		saved, now;
	uint16_t	raw[EE_WORDS];

	if (!ReadBackup(file, &saved))
	{
		printf("%s isn't a backup made by -rename.\n", file);
		return (EXIT_FAILURE);
	}
	if (!(h = Open(serial)))
		return (EXIT_FAILURE);
	if (!Inspect(h, serial, raw, &now))		// (only to know it's an EEPROM understood here)
	{
		FT_Close(h);
		return (EXIT_FAILURE);
	}
	if (!Program(h, &saved, saved.serial))
		return (PutBack(h, &now, file));
	printf("Restored the EEPROM from %s (name: %s). Reconnecting...\n", file, saved.serial);
	if (!Reconnect(h, saved.serial))
	{
		printf("The cartridge doesn't answer as %s yet: unplug it and plug it back in.\n", saved.serial);
		return (EXIT_FAILURE);
	}
	printf("rename=ok\nserial=%s\n", saved.serial);
	return (EXIT_SUCCESS);
}

#else

int	CartRename(const char *serial, const char *name)
{
	printf("Renaming the cartridge isn't in this build yet (Windows only): FTDI's FT_Prog, or ftdi_eeprom from "
		"libftdi, can do it.\n");
	return (EXIT_FAILURE);
}

int	CartRenameRestore(const char *serial, const char *file)
{
	return (CartRename(serial, NULL));
}

#endif
