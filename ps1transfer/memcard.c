/*
* PS1 DEV CART Transfer tool: memory cards: backup/restore, card image tools, commands on the card itself (from main.c)
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
// Memory card backup/restore: cardtool.ps-exe (built from the cardtool folder) is executed with a command block,
// the backup is written by the PS1 program with the PCDRV file server, see ps1-tools/cardtool/cardtool.c

bool	exit_on_eot;

// Saves listed from the directory frames (sectors 1-15): state 0x51 = first block of a save, size, file name
void	PrintCardDirectory(uint8_t *img)
{
	int	i, n = 0, used = 0;

	if (memcmp(img, "MC", 2))
	{
		printf("Not a formatted memory card image (no \"MC\" header).\n");
		return;
	}
	printf("Memory card saves:\n");
	for (i = 1; i < 16; i++)
	{
		uint8_t		*e = img + i * 128;
		uint32_t	size = e[4] | (e[5] << 8) | (e[6] << 16) | (e[7] << 24);

		if ((e[0] & 0xF0) == 0x50)
			used++;
		if (e[0] == 0x51)
		{
			printf("  block %2d: %-20.20s %u block(s)\n", i, (char *)e + 10, size / 8192);
			n++;
		}
	}
	printf("%d save(s), %d/15 blocks used\n", n, used);
}

/****************************************/
// Memory card image tools (on the computer, no PS1 needed): list with titles/icons, export/import .mcs saves,
// delete, copy between images, format. Card image: 16 blocks of 8KB, block 0 = header + directory frames (1-15),
// one directory frame per block: state, size (first block), next block, file name, XOR checksum.

static uint8_t	*McFrame(uint8_t *img, int block)	{return (img + block * MC_FRAME);}	// Directory frame of block 1-15

static void	McChecksum(uint8_t *frame)
{
	uint8_t	x = 0;
	int		i;

	for (i = 0; i < MC_FRAME - 1; i++)
		x ^= frame[i];
	frame[MC_FRAME - 1] = x;
}

static void	McSetEntry(uint8_t *img, int block, uint32_t state, uint32_t size, uint16_t next, const char *name)
{
	uint8_t	*f = McFrame(img, block);

	memset(f, 0, MC_FRAME);
	f[0] = state;
	f[4] = size;
	f[5] = size >> 8;
	f[6] = size >> 16;
	f[7] = size >> 24;
	f[8] = next;
	f[9] = next >> 8;
	if (name)
		memcpy(f + 10, name, strnlen(name, 20));	// 20 characters max, the frame is zero filled
	McChecksum(f);
}

// Blank formatted card: "MC" header, 15 free blocks, empty broken sector list
static void	McFormat(uint8_t *img)
{
	int	i;

	memset(img, 0, CARD_SIZE);
	img[0] = 'M';
	img[1] = 'C';
	McChecksum(img);
	for (i = 1; i < 16; i++)
		McSetEntry(img, i, MC_STATE_FREE, 0, 0xFFFF, NULL);
	for (i = 16; i < 36; i++)	// Broken sector list
	{
		memset(McFrame(img, i), 0, MC_FRAME);
		memset(McFrame(img, i), 0xFF, 4);
		McFrame(img, i)[8] = 0xFF;
		McFrame(img, i)[9] = 0xFF;
		McChecksum(McFrame(img, i));
	}
}

// Shift-JIS title (save title frame) to ASCII: full width letters, digits and common signs, others '?'
static void	McTitle(uint8_t *sjis, char *out, int max)
{
	static const struct {uint16_t code; char c;} signs[] = {{0x8140, ' '}, {0x8143, ','}, {0x8144, '.'}, {0x8146, ':'},
		{0x8147, ';'}, {0x8148, '?'}, {0x8149, '!'}, {0x815E, '/'}, {0x8169, '('}, {0x816A, ')'}, {0x817B, '+'},
		{0x817C, '-'}, {0x8181, '='}, {0x815B, '-'}, {0x8160, '~'}, {0x8175, '['}, {0x8176, ']'}, {0x8193, '%'}};
	int	i = 0, n = 0, k;

	while ((i < 64) && sjis[i] && (n < max - 1))
	{
		if (sjis[i] < 0x80)
			out[n++] = sjis[i++];
		else
		{
			uint16_t	c = (sjis[i] << 8) | sjis[i + 1];
			char		a = '?';

			if ((c >= 0x824F) && (c <= 0x8258))	a = '0' + c - 0x824F;
			else if ((c >= 0x8260) && (c <= 0x8279))	a = 'A' + c - 0x8260;
			else if ((c >= 0x8281) && (c <= 0x829A))	a = 'a' + c - 0x8281;
			else
				for (k = 0; k < sizeof(signs) / sizeof(signs[0]); k++)
					if (signs[k].code == c)
						a = signs[k].c;
			out[n++] = a;
			i += 2;
		}
	}
	while (n && (out[n - 1] == ' '))
		n--;
	out[n] = 0;
}

