/*
* PS1 DEV CART Transfer tool: Net Yaroze: runtime library upload, ECOFF executables, auto scripts (from main.c)
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
// Net Yaroze runtime library upload (normally loaded by the Net Yaroze boot disc)

uint32_t	libps_init;		// Address of the library start code, 0: unknown library
uint32_t	libps_adrs, libps_size;	// Where the library was uploaded

// The stub, just below the program (unused by Net Yaroze programs): returns its address, 0 on USB error
uint32_t	UploadNetYarozeStart(uint32_t exe_adrs, uint32_t entry)
{
	uint32_t	stub[LIBPS_STUB_SIZE / 4], adrs = exe_adrs - 32;

	stub[0] = 0x0C000000 | ((libps_init >> 2) & 0x03FFFFFF);	// jal libps_init
	stub[1] = 0;												// nop
	stub[2] = 0x3C080000 | (entry >> 16);						// lui t0, entry >> 16
	stub[3] = 0x35080000 | (entry & 0xFFFF);					// ori t0, t0, entry & 0xFFFF
	stub[4] = 0x01000008;										// jr t0
	stub[5] = 0;												// nop
	printf("Net Yaroze library start code: 0x%X, then the program at 0x%X\n", libps_init, entry);
	return (USB_Process('U', (uint8_t *)stub, adrs, sizeof(stub)) ? adrs : 0);
}

bool	UploadNetYarozeLibrary(char *filename, uint32_t exe_adrs, uint32_t exe_size)
{
	uint32_t	start_ms;
	uint8_t		*data, *ptr;
	uint32_t	adrs = LIBPS_DEFAULT_ADRS;
	int			size;
	bool		ret;

	if (!(data = SystemGetFile(filename, &size)))
		return (false);

	ptr = data;
	if ((size > 2048) && !memcmp(data, "PS-X EXE", 8))
	{
		adrs = *((uint32_t*)&data[8+4+4+(2*4)]);	// Text address from the exe header
		ptr = &data[2048];
		size -= 2048;
	}

	if ((exe_adrs < adrs + size) && (exe_adrs + exe_size > adrs))
	{
		printf("The executable (0x%X-0x%X) overlaps the Net Yaroze library (0x%X-0x%X) !\n",
			exe_adrs, exe_adrs + exe_size, adrs, adrs + size);
		free(data);
		return (false);
	}

	libps_init = 0;
	if ((size > LIBPS_INIT_OFFSET + 4) && (*(uint32_t *)&ptr[LIBPS_INIT_OFFSET] == LIBPS_INIT_CODE))
		libps_init = adrs + LIBPS_INIT_OFFSET;
	else
		printf("Unknown Net Yaroze library version: its start code isn't run (pads may not work).\n");

	libps_adrs = adrs;
	libps_size = size;
	printf("Uploading Net Yaroze library %u bytes to 0x%X...\n", size, adrs);
	start_ms = NowMs();
	ret = USB_Process('U', ptr, adrs, size);
	if (ret)
		PrintUploadTime(size, start_ms);
	else
		printf("\n");
	free(data);

	return (ret);
}

/****************************************/
// Net Yaroze programs as the Net Yaroze tools make them: the ECOFF executable of the compiler ("main") and the
// "auto" script of siocons, which loads it and the data files ("local load main", "local dload file address",
// "go"). A program converted to a PS-X EXE (eco2exe, or yarexe, which also includes the library) runs as usual.

									// the files): data must stay below

AutoData	auto_data[AUTO_MAX_DATA];
int		auto_count;
char		auto_program[1024];

