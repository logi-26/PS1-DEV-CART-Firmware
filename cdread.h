/*
* PS1 DEV CART: the disc read with polled drive commands, no BIOS CD driver. Shared by the firmware (the disc boot,
* ps1devcart.c) and the debug monitor (Load and LoadExec of "cdrom:" files for a game the firmware booted, see
* monitor/monitor.c): no C library and no global buffer (the monitor runs from the cartridge flash).
*
* The includer defines: CD_REG(n) and IRQ_STAT (the registers), CD_DELAY_US(us) (a busy wait), CD_CRITICAL_ENTER()
* and CD_CRITICAL_EXIT() (interrupts off and back as they were, around each read).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _CDREAD_H_
#define _CDREAD_H_

#include <stdint.h>
#include <stdbool.h>

#define	CD_SECTOR		2048
#define	CD_BCD(n)		((((n) / 10) << 4) | ((n) % 10))
#define	CD_LE32(p)		((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((p)[3] << 24))

// Next response of the drive (timeout in 100us steps): its interrupt type, 0 on timeout
static int	CD_RawResponse(uint8_t *resp, int resp_size, int timeout)
{
	int		irq, n;

	CD_REG(0) = 1;
	while (!(irq = CD_REG(3) & 7))
	{
		if (!timeout--)
			return (0);
		CD_DELAY_US(100);
	}
	for (n = 0; CD_REG(0) & 0x20; n++)	// Response FIFO
	{
		uint8_t	c = CD_REG(1);

		if (n < resp_size)
			resp[n] = c;
	}
	if (n < resp_size)
		resp[n] = 0;
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;
	IRQ_STAT = ~(1 << 2);	// (the CPU side of the CD interrupt, not taken in the critical section)
	return (irq);
}

// Raw CD command, polled with interrupts disabled (the BIOS and PSn00bSDK don't know the secret commands): returns the
// interrupt type of the first response (3 = acknowledge, 5 = error), 0 if the drive doesn't answer (about 1 second).
// Sectors (INT1) and data end (INT4) of a read or play still going on are skipped: after a reset of the console while
// a game read the disc, they were taken for the answer.
static int	CD_RawCommand(uint8_t cmd, const void *param, int len, uint8_t *resp, int resp_size)
{
	const uint8_t	*p = param;
	int				timeout, irq;

	for (timeout = 10000; (CD_REG(0) & 0x80) && timeout; timeout--)	// Busy with the previous command
		CD_DELAY_US(100);
	CD_REG(0) = 1;
	CD_REG(3) = 0x1F;	// Acknowledge the interrupts
	CD_REG(3) = 0x40;	// Clear the parameter FIFO
	CD_REG(0) = 0;
	while (len-- > 0)
		CD_REG(2) = *p++;
	CD_REG(1) = cmd;
	for (timeout = 0; timeout < 100; timeout++)
		if (((irq = CD_RawResponse(resp, resp_size, 10000)) != 1) && (irq != 4))
			break;
	return (irq);
}

// Sectors read with polled drive commands, interrupts disabled: the BIOS driver (read() and Load) lost the first
// sectors of the program (29 with a burned disc: the program was 0xE800 bytes too low in RAM). One INT1 a sector, its
// data read before the acknowledge. Single speed: at double speed, sectors were lost in the middle of the program.
// After an error the read starts again from the sector not read.
static bool	CD_ReadSectors(int lba, uint8_t *dst, uint32_t size)
{
	uint8_t	resp[8], loc[3], mode = 0x00, c;	// (mode: single speed, 2048-byte sectors)
	int		retry, timeout, irq, pos, i, n;

	CD_CRITICAL_ENTER();
	if (CD_RawCommand(0x09, 0, 0, resp, sizeof(resp)) == 3)	// Pause: a read or play still going on is stopped
		CD_RawResponse(resp, sizeof(resp), 50000);
	CD_RawCommand(0x0E, &mode, 1, resp, sizeof(resp));	// Setmode
	for (retry = 0; size && (retry < 8); retry++)
	{
		pos = lba + 150;	// (2 seconds of lead-in)
		loc[0] = CD_BCD(pos / 4500);
		loc[1] = CD_BCD((pos / 75) % 60);
		loc[2] = CD_BCD(pos % 75);
		if (((irq = CD_RawCommand(0x02, loc, 3, resp, sizeof(resp))) != 3) ||
			((irq = CD_RawCommand(0x06, 0, 0, resp, sizeof(resp))) != 3))	// Setloc, ReadN
			continue;
		while (size)
		{
			CD_REG(0) = 1;
			for (timeout = 30000; !(irq = CD_REG(3) & 7) && timeout; timeout--)	// (3 seconds: seek, spin up)
				CD_DELAY_US(100);
			if (irq != 1)	// Not a sector: error (or timeout), read again
				break;
			while (CD_REG(0) & 0x20)	// Response FIFO (status)
				c = CD_REG(1);
			CD_REG(0) = 0;
			CD_REG(3) = 0;
			CD_REG(3) = 0x80;	// Want data
			for (timeout = 1000; !(CD_REG(0) & 0x40) && timeout; timeout--)	// Data FIFO not empty
				CD_DELAY_US(10);
			n = (size < CD_SECTOR) ? size : CD_SECTOR;
			for (i = 0; i < CD_SECTOR; i++)
			{
				c = CD_REG(2);
				if (i < n)
					*dst++ = c;
			}
			CD_REG(3) = 0;
			CD_REG(0) = 1;
			CD_REG(3) = 0x1F;	// Acknowledge: the next sector
			IRQ_STAT = ~(1 << 2);
			size -= n;
			lba++;
		}
	}
	if (CD_RawCommand(0x09, 0, 0, resp, sizeof(resp)) == 3)	// Pause: the drive stops reading
		CD_RawResponse(resp, sizeof(resp), 50000);
	CD_CRITICAL_EXIT();
	(void)c;
	return (!size);
}

// ISO 9660 name (with or without ";1", "NAME." for no extension) against a path part, case not significant
static bool	CD_NameMatch(const uint8_t *name, int name_len, const char *part, int len)
{
	int		i;

	for (i = 0; (i < name_len) && (name[i] != ';'); i++)
		;
	if (i && (name[i - 1] == '.'))
		i--;
	name_len = i;
	for (i = 0; (i < len) && (part[i] != ';'); i++)
		;
	if (i != name_len)
		return (false);
	while (i--)
		if ((name[i] | 0x20) != (part[i] | 0x20))
			return (false);
	return (true);
}

// "cdrom:" (case not significant) at the start of a path: the length to skip, 0 if it isn't there
static int	CD_DevicePrefix(const char *path)
{
	static const char	dev[] = "cdrom:";
	int					i;

	for (i = 0; dev[i]; i++)
		if ((path[i] | 0x20) != dev[i])
			return (0);
	return (i);
}

// File of the disc ("cdrom:\DIR\NAME.EXT;1"): its first sector and size, found in the directories (no BIOS CD driver:
// it retried a read error forever, the menu hung). sec: a sector buffer. Returns -1 if the disc can't be read, 0 if the
// file isn't there.
static int	CD_FindFile(const char *path, uint32_t *size, uint8_t *sec)
{
	static const char	cd001[] = "CD001";
	uint8_t				*r;
	const char			*end;
	uint32_t			dir_size, off;
	int					lba, len, i;
	bool				found;

	path += CD_DevicePrefix(path);
	if (!CD_ReadSectors(16, sec, CD_SECTOR))	// Primary volume descriptor
		return (-1);
	for (i = 0; i < 5; i++)
		if (sec[1 + i] != cd001[i])
			return (-1);
	lba = CD_LE32(sec + 156 + 2);	// Root directory
	dir_size = CD_LE32(sec + 156 + 10);
	for (;;)
	{
		while ((*path == '\\') || (*path == '/'))
			path++;
		for (end = path; *end && (*end != '\\') && (*end != '/'); end++)
			;
		if (!(len = end - path))
			return (0);
		for (found = false, off = 0; !found && (off < dir_size); off += CD_SECTOR)
		{
			if (!CD_ReadSectors(lba + off / CD_SECTOR, sec, CD_SECTOR))
				return (-1);
			for (i = 0; (i + 33 < CD_SECTOR) && sec[i] && (i + sec[i] <= CD_SECTOR); i += sec[i])	// (records don't cross sectors)
			{
				r = sec + i;
				if ((33 + r[32] <= r[0]) && CD_NameMatch(r + 33, r[32], path, len))
				{
					lba = CD_LE32(r + 2);
					dir_size = CD_LE32(r + 10);
					found = true;
					break;
				}
			}
		}
		if (!found)
			return (0);
		if (!*end)
		{
			*size = dir_size;
			return (lba);
		}
		path = end;
	}
}

#endif