// Save: block number (1-15) or file name, returns the first block or 0
static int	McFind(uint8_t *img, char *save)
{
	int	i, n = atoi(save);

	if ((n >= 1) && (n <= 15) && (McFrame(img, n)[0] == MC_STATE_FIRST))
		return (n);
	for (i = 1; i < 16; i++)
		if ((McFrame(img, i)[0] == MC_STATE_FIRST) && !strncmp((char *)McFrame(img, i) + 10, save, 20))
			return (i);
	printf("Save '%s' not found (give its file name or block number, see -mclist)\n", save);
	return (0);
}

// Blocks of a save, following the "next" links from its first block
static int	McChain(uint8_t *img, int first, int *blocks)
{
	int	n = 0, b = first;

	while ((b >= 1) && (b <= 15) && (n < 15))
	{
		uint8_t		*f = McFrame(img, b);
		uint16_t	next = f[8] | (f[9] << 8);

		blocks[n++] = b;
		if (next == 0xFFFF)
			break;
		b = next + 1;
	}
	return (n);
}

static void	McList(uint8_t *img, char *icon_dir)
{
	int		i, j, n, blocks[15], used = 0, saves = 0;
	char	title[96], name[24], png[4096];

	if (memcmp(img, "MC", 2))
	{
		printf("Not a formatted memory card image (no \"MC\" header).\n");
		return;
	}
	for (i = 1; i < 16; i++)
	{
		uint8_t	*f = McFrame(img, i), *save = img + i * MC_BLOCK;

		used += ((f[0] & 0xF0) == 0x50);
		if (f[0] != MC_STATE_FIRST)
			continue;
		saves++;
		n = McChain(img, i, blocks);
		memcpy(name, f + 10, 20);
		name[20] = 0;
		McTitle(save + 4, title, sizeof(title));
		printf("  block %2d: %-20s %d block(s)  \"%s\"%s\n", i, name, n, title, memcmp(save, "SC", 2) ? "  (no SC title frame)" : "");
		printf("            blocks");
		for (j = 0; j < n; j++)
			printf(" %d", blocks[j]);
		printf(", icon %d frame(s)\n", save[2] & 3);

		// Icon: first frame, 16x16 4-bit, CLUT of 16 colors (15-bit, 0 = transparent) in the title frame
		if (icon_dir && !memcmp(save, "SC", 2))
		{
			uint8_t	rgba[16 * 16 * 4];

			for (j = 0; j < 256; j++)
			{
				uint8_t		p = (save[MC_FRAME + j / 2] >> ((j & 1) * 4)) & 15;
				uint16_t	c = save[0x60 + p * 2] | (save[0x61 + p * 2] << 8);

				rgba[j * 4] = (c & 31) << 3;
				rgba[j * 4 + 1] = ((c >> 5) & 31) << 3;
				rgba[j * 4 + 2] = ((c >> 10) & 31) << 3;
				rgba[j * 4 + 3] = c ? 255 : 0;
			}
			snprintf(png, sizeof(png), "%s/block%02d_%s.png", icon_dir, i, name);
			if (!lodepng_encode32_file(png, rgba, 16, 16))
				printf("            icon: %s\n", png);
		}
	}
	printf("%d save(s), %d/15 blocks used\n", saves, used);
}

// .mcs single save: directory frame of the first block (128 bytes) followed by the save blocks
static uint8_t	*McExport(uint8_t *img, int first, int *size)
{
	int		blocks[15], n = McChain(img, first, blocks), i;
	uint8_t	*mcs = malloc(MC_FRAME + n * MC_BLOCK);

	memcpy(mcs, McFrame(img, first), MC_FRAME);
	for (i = 0; i < n; i++)
		memcpy(mcs + MC_FRAME + i * MC_BLOCK, img + blocks[i] * MC_BLOCK, MC_BLOCK);
	*size = MC_FRAME + n * MC_BLOCK;
	return (mcs);
}

