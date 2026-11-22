/*
* PS1 DEV CART Transfer tool: PS1 tools run by ps1transfer: TMD and TIM viewers, sound tester, snapshot (from main.c)
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
// TMD model viewer: tmdviewer.ps-exe (built from the tmdviewer folder) is executed with a data block holding
// the model and its textures, see ps1-tools/tmdviewer/tmdviewer.c

char	*GetToolPath(char *argv0, char *name)
{
	static char	path[4096];
	char		*sep, *sep2;
	int			len;

#ifdef _WIN32
	len = GetModuleFileNameA(NULL, path, sizeof(path) - TOOL_NAME_MAX);
#else
	len = readlink("/proc/self/exe", path, sizeof(path) - TOOL_NAME_MAX - 1);
#endif
	if (len <= 0)
	{
		strncpy(path, argv0, sizeof(path) - TOOL_NAME_MAX - 1);
		len = strlen(path);
	}
	path[len] = 0;

	sep = strrchr(path, '/');
	sep2 = strrchr(path, '\\');
	if (sep2 > sep)
		sep = sep2;
	strncpy(sep ? sep + 1 : path, name, TOOL_NAME_MAX - 1);
	return (path);
}

/****************************************/
// -snapshot DIR: what the running program has loaded, for the PS1 Dev Cart Manager's Game Data page (asset scanner):
// ram.bin (the 2 MB of main RAM, read by the kernel hook while the program runs), vram.bin (1024x512, 16-bit pixels,
// read from the GPU) with vram.png, spu.bin (the 512KB of SPU RAM and the 512 bytes of SPU registers, as "-spu",
// debug monitor version 10). Works with a game booted from its disc too. Lines "SNAPSHOT PART FILE" (PART: ram, vram,
// spu), "SNAPSHOT done DIR" at the end; a part that can't be read is skipped ("SNAPSHOT skipped PART why").

static bool	SnapshotWrite(const char *dir, const char *name, const uint8_t *data, uint32_t size)
{
	char	path[1024];
	FILE	*f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "wb")) || (fwrite(data, 1, size, f) != size) || fclose(f))
	{
		printf("Can't write %s\n", path);
		return (false);
	}
	return (true);
}

// The SPU RAM read by DMA (debug monitor) has glitches: now and then a halfword reads wrong, mostly as 0xFFxx (about
// 80 halfwords in 512KB, not the same ones twice). Three reads merged: where they differ, a halfword whose high byte isn't
// 0xFF, the one most reads agree on (a whole sound bank compared with its file: 1 byte left in 480KB).
static void	SpuMerge(uint16_t *a, const uint16_t *b, const uint16_t *c, uint32_t count)
{
	uint32_t	i;
	int			k, j, best, votes, n;

	for (i = 0; i < count; i++)
	{
		uint16_t	v[3] = {a[i], b[i], c[i]};
		bool		good[3];

		if ((v[0] == v[1]) && (v[0] == v[2]))
			continue;
		for (k = n = 0; k < 3; k++)
			n += (good[k] = ((v[k] >> 8) != 0xFF));
		for (best = -1, votes = 0, k = 0; k < 3; k++)
		{
			int	same = 0;

			if (n && !good[k])
				continue;
			for (j = 0; j < 3; j++)
				same += (v[j] == v[k]) && (!n || good[j]);
			if (same > votes)
			{
				votes = same;
				best = k;
			}
		}
		a[i] = v[best];
	}
}

// A part named in PARTS (comma separated: ram, vram, spu; NULL: all of them)
static bool	SnapshotPart(const char *parts, const char *name)
{
	size_t		n = strlen(name);
	const char	*p = parts;

	if (!parts)
		return (true);
	while (p && *p)
	{
		if (!strncmp(p, name, n) && ((p[n] == ',') || !p[n]))
			return (true);
		if ((p = strchr(p, ',')))
			p++;
	}
	return (false);
}

bool	Snapshot(const char *dir, const char *parts)
{
	bool	want_ram = SnapshotPart(parts, "ram"), want_vram = SnapshotPart(parts, "vram");
	bool	want_spu = SnapshotPart(parts, "spu");

	MonitorState	ms;
	uint8_t			*ram = malloc(0x200000), *vram = malloc(1024 * 512 * (sizeof(uint16_t) + 3));
	uint8_t			*spu = malloc(MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE), *spu2 = malloc(MON_SPU_RAM_SIZE * 2);
	char			png[1024];
	int				r;
	bool			monitor, stopped, ok = false, got;

	if (!ram || !vram || !spu || !spu2)
		goto out;
	if (!ps1_program_running)
	{
		printf("No program runs: start a program or boot a game first\n");
		goto out;
	}
	MAKE_DIR(dir);
	monitor = USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, 16) && (ms.magic == MONITOR_MAGIC);
	stopped = monitor && (ms.reason != MON_REASON_NONE);

	if (want_ram)
	{
		printf("Reading the RAM (2 MB)...\n");
		if (!USB_Process('D', ram, 0x80000000, 0x200000))
			goto out;
		if (!SnapshotWrite(dir, "ram.bin", ram, 0x200000))
			goto out;
		printf("SNAPSHOT ram ram.bin\n");
	}

	if (want_vram)
		printf("Reading the VRAM (1024x512)...\n");
	got = want_vram && ((monitor && (ms.version >= 2) && !stopped) ? HotService('V', vram, 0, (1024 << 16) | 512) :
		USB_Process('V', vram, 0, (1024 << 16) | 512));
	if (!want_vram)
		;
	else if (got && SnapshotWrite(dir, "vram.bin", vram, 1024 * 512 * 2))
	{
		snprintf(png, sizeof(png), "%s/vram.png", dir);
		VramToPng(png, (uint16_t *)vram, 1024, 512);
		printf("SNAPSHOT vram vram.bin\n");
	}
	else
		printf("SNAPSHOT skipped vram (USB error)\n");

	if (!want_spu)
		;
	else if (!monitor || (ms.version < 10))
		printf("SNAPSHOT skipped spu (the firmware's debug monitor is too old to read the SPU RAM)\n");
	else
	{
		printf("Reading the SPU registers and RAM (512KB, 3 times: see SpuMerge)...\n");
		got = stopped ? USB_Process(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE) :
			HotService(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE);
		for (r = 0; got && (r < 3); r++)
		{
			uint8_t	*dst = r ? spu2 + (r - 1) * MON_SPU_RAM_SIZE : spu;

			got = stopped ? USB_Process(MON_CMD_SPU_READ, dst, 0, MON_SPU_RAM_SIZE) :
				HotService(MON_CMD_SPU_READ, dst, 0, MON_SPU_RAM_SIZE);
		}
		if (got)
			SpuMerge((uint16_t *)spu, (uint16_t *)spu2, (uint16_t *)(spu2 + MON_SPU_RAM_SIZE), MON_SPU_RAM_SIZE / 2);
		if (got && SnapshotWrite(dir, "spu.bin", spu, MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE))
			printf("SNAPSHOT spu spu.bin\n");
		else
			printf("SNAPSHOT skipped spu (USB error)\n");
	}
	printf("SNAPSHOT done %s\n", dir);
	ok = true;