// ECOFF executable to a PS-X EXE in memory (sections with data at their addresses, entry point and gp); NULL: not
// an ECOFF file. The program clears its .bss itself (as with the Net Yaroze boot disc and yarexe).
uint8_t	*EcoffToPsxExe(const uint8_t *data, int size, int *exe_size)
{
	uint16_t	nscns, opthdr;
	uint32_t	lo = 0xFFFFFFFF, hi = 0, image, i;
	const uint8_t	*sec;
	uint8_t		*exe;

	if ((size < 20 + 56) || (*(uint16_t *)data != ECOFF_MAGIC))
		return (NULL);
	nscns = *(uint16_t *)&data[2];
	opthdr = *(uint16_t *)&data[16];
	if ((opthdr < 56) || (20 + opthdr + nscns * 40 > (uint32_t)size))
		return (NULL);
	for (i = 0, sec = data + 20 + opthdr; i < nscns; i++, sec += 40)
	{
		uint32_t	vaddr = *(uint32_t *)&sec[12], ssize = *(uint32_t *)&sec[16], ptr = *(uint32_t *)&sec[20];

		if (!ssize || !ptr || (*(uint32_t *)&sec[36] & ECOFF_STYP_NOLOAD))
			continue;
		if (ptr + ssize > (uint32_t)size)
		{
			printf("ECOFF section %.8s is outside of the file\n", (const char *)sec);
			return (NULL);
		}
		if (vaddr < lo)
			lo = vaddr;
		if (vaddr + ssize > hi)
			hi = vaddr + ssize;
	}
	if (lo >= hi)
		return (NULL);
	image = (hi - lo + 2047) & ~2047;
	if (!(exe = calloc(1, 2048 + image)))
		return (NULL);
	memcpy(exe, "PS-X EXE", 8);
	*(uint32_t *)&exe[0x10] = *(uint32_t *)&data[20 + 16];	// a.out entry
	*(uint32_t *)&exe[0x14] = *(uint32_t *)&data[20 + 52];	// gp value
	*(uint32_t *)&exe[0x18] = lo;
	*(uint32_t *)&exe[0x1C] = image;
	strcpy((char *)&exe[0x4C], "Net Yaroze ECOFF executable (ps1transfer)");
	for (i = 0, sec = data + 20 + opthdr; i < nscns; i++, sec += 40)
	{
		uint32_t	vaddr = *(uint32_t *)&sec[12], ssize = *(uint32_t *)&sec[16], ptr = *(uint32_t *)&sec[20];

		if (ssize && ptr && !(*(uint32_t *)&sec[36] & ECOFF_STYP_NOLOAD))
			memcpy(exe + 2048 + vaddr - lo, data + ptr, ssize);
	}
	printf("Net Yaroze ECOFF executable: 0x%X-0x%X, entry point 0x%X\n", lo, hi, *(uint32_t *)&exe[0x10]);
	*exe_size = 2048 + image;
	return (exe);
}

// A file named in an auto script: relative to the folder of the script ('\' or '/')
static void	AutoPath(char *out, size_t n, const char *script, const char *name)
{
	const char	*slash = strrchr(script, '/'), *bslash = strrchr(script, '\\');
	size_t		dir;

	if (bslash > slash)
		slash = bslash;
	dir = slash ? (size_t)(slash - script + 1) : 0;
	if ((name[0] == '/') || (name[0] == '\\') || (name[0] && (name[1] == ':')) || (dir >= n))
		dir = 0;
	snprintf(out, n, "%.*s%s", (int)dir, script, name);
}