static bool	McImport(uint8_t *img, uint8_t *mcs, int size)
{
	int		n = (size - MC_FRAME) / MC_BLOCK, blocks[15], i, free_count = 0;
	char	name[24];

	if ((size < MC_FRAME + MC_BLOCK) || ((size - MC_FRAME) % MC_BLOCK) || (mcs[0] != MC_STATE_FIRST))
	{
		printf("Not a .mcs save file (128 bytes directory frame + 8KB blocks)\n");
		return (false);
	}
	memcpy(name, mcs + 10, 20);
	name[20] = 0;
	for (i = 1; i < 16; i++)
		if ((McFrame(img, i)[0] == MC_STATE_FIRST) && !strncmp((char *)McFrame(img, i) + 10, name, 20))
		{
			printf("The card already has a save '%s' (block %d), delete it first with -mcdelete\n", name, i);
			return (false);
		}
	// Free blocks, then deleted ones (a deleted save can still be undeleted as long as its blocks are not reused)
	for (i = 1; (i < 16) && (free_count < n); i++)
		if (McFrame(img, i)[0] == MC_STATE_FREE)
			blocks[free_count++] = i;
	for (i = 1; (i < 16) && (free_count < n); i++)
		if (((McFrame(img, i)[0] & 0xF0) != 0x50) && (McFrame(img, i)[0] != MC_STATE_FREE))
			blocks[free_count++] = i;
	if (free_count < n)
	{
		printf("Not enough free blocks: %d needed\n", n);
		return (false);
	}
	for (i = 0; i < n; i++)
	{
		memcpy(img + blocks[i] * MC_BLOCK, mcs + MC_FRAME + i * MC_BLOCK, MC_BLOCK);
		McSetEntry(img, blocks[i], (i == 0) ? MC_STATE_FIRST : ((i == n - 1) ? MC_STATE_LAST : MC_STATE_MIDDLE),
			(i == 0) ? n * MC_BLOCK : 0, (i == n - 1) ? 0xFFFF : blocks[i + 1] - 1, name);
	}
	printf("Save '%s' imported in block(s)", name);
	for (i = 0; i < n; i++)
		printf(" %d", blocks[i]);
	printf("\n");
	return (true);
}

// Deleted like the PS1 does: states 0xA1 (first), 0xA2 (middle), 0xA3 (last), data and links kept
static void	McDelete(uint8_t *img, int first)
{
	int	blocks[15], n = McChain(img, first, blocks), i;

	for (i = 0; i < n; i++)
	{
		McFrame(img, blocks[i])[0] = 0xA0 | (McFrame(img, blocks[i])[0] & 0x0F);
		McChecksum(McFrame(img, blocks[i]));
	}
	printf("Save in block %d deleted (%d block(s) freed)\n", first, n);
}

// DexDrive .gme images loaded: their header, written back with the image (the file stays a .gme: it was saved as a
// raw image, its header lost, after an import, a delete or a copy)
static struct
{
	char	name[1024];
	uint8_t	head[CARD_GME_HEADER];
}	mc_gme[2];
static int	mc_gme_next;

static uint8_t	*McLoadImage(char *filename)
{
	uint8_t	*file, *img;
	int		size, i;

	if (!(file = SystemGetFile(filename, &size)))
		return (NULL);
	if ((size != CARD_SIZE) && (size != CARD_SIZE + CARD_GME_HEADER))
	{
		printf("'%s' is not a memory card image (%d bytes, 131072 expected)\n", filename, size);
		free(file);
		return (NULL);
	}
	img = malloc(CARD_SIZE);
	memcpy(img, file + size - CARD_SIZE, CARD_SIZE);	// DexDrive .gme: header kept aside
	for (i = 0; i < 2; i++)
		if (!strcmp(mc_gme[i].name, filename))
			mc_gme[i].name[0] = 0;
	if (size == CARD_SIZE + CARD_GME_HEADER)
	{
		i = mc_gme_next++ & 1;
		snprintf(mc_gme[i].name, sizeof(mc_gme[i].name), "%s", filename);
		memcpy(mc_gme[i].head, file, CARD_GME_HEADER);
	}
	free(file);
	return (img);
}

static bool	McSaveFile(char *filename, uint8_t *data, int size, bool backup)
{
	char	bak[4200];
	FILE	*f;

	if (backup && !access(filename, 0))
	{
		snprintf(bak, sizeof(bak), "%s.bak", filename);
		remove(bak);
		rename(filename, bak);
		printf("Previous image kept as %s\n", bak);
	}
	if (!(f = fopen(filename, "wb")))
	{
		printf("Can't write '%s'\n", filename);
		return (false);
	}
	fwrite(data, 1, size, f);
	fclose(f);
	return (true);
}

