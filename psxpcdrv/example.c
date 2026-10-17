/*
* PS1USB libsn_usb use example by OrionSoft [2024]
* This example shows how to use the PS1 DEV CART file server feature using psxpcdrv (psxpcdrv.c/.h, named libsn_usb before)
* The function "DataManager_Load" will load a file either from CDROM or from your computer over USB depending on the CDROM_RELEASE define.
* The command for the tool to act as a file server is: ps1transfer -e -f devcart_example.ps-exe -c -p .
* "-p ." is indicating the local path where the files are stored
*
* PS1 DEV CART: built with PSn00bSDK ("make"; "make CDROM=1" for the CD-ROM version, "make iso" for its CD image,
* "make run" to run it with the file server).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>

// Defined ("make CDROM=1") for a CDROM release, otherwise the file is loaded from your computer over USB
//#define	CDROM_RELEASE

#ifdef	CDROM_RELEASE
#include <psxcd.h>
#define	CD_LOAD_RETRY	10
#else
#include "psxpcdrv.h"
#define	PC_READ_SIZE	2048
#endif

#define	BUFFER_SIZE		(2048*150)	// Must be a multiple of 2048 (whole sectors are read from the CD)

int	DataManager_Load(char *filename, unsigned char *buffer)
{
#ifdef CDROM_RELEASE
	int		i, sectors;
	CdlFILE	file;
	char	flname[64];

	// ALWAYS UPPER CASE FILENAME FOR CD LOADING !!
	snprintf(flname, sizeof(flname), "\\%s;1", filename);
	for (i = 0; flname[i]; i++)
		if ((flname[i] >= 'a') && (flname[i] <= 'z'))
			flname[i] -= 'a' - 'A';

	printf("Loading '%s' from CD_ROM\n", flname);

	for (i = 0; i < CD_LOAD_RETRY; i++)
		if (CdSearchFile(&file, flname))
			break;
	if (i == CD_LOAD_RETRY)
	{
		printf("File not found '%s'\n", flname);
		return (-1);
	}
	if (file.size > BUFFER_SIZE)
	{
		printf("File too big '%s'\n", flname);
		return (-1);
	}

	sectors = (file.size + 2047) / 2048;
	for (i = 0; i < CD_LOAD_RETRY; i++)
	{
		CdControl(CdlSetloc, &file.pos, NULL);
		CdRead(sectors, (uint32_t *)buffer, CdlModeSpeed);
		if (CdReadSync(0, NULL) == 0)	// Waits for the end of the read: 0 when all the sectors were read
			break;
	}
	if (i == CD_LOAD_RETRY)
	{
		printf("Error while reading file '%s'\n", flname);
		return (-1);
	}

	return (file.size);
#else
	int		i, file, size;

	printf("Loading '%s' from PC\n", filename);

	file = PCopen(filename, PCDRV_READ, 0);

	if (file < 0)
	{
		printf("File not found '%s'\n", filename);
		return (-1);
	}

	size = PClseek(file, 0, PCDRV_SEEK_END);
	PClseek(file, 0, PCDRV_SEEK_SET);
	if ((size < 0) || (size >= BUFFER_SIZE))
	{
		printf("File too big '%s'\n", filename);
		PCclose(file);
		return (-1);
	}

	i = size;
	while (i > PC_READ_SIZE)
	{
		PCread(file, (char *)buffer, PC_READ_SIZE);
		buffer += PC_READ_SIZE;
		i -= PC_READ_SIZE;
	}
	PCread(file, (char *)buffer, i);
	PCclose(file);

	return (size);
#endif
}

/****************************************/

uint8_t	CD_sector[BUFFER_SIZE + 1];	// (+1: end of the text)

int main(void)
{
	int	size;

	// PSn00bSDK: ResetGraph also turns the VBlank interrupt on (ResetCallback alone leaves all the interrupts off).
	// The kernel hook of the cartridge runs on interrupts: it answers the computer and redirects printf to it, so
	// one frame is waited before the first printf.
	ResetGraph(0);
	VSync(0);

	printf("PS1 DEV CART psxpcdrv example\n\n");
#ifdef	CDROM_RELEASE
	CdInit();
#else
	if (PCinit() != 0)
		printf("The computer doesn't answer: run this example with \"ps1transfer -e -f devcart_example.ps-exe -c\"\n");
#endif

	if ((size = DataManager_Load("test.txt", CD_sector)) >= 0)
	{
		CD_sector[size] = 0;
		printf("%s\n", (char *)CD_sector);
	}
	else
		printf("Error loading data !\n");

	while (1);

	return (0);
}
