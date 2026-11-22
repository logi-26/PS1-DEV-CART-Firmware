/*
* PS1 DEV CART: the firmware packed for the ROM (unpacked at boot by cartrom/rom.s, "unpack").
*
* rom_packer firmware.ps-exe body.bin out.pak
*   firmware.ps-exe: its 2048-byte header is kept as it is
*   body.bin: the loaded bytes (objcopy -O binary of the ELF: no padding)
*   out.pak: header (2048 bytes), body size (32 bits), "Firmware: x.y" (16 bytes, found in the body: the Manager reads
*   the version of a ROM file from it), then the body packed
*
* Packing (LZSS): a flag byte, bit 0 first, for the next 8 items: 1 a byte as it is, 0 a copy of earlier output,
* 2 bytes: offset - 1 (12 bits: bits 0-7, then bits 8-11 in the low half of the second byte), length - 3 (4 bits,
* the high half): 3 to 18 bytes up to 4096 bytes back. The packed data is unpacked here and compared.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define	HEADER		2048
#define	TAG_SIZE	16
#define	WINDOW		4096
#define	MIN_LEN		3
#define	MAX_LEN		18

static uint8_t	*ReadFile(const char *name, long *size)
{
	FILE	*f = fopen(name, "rb");
	uint8_t	*data;

	if (!f)
		return (NULL);
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(*size + 1);
	if (!data || (fread(data, 1, *size, f) != (size_t)*size))
	{
		fclose(f);
		return (NULL);
	}
	fclose(f);
	return (data);
}

static long	Pack(const uint8_t *in, long size, uint8_t *out)
{
	long	i = 0, o = 0, flag_pos, j, best, best_off, len;
	int		bit;

	while (i < size)
	{
		flag_pos = o++;
		out[flag_pos] = 0;
		for (bit = 0; (bit < 8) && (i < size); bit++)
		{
			best = best_off = 0;
			for (j = i - 1; (j >= 0) && (i - j <= WINDOW); j--)
			{
				for (len = 0; (len < MAX_LEN) && (i + len < size) && (in[j + len] == in[i + len]); len++)
					;
				if (len > best)
				{
					best = len;
					best_off = i - j;
					if (len == MAX_LEN)
						break;
				}
			}
			if (best >= MIN_LEN)
			{
				out[o++] = (best_off - 1) & 0xFF;
				out[o++] = ((best_off - 1) >> 8) | ((best - MIN_LEN) << 4);
				i += best;
			}
			else
			{
				out[flag_pos] |= 1 << bit;
				out[o++] = in[i++];
			}
		}
	}
	return (o);
}

// As cartrom/rom.s does it
static void	Unpack(const uint8_t *in, uint8_t *out, long size)
{
	long	o = 0, off, len;
	int		flags = 0, bits = 0;

	while (o < size)
	{
		if (!bits)
		{
			flags = *in++;
			bits = 8;
		}
		bits--;
		if (flags & 1)
			out[o++] = *in++;
		else
		{
			off = (in[0] | ((in[1] & 0x0F) << 8)) + 1;
			len = (in[1] >> 4) + MIN_LEN;
			in += 2;
			while (len--)
			{
				out[o] = out[o - off];
				o++;
			}
		}
		flags >>= 1;
	}
}

int		main(int argc, char *argv[])
{
	uint8_t		*exe, *body, *packed, *check, tag[TAG_SIZE];
	long		exe_size, size, packed_size, i;
	uint32_t	size32;
	FILE		*f;

	if (argc != 4)
	{
		printf("Usage: rom_packer firmware.ps-exe body.bin out.pak\n");
		return (1);
	}
	if (!(exe = ReadFile(argv[1], &exe_size)) || (exe_size < HEADER) || memcmp(exe, "PS-X EXE", 8) ||
		!(body = ReadFile(argv[2], &size)))
	{
		printf("rom_packer: can't read %s or %s\n", argv[1], argv[2]);
		return (1);
	}

	// The version text, found in the body
	memset(tag, 0, sizeof(tag));
	for (i = 0; i + 10 < size; i++)
		if (!memcmp(body + i, "Firmware: ", 10))
		{
			strncpy((char *)tag, (char *)body + i, sizeof(tag) - 1);
			break;
		}

	packed = malloc(size + size / 8 + 16);
	check = malloc(size + MAX_LEN);
	packed_size = Pack(body, size, packed);
	Unpack(packed, check, size);
	if (memcmp(check, body, size))
	{
		printf("rom_packer: the packed firmware doesn't unpack to the same bytes\n");
		return (1);
	}

	if (!(f = fopen(argv[3], "wb")))
	{
		printf("rom_packer: can't write %s\n", argv[3]);
		return (1);
	}
	size32 = size;
	fwrite(exe, 1, HEADER, f);
	fputc(size32 & 0xFF, f);
	fputc((size32 >> 8) & 0xFF, f);
	fputc((size32 >> 16) & 0xFF, f);
	fputc(size32 >> 24, f);
	fwrite(tag, 1, sizeof(tag), f);
	fwrite(packed, 1, packed_size, f);
	fclose(f);
	printf("Firmware packed: %ld bytes to %ld (%s)\n", size, packed_size, tag[0] ? (char *)tag : "no version");
	return (0);
}
