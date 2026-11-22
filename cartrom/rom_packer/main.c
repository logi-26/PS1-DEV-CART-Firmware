/*
* PS1 DEV CART: the firmware and the menu images packed for the ROM.
*
* rom_packer firmware.ps-exe body.bin out.pak out2.pak max
*   firmware.ps-exe: its 2048-byte header is kept as it is
*   body.bin: the loaded bytes (objcopy -O binary of the ELF: no padding)
*   out.pak: header (2048 bytes), body size (32 bits), "Firmware: x.y" (16 bytes, found in the body: the Manager reads
*   the version of a ROM file from it), then the body packed, at most max bytes in all (the room before the cheats'
*   area, see cartrom/rom.s); out2.pak: the rest of the packed body (empty if it all fits), placed after the menu
*   images. The packed body is cut between two items, so the unpacker (cartrom/unpack.inc) only has to check where it
*   reads before each item.
*
* rom_packer -tfs in.tfs out.tfs
*   The menu images' archive (cartrom/tim.tfs, made by tfs_tool.py) with each file packed: its size field gets
*   TFS_PACKED (bit 31), its data is the unpacked size (32 bits) then the packed bytes (unpacked by tfs.h, ROM_LoadFile).
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
#define	TFS_ENTRY	32			// TFS: 24-byte name, offset, size (tfs.h, tfs_tool.py)
#define	TFS_NAME	24
#define	TFS_PACKED	0x80000000

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

static int	WriteFile(const char *name, const uint8_t *data, long size)
{
	FILE	*f = fopen(name, "wb");

	if (!f)
		return (0);
	if (size && (fwrite(data, 1, size, f) != (size_t)size))
	{
		fclose(f);
		return (0);
	}
	return (fclose(f) == 0);
}

static void	Put32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = v >> 24;
}

static uint32_t	Get32(const uint8_t *p)
{
	return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Packs in[size] into out; *cut (if not NULL): the end of the last item that ends at most at limit (in out)
static long	Pack(const uint8_t *in, long size, uint8_t *out, long limit, long *cut)
{
	long	i = 0, o = 0, flag_pos, j, best, best_off, len;
	int		bit;

	if (cut)
		*cut = 0;
	while (i < size)
	{
		flag_pos = o++;
		out[flag_pos] = 0;
		for (bit = 0; (bit < 8) && (i < size); bit++)
		{
			if (cut && (o <= limit))
				*cut = o;		// (an item starts here: the stream can be cut before it)
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
	if (cut && (o <= limit))
		*cut = o;
	return (o);
}

// As cartrom/unpack.inc and tfs.h do it
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

// Packs and checks; returns the packed size, -1 if it doesn't unpack to the same bytes
static long	PackChecked(const uint8_t *in, long size, uint8_t *out, long limit, long *cut)
{
	uint8_t	*check = malloc(size + MAX_LEN);
	long	packed = Pack(in, size, out, limit, cut);

	Unpack(out, check, size);
	if (memcmp(check, in, size))
		packed = -1;
	free(check);
	return (packed);
}

static int	PackTfs(const char *in_name, const char *out_name)
{
	uint8_t	*in, *out;
	long	size, o, n, i, packed, total = 0, total_packed = 0;

	if (!(in = ReadFile(in_name, &size)) || (size < 4) || memcmp(in, "TFS", 3))
	{
		printf("rom_packer: %s isn't a TFS archive\n", in_name);
		return (1);
	}
	n = in[3];
	out = malloc(size * 2 + 64);
	memcpy(out, in, 4 + n * TFS_ENTRY);
	o = 4 + n * TFS_ENTRY;
	for (i = 0; i < n; i++)
	{
		uint8_t		*entry = out + 4 + i * TFS_ENTRY;
		uint32_t	offset = Get32(entry + TFS_NAME), file_size = Get32(entry + TFS_NAME + 4);

		if ((file_size & TFS_PACKED) || (offset + file_size > (uint32_t)size))
		{
			printf("rom_packer: %s: bad entry %ld (already packed?)\n", in_name, i);
			return (1);
		}
		o = (o + 3) & ~3;	// (each file's data 32-bit aligned)
		Put32(out + o, file_size);
		if ((packed = PackChecked(in + offset, file_size, out + o + 4, 0, NULL)) < 0)
		{
			printf("rom_packer: %.24s doesn't unpack to the same bytes\n", (char *)entry);
			return (1);
		}
		Put32(entry + TFS_NAME, o);
		Put32(entry + TFS_NAME + 4, (4 + packed) | TFS_PACKED);
		o += 4 + packed;
		total += file_size;
		total_packed += 4 + packed;
	}
	if (!WriteFile(out_name, out, o))
	{
		printf("rom_packer: can't write %s\n", out_name);
		return (1);
	}
	printf("Menu images packed: %ld bytes to %ld (archive %ld bytes)\n", total, total_packed, o);
	return (0);
}

int		main(int argc, char *argv[])
{
	uint8_t		*exe, *body, *out, tag[TAG_SIZE];
	long		exe_size, size, packed_size, i, max, cut;

	if ((argc == 4) && !strcmp(argv[1], "-tfs"))
		return (PackTfs(argv[2], argv[3]));
	if (argc != 6)
	{
		printf("Usage: rom_packer firmware.ps-exe body.bin out.pak out2.pak max\n"
			"       rom_packer -tfs in.tfs out.tfs\n");
		return (1);
	}
	if (!(exe = ReadFile(argv[1], &exe_size)) || (exe_size < HEADER) || memcmp(exe, "PS-X EXE", 8) ||
		!(body = ReadFile(argv[2], &size)))
	{
		printf("rom_packer: can't read %s or %s\n", argv[1], argv[2]);
		return (1);
	}
	max = strtol(argv[5], NULL, 0);
	if (max <= HEADER + 4 + TAG_SIZE)
	{
		printf("rom_packer: max %s is too small\n", argv[5]);
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

	out = malloc(HEADER + 4 + TAG_SIZE + size + size / 8 + 16);
	memcpy(out, exe, HEADER);
	Put32(out + HEADER, size);
	memcpy(out + HEADER + 4, tag, TAG_SIZE);
	if ((packed_size = PackChecked(body, size, out + HEADER + 4 + TAG_SIZE, max - (HEADER + 4 + TAG_SIZE), &cut)) < 0)
	{
		printf("rom_packer: the packed firmware doesn't unpack to the same bytes\n");
		return (1);
	}
	cut += HEADER + 4 + TAG_SIZE;
	if (!WriteFile(argv[3], out, cut) ||
		!WriteFile(argv[4], out + cut, HEADER + 4 + TAG_SIZE + packed_size - cut))
	{
		printf("rom_packer: can't write %s or %s\n", argv[3], argv[4]);
		return (1);
	}
	printf("Firmware packed: %ld bytes to %ld (%s): %ld before the cheats (room %ld), %ld after the menu images\n",
		size, packed_size, tag[0] ? (char *)tag : "no version", cut, max,
		HEADER + 4 + TAG_SIZE + packed_size - cut);
	return (0);
}
