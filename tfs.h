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

/****************************************/
// PS1 DEV CART: the ROM's files are packed (cartrom/rom_packer: size field with TFS_PACKED, data = unpacked size then
// LZSS, the firmware's format: see rom_packer/main.c). A test build's archive uploaded to RAM isn't (tim.tfs as it is).

#define	TFS_PACKED	0x80000000

// The file copied, unpacked if packed, to dst (RAM); its size (unpacked), -1 if not found
int		ROM_LoadFile(char *filename, uint8_t *dst)
{
	volatile uint8_t	*src;
	uint8_t				*out = dst, *end;
	int					size, flags = 0, bits = 0, off, len;

	if (!(src = ROM_GetFile(filename, &size)))
		return (-1);
	if (!(size & TFS_PACKED))
	{
		memcpy(dst, (const void *)src, size);
		return (size);
	}
	size = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
	src += 4;
	end = dst + size;
	while (out < end)
	{
		if (!bits)
		{
			flags = *src++;
			bits = 8;
		}
		bits--;
		if (flags & 1)
			*out++ = *src++;
		else
		{
			off = (src[0] | ((src[1] & 0x0F) << 8)) + 1;
			len = (src[1] >> 4) + 3;
			src += 2;
			while (len--)
			{
				*out = out[-off];
				out++;
			}
		}
		flags >>= 1;
	}
	return (size);
}