// Reads an auto script (and the scripts it calls with "auto"): the program and the data files with their addresses
bool	ReadAutoScript(const char *script, int depth)
{
	FILE	*f;
	char	line[1024], *w[4], *p;
	int		n, number = 0;

	if (depth >= AUTO_MAX_DEPTH)
	{
		printf("%s: too many nested auto scripts\n", script);
		return (false);
	}
	if (!(f = fopen(script, "r")))
	{
		printf("Auto script '%s' not found !\n", script);
		return (false);
	}
	while (fgets(line, sizeof(line), f))
	{
		number++;
		for (n = 0, p = strtok(line, " \t\r\n"); p && (n < 4); p = strtok(NULL, " \t\r\n"))
			w[n++] = p;
		if (n && !strcmp(w[0], "local"))	// The "local" commands of siocons (and the same without it)
		{
			memmove(w, w + 1, sizeof(char *) * 3);
			n--;
		}
		if (!n || (w[0][0] == '#') || (w[0][0] == ';'))
			continue;
		if (!strcmp(w[0], "load") && (n >= 2))
		{
			if (auto_program[0])
				printf("%s line %d: a second program, only the last one is run\n", script, number);
			AutoPath(auto_program, sizeof(auto_program), script, w[1]);
		}
		else if (!strcmp(w[0], "dload") && (n >= 3))
		{
			AutoData	*d = &auto_data[auto_count];

			if (auto_count >= AUTO_MAX_DATA)
			{
				printf("%s: more than %d data files\n", script, AUTO_MAX_DATA);
				fclose(f);
				return (false);
			}
			AutoPath(d->path, sizeof(d->path), script, w[1]);
			d->adrs = strtoul(w[2], NULL, 16);
			if (d->adrs < 0x80000000)
				d->adrs |= 0x80000000;
			auto_count++;
		}
		else if (!strcmp(w[0], "auto") && (n >= 2))
		{
			char	sub[1024];

			AutoPath(sub, sizeof(sub), script, w[1]);
			if (!ReadAutoScript(sub, depth + 1))
			{
				fclose(f);
				return (false);
			}
		}
		else if (strcmp(w[0], "go"))
			printf("%s line %d: '%s' skipped (only load, dload, auto and go are used)\n", script, number, w[0]);
	}
	fclose(f);
	return (true);
}

// Data files of the auto script, after the library: each one must fit below the firmware, and not overlap the
// library, the program or another file
bool	UploadAutoData(uint32_t exe_adrs, uint32_t exe_size, uint32_t lib_adrs, uint32_t lib_size)
{
	int		i, j;
	uint8_t	*data;
	int		size;

	for (i = 0; i < auto_count; i++)
	{
		FILE	*f = fopen(auto_data[i].path, "rb");

		if (!f)
		{
			printf("Data file '%s' not found !\n", auto_data[i].path);
			return (false);
		}
		fseek(f, 0, SEEK_END);
		auto_data[i].size = ftell(f);
		fclose(f);
	}
	for (i = 0; i < auto_count; i++)
	{
		AutoData	*d = &auto_data[i];
		const char	*clash = NULL;

		if ((d->adrs < RAM_USER_START) || (d->adrs + d->size > RAM_FIRMWARE))
		{
			printf("%s at 0x%X-0x%X: outside of 0x%X-0x%X (the cartridge firmware is above)\n", d->path, d->adrs,
				d->adrs + d->size, RAM_USER_START, RAM_FIRMWARE);
			return (false);
		}
		if (lib_size && (d->adrs < lib_adrs + lib_size) && (d->adrs + d->size > lib_adrs))
			clash = "the Net Yaroze library";
		else if ((d->adrs < exe_adrs + exe_size) && (d->adrs + d->size > exe_adrs))
			clash = "the program";
		for (j = 0; !clash && (j < i); j++)
			if ((d->adrs < auto_data[j].adrs + auto_data[j].size) && (d->adrs + d->size > auto_data[j].adrs))
				clash = auto_data[j].path;
		if (clash)
		{
			printf("%s at 0x%X-0x%X overlaps %s\n", d->path, d->adrs, d->adrs + d->size, clash);
			return (false);
		}
	}
	for (i = 0; i < auto_count; i++)
	{
		if (!(data = SystemGetFile(auto_data[i].path, &size)))
			return (false);
		printf("%s: %d bytes to 0x%X\n", auto_data[i].path, size, auto_data[i].adrs);
		usb_quiet = true;
		if (!USB_Process('U', data, auto_data[i].adrs, size))
		{
			usb_quiet = false;
			free(data);
			return (false);
		}
		usb_quiet = false;
		free(data);
	}
	return (true);
}