// A card image written back (a .gme with its header)
static bool	McSaveImage(char *filename, uint8_t *img, bool backup)
{
	uint8_t	*buf;
	bool	ok;
	int		i;

	for (i = 0; i < 2; i++)
		if (mc_gme[i].name[0] && !strcmp(mc_gme[i].name, filename))
		{
			buf = malloc(CARD_GME_HEADER + CARD_SIZE);
			memcpy(buf, mc_gme[i].head, CARD_GME_HEADER);
			memcpy(buf + CARD_GME_HEADER, img, CARD_SIZE);
			ok = McSaveFile(filename, buf, CARD_GME_HEADER + CARD_SIZE, backup);
			free(buf);
			return (ok);
		}
	return (McSaveFile(filename, img, CARD_SIZE, backup));
}

// -mclist, -mcexport, -mcimport, -mcdelete, -mccopy, -mcformat: returns the exit code
int	CardImageCommand(int argc, char **argv, int i)
{
	char	*cmd = argv[i], *icons = NULL;
	uint8_t	*img = NULL, *img2 = NULL, *mcs = NULL;
	int		first, size, ret = EXIT_FAILURE, need = (!strcmp(cmd, "-mclist") || !strcmp(cmd, "-mcformat")) ? 1 :
				((!strcmp(cmd, "-mccopy") || !strcmp(cmd, "-mcexport")) ? 3 : 2);

	if (i + need >= argc)
	{
		printf("%s: missing parameters, see the usage\n", cmd);
		return (ret);
	}
	if (!strcmp(cmd, "-mcformat"))
	{
		img = malloc(CARD_SIZE);
		McFormat(img);
		if (McSaveFile(argv[i + 1], img, CARD_SIZE, true))
		{
			printf("Blank formatted memory card image written: %s\n", argv[i + 1]);
			ret = EXIT_SUCCESS;
		}
		goto out;
	}
	if (!(img = McLoadImage(argv[i + 1])))
		goto out;

	if (!strcmp(cmd, "-mclist"))
	{
		if ((i + 3 < argc) && !strcmp(argv[i + 2], "-icons"))
			icons = argv[i + 3];
		McList(img, icons);
		ret = EXIT_SUCCESS;
	}
	else if (!strcmp(cmd, "-mcexport"))
	{
		if ((first = McFind(img, argv[i + 2])) && (mcs = McExport(img, first, &size)) && McSaveFile(argv[i + 3], mcs, size, false))
		{
			printf("Save exported: %s (%d bytes)\n", argv[i + 3], size);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcimport"))
	{
		if ((mcs = SystemGetFile(argv[i + 2], &size)) && McImport(img, mcs, size) && McSaveImage(argv[i + 1], img, true))
		{
			McList(img, NULL);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcdelete"))
	{
		if ((first = McFind(img, argv[i + 2])))
		{
			McDelete(img, first);
			if (McSaveImage(argv[i + 1], img, true))
				ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mccopy"))	// src save dst
	{
		if ((first = McFind(img, argv[i + 2])) && (mcs = McExport(img, first, &size)) && (img2 = McLoadImage(argv[i + 3])) &&
			McImport(img2, mcs, size) && McSaveImage(argv[i + 3], img2, true))
		{
			McList(img2, NULL);
			ret = EXIT_SUCCESS;
		}
	}

out:
	free(img);
	free(img2);
	free(mcs);
	return (ret);
}

static const char	*CardSlotName(int slot);

bool	UploadCardData(int mode, int slot, char *filename)
{
	uint8_t		*block, *file = NULL;
	uint32_t	*header, size = CARD_IMAGE_OFFSET;
	char		*name, *sep;
	int			fsize;
	bool		ret = false;

	if (!(block = calloc(1, CARD_IMAGE_OFFSET + CARD_SIZE)))
		return (false);
	header = (uint32_t *)block;
	header[0] = CARD_MAGIC;
	header[1] = mode;
	header[2] = slot;
	// Backup file name without path (the file server runs in its directory)
	name = filename;
	if ((sep = strrchr(name, '/')))
		name = sep + 1;
	if ((sep = strrchr(name, '\\')))
		name = sep + 1;
	strncpy((char *)&header[3], name, 63);

	if (mode == CARD_RESTORE)
	{
		if (!(file = SystemGetFile(filename, &fsize)))
			goto out;
		if (fsize == CARD_SIZE + CARD_GME_HEADER)	// DexDrive .gme
			memcpy(block + CARD_IMAGE_OFFSET, file + CARD_GME_HEADER, CARD_SIZE);
		else if (fsize == CARD_SIZE)
			memcpy(block + CARD_IMAGE_OFFSET, file, CARD_SIZE);
		else
		{
			printf("'%s' is not a memory card image (%d bytes, 131072 expected) !\n", filename, fsize);
			goto out;
		}
		PrintCardDirectory(block + CARD_IMAGE_OFFSET);
		size += CARD_SIZE;
		printf("Confirm on the PS1: Cross to write the card in slot %s, Circle to cancel.\n", CardSlotName(slot));
	}

	ret = USB_Process('U', block, CARD_DATA_ADRS, size);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(file);
	free(block);
	return (ret);
}

bool	UploadSoundData(char **files, int count)
{
	static const char	*type_names[] = {"?", "VH", "VB", "VAB", "SEQ", "VAG", "SQ", "HD", "BD"};
	uint8_t				*block, *file;
	uint32_t			*header, offset = TESTER_HEADER_SIZE;
	int					i, size, type;
	bool				ret = false;

	if (!(block = calloc(1, TESTER_DATA_END - TESTER_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = TESTER_MAGIC;
	header[1] = count;

	for (i = 0; i < count; i++)
	{
		if (!(file = SystemGetFile(files[i], &size)))
			goto out;
		if (!(type = SoundFileType(files[i], file, size)))
		{
			printf("'%s' is not a VH, VB, VAB, SEQ, VAG, SQ, HD or BD file !\n", files[i]);
			free(file);
			goto out;
		}
		if (offset + size > TESTER_DATA_END - TESTER_DATA_ADRS)
		{
			printf("Sound files are too big (max %d bytes) !\n", TESTER_DATA_END - TESTER_DATA_ADRS - TESTER_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		header[2 + i * 3] = type;
		header[2 + i * 3 + 1] = TESTER_DATA_ADRS + offset;
		header[2 + i * 3 + 2] = size;
		printf("%s: %s (%d bytes)\n", type_names[type], files[i], size);
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading %d sound file(s), %u bytes to 0x%X...\n", count, offset, TESTER_DATA_ADRS);
	ret = USB_Process('U', block, TESTER_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

// Data block: header (magic, TMD address, number of TIM, TIM addresses; at VIEWER_TOD_TABLE the TOD table) then the
// TMD, TIM and TOD files
bool	UploadModelData(char *model, char **tims, int tim_count, char **tods, int tod_count)
{
	uint8_t		*block, *file;
	uint32_t	*header, offset = VIEWER_HEADER_SIZE, *tod_table;
	int			i, size;
	bool		ret = false;

	if (!(block = calloc(1, VIEWER_DATA_END - VIEWER_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = VIEWER_MAGIC;
	header[2] = tim_count;
	tod_table = (uint32_t *)(block + VIEWER_TOD_TABLE);
	tod_table[0] = tod_count;

	for (i = -1; i < tim_count + tod_count; i++)
	{
		char		*name = (i < 0) ? model : ((i < tim_count) ? tims[i] : tods[i - tim_count]);
		uint32_t	id = (i < 0) ? 0x41 : ((i < tim_count) ? 0x10 : 0x50);

		if (!(file = SystemGetFile(name, &size)))
			goto out;
		if ((size < 8) || ((*((uint32_t *)file) & ((id == 0x50) ? 0xFF : 0xFFFFFFFF)) != id))
		{
			printf("'%s' is not a %s file !\n", name, (i < 0) ? "TMD" : ((i < tim_count) ? "TIM" : "TOD"));
			free(file);
			goto out;
		}
		if (offset + size > VIEWER_DATA_END - VIEWER_DATA_ADRS)
		{
			printf("Model and textures are too big (max %d bytes) !\n", VIEWER_DATA_END - VIEWER_DATA_ADRS - VIEWER_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		if (i < 0)
			header[1] = VIEWER_DATA_ADRS + offset;
		else if (i < tim_count)
			header[3 + i] = VIEWER_DATA_ADRS + offset;
		else
		{
			// TOD: address, size, name (file name without its folder, shown by the viewer)
			uint32_t	*entry = tod_table + 1 + (i - tim_count) * 8;
			char		*base = name + strlen(name);

			while ((base > name) && (base[-1] != '/') && (base[-1] != '\\'))
				base--;
			entry[0] = VIEWER_DATA_ADRS + offset;
			entry[1] = size;
			strncpy((char *)(entry + 2), base, 23);
			printf("TOD: %s (%d bytes, %d frames)\n", name, size, (int)((uint32_t *)file)[1]);
		}
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading model, %d texture(s) and %d animation(s), %u bytes to 0x%X...\n", tim_count, tod_count, offset,
		VIEWER_DATA_ADRS);
	ret = USB_Process('U', block, VIEWER_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

/****************************************/
// Memory card commands on the card itself. "slot1" or "slot2" in place of the image file of -mclist,
// -mcexport, -mcimport, -mcdelete and -mccopy. The card tool runs in server mode (ps1-tools/cardtool/cardserver.h) and only
// the sectors needed are read or written: the directory, the title/icon frames of the saves, the blocks of a save.
// The image functions above work on a partial image of the card in memory; the sectors they change are written.

// A memory card slot: "1", "2", or with a multitap's slot "1a"-"1d", "2a"-"2d" ("slot" before it for -mc...):
// CARD_SLOT (see cardserver.h), 0 if it isn't one
int	CardSlotParse(const char *arg, bool slot_word)
{
	int	port, tap = 0;

	if (slot_word)
	{
		if (strncmp(arg, "slot", 4))
			return (0);
		arg += 4;
	}
	if ((arg[0] != '1') && (arg[0] != '2'))
		return (0);
	port = arg[0] - '0';
	if (arg[1])
	{
		if ((tolower((unsigned char)arg[1]) < 'a') || (tolower((unsigned char)arg[1]) > 'd') || arg[2])
			return (0);
		tap = tolower((unsigned char)arg[1]) - 'a';
	}
	return (CARD_SLOT(port, tap));
}

int	CardSlotArg(char *arg)
{
	return (CardSlotParse(arg, true));
}

// "1", "2", "1B"...: a slot as shown
static const char	*CardSlotName(int slot)
{
	static char	name[4];

	name[0] = '0' + CARD_SLOT_PORT(slot);
	name[1] = CARD_SLOT_TAP(slot) ? 'A' + CARD_SLOT_TAP(slot) : 0;
	name[2] = 0;
	return (name);
}

bool	CardServerRunning(void)
{
	uint32_t	magic = 0;

	return (USB_Process('D', (uint8_t *)&magic, CS_FIELD(magic), 4) && (magic == CARDSRV_MAGIC));
}

// Uploads and executes the card tool in server mode (the PS1 is at the menu)
static bool	CardServerStart(char *argv0)
{
	uint8_t		*exe, header[CARD_IMAGE_OFFSET];
	uint32_t	zero = 0;
	int			size, i;
	bool		ok;

	if (!(exe = SystemGetFile(GetToolPath(argv0, CARD_FILENAME), &size)))
		return (false);
	memset(header, 0, sizeof(header));
	((uint32_t *)header)[0] = CARD_MAGIC;
	((uint32_t *)header)[1] = CARD_MODE_SERVER;
	printf("Starting the memory card tool...\n");
	// The magic of a previous run may still be in RAM: cleared, the card tool sets it once ready
	ok = USB_Process('U', (uint8_t *)&zero, CS_FIELD(magic), 4) && USB_Process('U', header, CARD_DATA_ADRS, sizeof(header)) &&
		USB_Process('U', exe + 2048, *(uint32_t *)&exe[0x18], size - 2048) &&
		USB_Process('E', exe + 16, EXEC_TTY_MODE_BIOS, 15 * 4);
	free(exe);
	if (!ok)
		return (false);
	ps1_program_running = true;
	usb_big_packet_size = 0;
	for (i = 0; i < 50; i++)
	{
		SleepMs(100);
		if (CardServerRunning())
			return (true);
	}
	printf("The memory card tool didn't start.\n");
	return (false);
}

// Runs a read or write of the sectors in the list (already in the image buffer for a write)
static bool	CardServerCommand(int cmd, int slot, uint16_t *sectors, int count, const char *text)
{
	struct
	{
		uint32_t	cmd, slot, count;
		char		text[CARDSRV_TEXT];
	}			fields;
	uint32_t	seq = 0, done = 0, error_sector = 0;
	int32_t		result = 0;
	time_t		start;

	memset(&fields, 0, sizeof(fields));
	fields.cmd = cmd;
	fields.slot = slot;
	fields.count = count;
	if (text)
		strncpy(fields.text, text, CARDSRV_TEXT - 1);
	if (!USB_Process('D', (uint8_t *)&seq, CS_FIELD(done_seq), 4) ||
		!USB_Process('U', (uint8_t *)&fields, CS_FIELD(cmd), sizeof(fields)) ||
		!USB_Process('U', (uint8_t *)sectors, CS_FIELD(sectors), (count * 2 + 3) & ~3))
		goto usb_error;
	seq++;
	if (!USB_Process('U', (uint8_t *)&seq, CS_FIELD(cmd_seq), 4))
		goto usb_error;
	if (cmd == CARDSRV_WRITE)
		printf("Confirm on the PS1: Cross to write %d sector(s) of the card in slot %s, Circle to cancel.\n", count,
			CardSlotName(slot));
	for (start = time(NULL); done != seq; SleepMs(20))
	{
		if (!USB_Process('D', (uint8_t *)&done, CS_FIELD(done_seq), 4))
			goto usb_error;
		if (time(NULL) - start > ((cmd == CARDSRV_WRITE) ? 600 : 60))
		{
			printf("The memory card tool doesn't answer.\n");
			return (false);
		}
	}
	if (!USB_Process('D', (uint8_t *)&result, CS_FIELD(result), 4) ||
		!USB_Process('D', (uint8_t *)&error_sector, CS_FIELD(error_sector), 4))
		goto usb_error;
	switch (result)
	{
		case CARDSRV_OK:			return (true);
		case CARDSRV_NO_CARD:		printf("No memory card in slot %s.\n", CardSlotName(slot)); break;
		case CARDSRV_IO_ERROR:		printf("Memory card error %s sector %u.\n", (cmd == CARDSRV_WRITE) ? "writing" : "reading",
										error_sector); break;
		case CARDSRV_CANCELLED:		printf("Cancelled on the PS1, the card was not modified.\n"); break;
		default:					printf("The memory card tool refused the command (%d).\n", result);
	}
	return (false);

usb_error:
	printf("USB error.\n");
	return (false);
}

// Reads the sectors first..first+count-1 not read yet
static bool	LiveRead(LiveCard *c, int first, int count)
{
	uint16_t	list[CARDSRV_SECTORS];
	int			n = 0, i, run;

	for (i = first; i < first + count; i++)
		if (!c->have[i])
			list[n++] = i;
	if (!n)
		return (true);
	if (!CardServerCommand(CARDSRV_READ, c->slot, list, n, NULL))
		return (false);
	for (i = 0; i < n; i += run)	// Runs of consecutive sectors from the image buffer
	{
		for (run = 1; (i + run < n) && (list[i + run] == list[i] + run); run++)
			;
		if (!USB_Process('D', c->img + list[i] * CARD_SECTOR, CARDSRV_IMAGE_ADRS + list[i] * CARD_SECTOR, run * CARD_SECTOR))
		{
			printf("USB error.\n");
			return (false);
		}
	}
	for (i = 0; i < n; i++)
	{
		memcpy(c->orig + list[i] * CARD_SECTOR, c->img + list[i] * CARD_SECTOR, CARD_SECTOR);
		c->have[list[i]] = true;
	}
	return (true);
}

// Header and directory frames (sectors 0-15)
static bool	LiveDirectory(LiveCard *c)
{
	if (!LiveRead(c, 0, 16))
		return (false);
	if (memcmp(c->img, "MC", 2))
	{
		printf("The card in slot %s is not formatted (no \"MC\" header).\n", CardSlotName(c->slot));
		return (false);
	}
	return (true);
}

// Writes what the image functions changed: the directory frames, and the whole blocks of the saves written
static bool	LiveWriteChanges(LiveCard *c, const char *text)
{
	uint16_t	list[CARDSRV_SECTORS];
	int			n = 0, s, b, i, run;

	for (s = 0; s < 16; s++)
	{
		if (!memcmp(c->img + s * CARD_SECTOR, c->orig + s * CARD_SECTOR, CARD_SECTOR))
			continue;
		list[n++] = s;
		b = s;		// Directory frame s = block s
		if ((b >= 1) && (c->img[b * CARD_SECTOR] >= MC_STATE_FIRST) && (c->img[b * CARD_SECTOR] <= MC_STATE_LAST))
			for (i = 0; i < CARD_BLOCK_SECTORS; i++)
				list[n++] = b * CARD_BLOCK_SECTORS + i;
	}
	if (!n)
	{
		printf("Nothing to write.\n");
		return (true);
	}
	for (i = 0; i < n; i += run)
	{
		for (run = 1; (i + run < n) && (list[i + run] == list[i] + run); run++)
			;
		if (!USB_Process('U', c->img + list[i] * CARD_SECTOR, CARDSRV_IMAGE_ADRS + list[i] * CARD_SECTOR, run * CARD_SECTOR))
		{
			printf("USB error.\n");
			return (false);
		}
	}
	return (CardServerCommand(CARDSRV_WRITE, c->slot, list, n, text));
}

// Whole blocks of a save
static bool	LiveSaveBlocks(LiveCard *c, int first)
{
	int	blocks[15], n = McChain(c->img, first, blocks), i;

	for (i = 0; i < n; i++)
		if (!LiveRead(c, blocks[i] * CARD_BLOCK_SECTORS, CARD_BLOCK_SECTORS))
			return (false);
	return (true);
}

// A save as .mcs data from a card slot or an image file
static uint8_t	*SaveFrom(char *source, char *save, int *size)
{
	LiveCard	*c;
	uint8_t		*img, *mcs = NULL;
	int			first;

	if (CardSlotArg(source))
	{
		c = calloc(1, sizeof(LiveCard));
		c->slot = CardSlotArg(source);
		if (LiveDirectory(c) && (first = McFind(c->img, save)) && LiveSaveBlocks(c, first))
			mcs = McExport(c->img, first, size);
		free(c);
	}
	else if ((img = McLoadImage(source)))
	{
		if ((first = McFind(img, save)))
			mcs = McExport(img, first, size);
		free(img);
	}
	return (mcs);
}

static bool	SaveName(uint8_t *mcs, char *name)
{
	memcpy(name, mcs + 10, 20);
	name[20] = 0;
	return (true);
}

// -mclist/-mcexport/-mcimport/-mcdelete/-mccopy with slot1/slot2: returns the exit code
int	CardLiveCommand(int argc, char **argv, int i, char *argv0, bool start)
{
	char		*cmd = argv[i], *icons = NULL, text[CARDSRV_TEXT], name[24];
	LiveCard	*c = calloc(1, sizeof(LiveCard));
	uint8_t		*mcs = NULL, *img = NULL;
	int			first, size, b, ret = EXIT_FAILURE, need = !strcmp(cmd, "-mclist") ? 1 :
					((!strcmp(cmd, "-mccopy") || !strcmp(cmd, "-mcexport")) ? 3 : 2);

	setvbuf(stdout, NULL, _IONBF, 0);
	if (i + need >= argc)
	{
		printf("%s: missing parameters, see the usage\n", cmd);
		goto out;
	}
	if (!strcmp(cmd, "-mcformat"))
	{
		printf("-mcformat only makes images (format a card with the BIOS memory card manager).\n");
		goto out;
	}
	if (start && !CardServerStart(argv0))
		goto out;
	ps1_program_running = true;
	usb_big_packet_size = 0;
	c->slot = CardSlotArg(argv[i + 1]);

	if (!strcmp(cmd, "-mclist"))
	{
		if ((i + 3 < argc) && !strcmp(argv[i + 2], "-icons"))
			icons = argv[i + 3];
		if (!LiveDirectory(c))
			goto out;
		for (b = 1; b < 16; b++)	// Title frame and icon frames of each save
			if ((McFrame(c->img, b)[0] == MC_STATE_FIRST) && !LiveRead(c, b * CARD_BLOCK_SECTORS, 4))
				goto out;
		printf("Memory card in slot %s:\n", CardSlotName(c->slot));
		McList(c->img, icons);
		ret = EXIT_SUCCESS;
	}
	else if (!strcmp(cmd, "-mcexport"))
	{
		if ((mcs = SaveFrom(argv[i + 1], argv[i + 2], &size)) && McSaveFile(argv[i + 3], mcs, size, false))
		{
			printf("Save exported: %s (%d bytes)\n", argv[i + 3], size);
			ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mcimport"))
	{
		if ((mcs = SystemGetFile(argv[i + 2], &size)) && LiveDirectory(c) && McImport(c->img, mcs, size) &&
			SaveName(mcs, name))
		{
			snprintf(text, sizeof(text), "IMPORT %s", name);
			if (LiveWriteChanges(c, text))
			{
				printf("Save written to the card in slot %s.\n", CardSlotName(c->slot));
				ret = EXIT_SUCCESS;
			}
		}
	}
	else if (!strcmp(cmd, "-mcdelete"))
	{
		if (LiveDirectory(c) && (first = McFind(c->img, argv[i + 2])))
		{
			memcpy(name, McFrame(c->img, first) + 10, 20);
			name[20] = 0;
			McDelete(c->img, first);
			snprintf(text, sizeof(text), "DELETE %s", name);
			if (LiveWriteChanges(c, text))
				ret = EXIT_SUCCESS;
		}
	}
	else if (!strcmp(cmd, "-mccopy"))	// src save dst: each one a card slot or an image file
	{
		if (!(mcs = SaveFrom(argv[i + 1], argv[i + 2], &size)))
			goto out;
		if (CardSlotArg(argv[i + 3]))
		{
			c->slot = CardSlotArg(argv[i + 3]);
			memset(c->have, 0, sizeof(c->have));
			SaveName(mcs, name);
			snprintf(text, sizeof(text), "IMPORT %s", name);
			if (LiveDirectory(c) && McImport(c->img, mcs, size) && LiveWriteChanges(c, text))
				ret = EXIT_SUCCESS;
		}
		else if ((img = McLoadImage(argv[i + 3])) && McImport(img, mcs, size) && McSaveImage(argv[i + 3], img, true))
		{
			McList(img, NULL);
			ret = EXIT_SUCCESS;
		}
	}
	else
		printf("%s doesn't work on a card slot.\n", cmd);

out:
	free(c);
	free(mcs);
	free(img);
	return (ret);
}