out:
	free(ram);
	free(vram);
	free(spu);
	free(spu2);
	return (ok);
}

/****************************************/
// Sound tester: soundtest.ps-exe (built from the soundtest folder) is executed with a data block holding
// the sound files, see ps1-tools/soundtest/soundtest.c

// Type from the file header ("pBAV" VH/VAB, "pQES"/"SEQp" SEQ or SEP, "VAGp" VAG), VB files have no header: ".vb" extension
int	SoundFileType(char *name, uint8_t *data, int size)
{
	char	*ext = strrchr(name, '.');

	if ((size >= 32) && !memcmp(data, "pBAV", 4))
	{
		uint32_t	ps = data[18] | (data[19] << 8);	// Number of programs
		uint32_t	vh_size = 32 + 128 * 16 + ps * 16 * 32 + 256 * 2;

		return ((size > vh_size) ? SOUND_VAB : SOUND_VH);
	}
	if ((size >= 4) && (!memcmp(data, "pQES", 4) || !memcmp(data, "SEQp", 4)))	// (SEQ or SEP: the same ID either
		return (SOUND_SEQ);															// way round)
	if ((size >= 48) && !memcmp(data, "VAGp", 4))
		return (SOUND_VAG);
	if ((size >= 0x110) && !memcmp(data + 12, "SSsq", 4))
		return (SOUND_SQ);
	if ((size >= 0x80) && !memcmp(data + 12, "SShd", 4))
		return (SOUND_HD);
	if (ext && ((!strcmp(ext, ".vb")) || (!strcmp(ext, ".VB")) || (!strcmp(ext, ".Vb"))))
		return (SOUND_VB);
	if (ext && ((!strcmp(ext, ".bd")) || (!strcmp(ext, ".BD")) || (!strcmp(ext, ".Bd"))))	// (no header either)
		return (SOUND_BD);
	return (0);
}

/****************************************/
// TIM and VRAM viewer: timviewer.ps-exe (built from the timviewer folder) is executed with a data block holding
// the TIM files and their names, see ps1-tools/timviewer/timviewer.c

bool	UploadTimData(char **files, int count)
{
	uint8_t		*block, *file, *entry;
	uint32_t	*header, offset = TIMVIEW_HEADER_SIZE;
	int			i, size;
	char		*name, *sep;
	bool		ret = false;

	if (!(block = calloc(1, TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS)))
		return (false);
	header = (uint32_t *)block;
	header[0] = TIMVIEW_MAGIC;
	header[1] = count;

	for (i = 0; i < count; i++)
	{
		if (!(file = SystemGetFile(files[i], &size)))
			goto out;
		if ((size < 8) || (*((uint32_t *)file) != 0x10))
		{
			printf("'%s' is not a TIM file !\n", files[i]);
			free(file);
			goto out;
		}
		if (offset + size > TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS)
		{
			printf("TIM files are too big (max %d bytes) !\n", TIMVIEW_DATA_END - TIMVIEW_DATA_ADRS - TIMVIEW_HEADER_SIZE);
			free(file);
			goto out;
		}
		memcpy(block + offset, file, size);
		// Entry: address, size, file name without path (displayed by the viewer)
		entry = block + 8 + i * (8 + TIMVIEW_NAME_LEN);
		((uint32_t *)entry)[0] = TIMVIEW_DATA_ADRS + offset;
		((uint32_t *)entry)[1] = size;
		name = files[i];
		if ((sep = strrchr(name, '/')))
			name = sep + 1;
		if ((sep = strrchr(name, '\\')))
			name = sep + 1;
		strncpy((char *)entry + 8, name, TIMVIEW_NAME_LEN - 1);
		offset += (size + 3) & ~3;
		free(file);
	}

	printf("Uploading %d TIM file(s), %u bytes to 0x%X...\n", count, offset, TIMVIEW_DATA_ADRS);
	ret = USB_Process('U', block, TIMVIEW_DATA_ADRS, offset);
	printf(ret ? "\n" : "\nUSB error.\n");

out:
	free(block);
	return (ret);
}

