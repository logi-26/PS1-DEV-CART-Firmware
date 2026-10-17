/*
* PS1USB firmware by OrionSoft [2024]
* Tiny File System to load data from ROM
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#define FILENAME_MAX_LEN    (32-(4+4+1))

volatile uint8_t	*ROM_GetFile(char *filename, int *size)
{
	int					i, n, *ptr32;
	char				*fnameptr, *flname;
	volatile uint8_t	*ofs, *fs = (volatile uint8_t *)0x1F010000;	// TFS in ROM

#ifdef TFS_RAM_ADRS
	// Firmware test build ("make RAMTEST=1"): menu images uploaded to RAM are used instead of the ROM ones
	// (ps1transfer -u -f cartrom/tim.tfs -a 0x80140000, before executing the test firmware)
	if ((((volatile uint8_t *)TFS_RAM_ADRS)[0] == 'T') && (((volatile uint8_t *)TFS_RAM_ADRS)[1] == 'F') &&
		(((volatile uint8_t *)TFS_RAM_ADRS)[2] == 'S'))
		fs = (volatile uint8_t *)TFS_RAM_ADRS;
#endif
	ofs = fs;	// Keep oririnal pointer
	fs += 3;	// Skip Signature
	n = *fs++;	// Number of Files in archive

	i = 0;
	for (i = 0; i < n; i++)
	{
		fnameptr = (char*)fs;
		flname = filename;
		while (*flname)						// Compare filenames
		{
			if ((*flname++) != (*fnameptr++))
				goto next;
		}
		fs += FILENAME_MAX_LEN+1;		// Skip filename
		ptr32 = (int*)fs;
		ofs += (*ptr32++);				// File data offset
		if (size)
			*size = *ptr32;				// File size
		return (ofs);
next:
		fs += 32;
	}

	return (NULL);	// File not found !
}
