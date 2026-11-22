/*
* PS1 DEV CART Transfer tool: crash report, symbols, stack unwinding, VRAM to PNG (from main.c)
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
// Crash report: registers of the crashed program (BIOS TCB) and stack, read while the PS1 DEV CART crash screen is displayed
// (the crash screen keeps processing USB commands with the PS1 DEV CART firmware)

Symbol	*symbols;
int		symbol_count;

int	SymbolCompare(const void *a, const void *b)
{
	uint32_t	x = ((const Symbol *)a)->adrs, y = ((const Symbol *)b)->adrs;

	return ((x > y) - (x < y));
}

int		symbol_max;

void	AddSymbol(uint32_t adrs, const char *name)
{
	if ((adrs < RAM_START) || (adrs >= RAM_END) || !name[0] || (name[0] == '.') || (name[0] == '$') || strchr(name, '='))
		return;
	if (symbol_count == symbol_max)
	{
		symbol_max = symbol_max ? symbol_max * 2 : 1024;
		symbols = realloc(symbols, symbol_max * sizeof(Symbol));
	}
	symbols[symbol_count].adrs = adrs;
	symbols[symbol_count].name = strdup(name);
	symbol_count++;
}

// ELF symbol table (SHT_SYMTAB section and its string table): defined functions, objects and labels
static uint32_t	Le32(uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}
static uint16_t	Le16(uint8_t *p)	{return (p[0] | (p[1] << 8));}

bool	LoadSymbolsElf(uint8_t *elf, int size)
{
	uint32_t	shoff = Le32(elf + 0x20), i, j;
	uint16_t	shentsize = Le16(elf + 0x2E), shnum = Le16(elf + 0x30);

	if ((elf[4] != 1) || (elf[5] != 1) || (shoff + shnum * shentsize > size))	// 32-bit little endian
		return (false);
	for (i = 0; i < shnum; i++)
	{
		uint8_t		*sh = elf + shoff + i * shentsize, *strsh;
		uint32_t	symoff = Le32(sh + 16), symsize = Le32(sh + 20), strtab;

		if ((Le32(sh + 4) != 2) || (Le32(sh + 24) >= shnum) || (symoff + symsize > size))	// SHT_SYMTAB
			continue;
		strsh = elf + shoff + Le32(sh + 24) * shentsize;
		strtab = Le32(strsh + 16);
		for (j = 0; j + 16 <= symsize; j += 16)
		{
			uint8_t		*sym = elf + symoff + j;
			uint32_t	name = Le32(sym), type = sym[12] & 15;

			if (Le16(sym + 14) && (type <= 2) && (strtab + name < size))	// Defined NOTYPE/OBJECT/FUNC
				AddSymbol(Le32(sym + 4), (char *)elf + strtab + name);
		}
	}
	return (true);
}

static bool	EcoffSymbolClass(uint32_t bits, bool external)
{
	uint32_t	st = bits & 0x3F, sc = (bits >> 6) & 0x1F;

	// Storage class: text, data, bss, absolute, sdata, sbss, rdata
	if ((sc != 1) && (sc != 2) && (sc != 3) && (sc != 5) && (sc != 13) && (sc != 14) && (sc != 15))
		return (false);
	return (external ? ((st == ECOFF_ST_GLOBAL) || (st == ECOFF_ST_PROC)) :
		((st == ECOFF_ST_STATIC) || (st == ECOFF_ST_STATICPROC)));	// (global procedures are also external)
}

bool	LoadSymbolsEcoff(uint8_t *ecoff, int size)
{
	uint32_t	symptr = Le32(ecoff + 8), i, j;
	uint8_t		*h = ecoff + symptr;
	uint32_t	isym, sym_off, ss_off, ss_ext_off, ifd, fd_off, iext, ext_off;

	if ((Le16(ecoff) != ECOFF_MAGIC) || !symptr || (symptr + 96 > (uint32_t)size) || (Le16(h) != ECOFF_HDRR_MAGIC))
		return (false);
	isym = Le32(h + 32);
	sym_off = Le32(h + 36);
	ss_off = Le32(h + 60);
	ss_ext_off = Le32(h + 68);
	ifd = Le32(h + 72);
	fd_off = Le32(h + 76);
	iext = Le32(h + 88);
	ext_off = Le32(h + 92);
	if ((ext_off + iext * 16 > (uint32_t)size) || (sym_off + isym * 12 > (uint32_t)size) ||
		(fd_off + ifd * 72 > (uint32_t)size))
		return (false);
	for (i = 0; i < iext; i++)		// EXTR: flags (2), file (2), SYMR: name (4), value (4), st/sc/index (4)
	{
		uint8_t		*e = ecoff + ext_off + i * 16;
		uint32_t	name = ss_ext_off + Le32(e + 4);

		if (EcoffSymbolClass(Le32(e + 12), true) && (name < (uint32_t)size) && memchr(ecoff + name, 0, size - name))
			AddSymbol(Le32(e + 8), (char *)ecoff + name);
	}
	for (i = 0; i < ifd; i++)		// FDR: names (issBase at 8), symbols (isymBase at 16, csym at 20)
	{
		uint8_t		*fd = ecoff + fd_off + i * 72;
		uint32_t	iss_base = Le32(fd + 8), isym_base = Le32(fd + 16), csym = Le32(fd + 20);

		for (j = 0; (j < csym) && (isym_base + j < isym); j++)
		{
			uint8_t		*s = ecoff + sym_off + (isym_base + j) * 12;
			uint32_t	name = ss_off + iss_base + Le32(s);

			if (EcoffSymbolClass(Le32(s + 8), false) && (name < (uint32_t)size) && memchr(ecoff + name, 0, size - name))
				AddSymbol(Le32(s + 4), (char *)ecoff + name);
		}
	}
	return (true);
}

// Symbol file: ELF executable, Net Yaroze ECOFF executable, GNU ld map file ("   0x80010000   main") or nm output
// ("80010000 T main")
void	LoadSymbols(char *filename)
{
	FILE		*f;
	char		line[1024], *tok[4], *name;
	uint8_t		magic[4] = {0};
	int			n;

	if (!(f = fopen(filename, "rb")))
	{
		printf("Symbol file '%s' not found !\n", filename);
		return;
	}
	fread(magic, 1, 4, f);
	fclose(f);
	if (!memcmp(magic, "\x7F" "ELF", 4))
	{
		uint8_t	*elf = SystemGetFile(filename, &n);

		if (elf && !LoadSymbolsElf(elf, n))
			printf("'%s' is not a 32-bit little endian ELF file !\n", filename);
		free(elf);
	}
	else if ((magic[0] | (magic[1] << 8)) == ECOFF_MAGIC)
	{
		uint8_t	*ecoff = SystemGetFile(filename, &n);

		if (ecoff && !LoadSymbolsEcoff(ecoff, n))
			printf("'%s': no symbol table in this ECOFF executable (stripped?)\n", filename);
		free(ecoff);
	}
	else if ((f = fopen(filename, "r")))
	{
		while (fgets(line, sizeof(line), f))
		{
			char	whole[1024];

			strcpy(whole, line);
			for (n = 0, tok[0] = strtok(line, " \t\r\n"); tok[n] && (n < 3); tok[++n] = strtok(NULL, " \t\r\n"))
				;
			if ((n == 2) && !strncmp(tok[0], "0x", 2) && strncmp(tok[1], "0x", 2))	// map: "0x80010000  main"
				name = tok[1];
			else if ((n == 3) && (strlen(tok[1]) == 1) && isalpha((unsigned char)tok[1][0]))	// nm: "80010000 T main"
			{
				// The rest of the line: C++ names from "nm -C" have spaces ("Game::draw(int, int)")
				name = whole + (tok[2] - line);
				name[strcspn(name, "\r\n")] = 0;
			}
			else
				continue;
			AddSymbol((uint32_t)strtoull(tok[0], NULL, 16), name);
		}
		fclose(f);
	}
	qsort(symbols, symbol_count, sizeof(Symbol), SymbolCompare);
	printf("%d symbols loaded from '%s'\n", symbol_count, filename);
}

// The last executable run with -e is remembered, its symbol file (same name: .map, .sym or .elf) is loaded automatically
char	*LastExePath(void)
{
	static char	path[4096];
	char		*tmp = getenv("TEMP");

#ifndef _WIN32
	if (!tmp)
		tmp = "/tmp";
#endif
	snprintf(path, sizeof(path), "%s/ps1transfer_last_exe.txt", tmp ? tmp : ".");
	return (path);
}

void	SaveLastExe(char *exe)
{
	char	full[4096];
	FILE	*f;

#ifdef _WIN32
	if (!_fullpath(full, exe, sizeof(full)))
#else
	if (!realpath(exe, full))
#endif
		return;
	if ((f = fopen(LastExePath(), "w")))
	{
		fprintf(f, "%s\n", full);
		fclose(f);
	}
}

void	AutoLoadSymbols(char *symfile)
{
	static const char	*exts[] = {".elf", ".map", ".sym", ".ELF", ".MAP", ".SYM"};	// (ELF: static functions too)
	char				exe[4096], full[4096], cand[4200], *dot, *sep;
	uint8_t				magic[2] = {0};
	FILE				*f;
	int					i;

	if (symbol_count)
		return;
	if (symfile)
	{
		LoadSymbols(symfile);
		return;
	}
	if (!(f = fopen(LastExePath(), "r")))
		return;
	exe[0] = 0;
	fgets(exe, sizeof(exe), f);
	fclose(f);
	exe[strcspn(exe, "\r\n")] = 0;
	// A Net Yaroze ECOFF executable (e.g. "main" of an auto script) has its own symbols, unless a file is given
	// next to it
	if ((f = fopen(exe, "rb")))
	{
		fread(magic, 1, 2, f);
		fclose(f);
	}
	strcpy(full, exe);
	dot = strrchr(exe, '.');
	sep = strrchr(exe, '\\') > strrchr(exe, '/') ? strrchr(exe, '\\') : strrchr(exe, '/');
	if (dot && (!sep || (dot > sep)))
		*dot = 0;
	for (i = 0; i < 6; i++)
	{
		snprintf(cand, sizeof(cand), "%s%s", exe, exts[i]);
		if (!access(cand, 0))
		{
			printf("Symbols of the last executable: ");
			LoadSymbols(cand);
			return;
		}
	}
	if ((magic[0] | (magic[1] << 8)) == ECOFF_MAGIC)
	{
		printf("Symbols of the last executable (ECOFF): ");
		LoadSymbols(full);
	}
}

// Address: hexadecimal, or a symbol "@name" / "@name+0x10" (symbol file loaded automatically or with -sym)
bool	ParseAddress(char *str, uint32_t *adrs, char *symfile)
{
	char	name[256], *plus;
	int		i;

	if (str[0] != '@')
	{
		*adrs = atohex(str);
		return (true);
	}
	AutoLoadSymbols(symfile);
	strncpy(name, str + 1, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	if ((plus = strchr(name, '+')))
		*plus++ = 0;
	for (i = 0; i < symbol_count; i++)
		if (!strcmp(symbols[i].name, name))
		{
			*adrs = symbols[i].adrs + (plus ? strtoul(plus, NULL, 0) : 0);
			printf("%s = 0x%08X\n", str, *adrs);
			return (true);
		}
	printf("Symbol '%s' not found%s\n", name, symbol_count ? "" : " (no symbol file: use -sym file, or run the program with -e first)");
	return (false);
}

// Nearest symbol at or before the address
Symbol	*FindSymbol(uint32_t adrs)
{
	int	low = 0, high = symbol_count - 1, mid;

	if (!symbol_count || (adrs < symbols[0].adrs))
		return (NULL);
	while (low < high)
	{
		mid = (low + high + 1) / 2;
		if (symbols[mid].adrs <= adrs)
			low = mid;
		else
			high = mid - 1;
	}
	return ((adrs - symbols[low].adrs < SYMBOL_MAX_OFFSET) ? &symbols[low] : NULL);
}

// Name and start of the symbol at or before an address (profiler), NULL if none
const char	*SymbolAt(uint32_t adrs, uint32_t *start)
{
	Symbol	*s = FindSymbol(adrs);

	if (!s)
		return (NULL);
	*start = s->adrs;
	return (s->name);
}

// A symbol of this name is loaded (profiler: the waiting functions of the program are known)
bool	SymbolExists(const char *name)
{
	int	i;

	for (i = 0; i < symbol_count; i++)
		if (!strcmp(symbols[i].name, name))
			return (true);
	return (false);
}

void	PrintAddress(uint32_t adrs)
{
	Symbol	*s = FindSymbol(adrs);

	printf("%08X", adrs);
	if (s)
		printf("  %s + 0x%X", s->name, adrs - s->adrs);
	printf("\n");
}

// RAM address seen through KSEG0 (the BIOS stores some pointers as physical addresses 0x0000xxxx, or KSEG1 0xA0xxxxxx)
uint32_t	RamAddress(uint32_t adrs)
{
	return (((adrs & 0x1FFFFFFF) < (RAM_END - RAM_START)) ? (RAM_START | (adrs & 0x1FFFFFFF)) : adrs);
}

bool	ReadWord(uint32_t adrs, uint32_t *value)
{
	adrs = RamAddress(adrs);
	return (((adrs & 3) == 0) && (adrs >= RAM_START) && (adrs < RAM_END) && USB_Process('D', (uint8_t *)value, adrs, 4));
}

// Function start: symbol, or the nearest "addiu sp,sp,-N" before pc (searched in the 1KB before it)
uint32_t	FunctionStart(uint32_t pc)
{
	Symbol		*s = FindSymbol(pc);
	uint32_t	code[256], start = (pc - RAM_START > 1024) ? pc - 1024 : RAM_START, n, i;

	if (s)
		return (s->adrs);
	n = (pc - start) / 4;
	if (!n || !USB_Process('D', (uint8_t *)code, start, n * 4))
		return (0);
	for (i = n; i-- > 0;)
		if ((code[i] & 0xFFFF8000) == 0x27BD8000)	// addiu sp,sp,-N
			return (start + i * 4);
	return (0);
}

// Walk the frames: each function prologue gives its frame size ("addiu sp,sp,-N") and where ra is saved
// ("sw ra,off(sp)"). Only the instructions before pc have been executed (frame not set up yet at the start).
int		UnwindStack(uint32_t *regs)
{
	uint32_t	pc = RamAddress(regs[32]), sp = RamAddress(regs[29]), ra = regs[31], start, code[256], n, i, frame, saved, at;
	int			depth, ra_off;

	printf("\nCall stack (from the function prologues, most recent first, callers shown at their call):\n");
	for (depth = 0; depth < CRASH_MAX_CALLERS; depth++)
	{
		// Callers: the call instruction (return address - 8), the return address can be after the end of the
		// function when it ends with a call that never returns
		at = depth ? pc - 8 : pc;
		printf("  #%-2d ", depth);
		PrintAddress(at);
		if (!(start = FunctionStart(at)) || (pc - start > sizeof(code)))
			break;
		n = (pc - start) / 4;
		if (n && !USB_Process('D', (uint8_t *)code, start, n * 4))
			break;
		frame = 0;
		ra_off = -1;
		for (i = 0; i < n; i++)
		{
			if (!frame && ((code[i] & 0xFFFF8000) == 0x27BD8000))		// addiu sp,sp,-N
				frame = -(int16_t)(code[i] & 0xFFFF);
			else if ((ra_off < 0) && ((code[i] & 0xFFFF0000) == 0xAFBF0000))	// sw ra,off(sp)
				ra_off = (int16_t)(code[i] & 0xFFFF);
		}
		if (ra_off >= 0)		// ra saved in this frame
		{
			if (!ReadWord(sp + ra_off, &saved))
				break;
			ra = saved;
		}
		else if (depth > 0)	// A caller always saves ra: unknown prologue
			break;
		sp += frame;
		// Next frame: the return address must point after a call in RAM
		if ((ra & 3) || (RamAddress(ra) < RAM_START + 0x10000) || (RamAddress(ra) >= RAM_END) || (RamAddress(ra) == pc))
			break;
		pc = RamAddress(ra);
	}
	return (depth + 1);
}

// 16-bit VRAM pixels (w * h) to a 24-bit PNG file
bool	VramToPng(const char *filename, const uint16_t *vram, int w, int h)
{
	uint8_t	*rgb = malloc(w * h * 3), *p = rgb;
	int		i;
	bool	ok;

	if (!rgb)
		return (false);
	for (i = 0; i < w * h; i++)
	{
		uint16_t	color = vram[i];
		uint8_t		r = color & 31;
		uint8_t		g = (color >> 5) & 31;
		uint8_t		b = (color >> 10) & 31;

		*p++ = (r << 3) | (r >> 2);
		*p++ = (g << 3) | (g >> 2);
		*p++ = (b << 3) | (b >> 2);
	}
	ok = !lodepng_encode24_file(filename, rgb, w, h);
	free(rgb);
	return (ok);
}

// Last frame displayed by the crashed program (monitor version 5): the crash screen is drawn in the program's draw
// area, the buffer it was drawing, so the frame it displayed is still in VRAM, above or below it (double buffering)
static void	CrashFrame(uint32_t info, const char *png)
{
	static const int	widths[4] = {256, 320, 512, 640};
	uint32_t	mode = (info >> 20) & 0x7F;	// GPUSTAT bits 16-22
	int			x = info & 0x3FF, y = (info >> 10) & 0x3FF, w, h, other, i, fy = -1, cand[4];
	uint16_t	*vram;

	w = (mode & 0x01) ? 368 : widths[(mode >> 1) & 3];
	h = (mode & 0x10) ? 256 : 240;
	other = (h == 256) ? 240 : 256;	// (PAL programs may use 240 lines, NTSC ones 256)
	if ((mode & 0x08) && (mode & 0x40))	// 480 lines, interlaced: one buffer
		h *= 2;
	cand[0] = y - h;
	cand[1] = y + h;
	cand[2] = (h == 480) ? -1 : y - other;
	cand[3] = (h == 480) ? -1 : y + other;
	for (i = 0; (i < 4) && (fy < 0); i++)
		if ((cand[i] >= 0) && (cand[i] + h <= 512))
			fy = cand[i];
	if (x + w > 1024)
		w = 1024 - x;
	if (fy < 0)
	{
		printf("\nLast frame: not kept (one display buffer: the crash screen is drawn over it)\n");
		return;
	}
	printf("\nLast frame displayed (double buffered programs): VRAM %d,%d %dx%d%s\n", x, fy, w, h,
		(mode & 0x20) ? " (24-bit video mode)" : "");
	printf("  (a program drawing where it displays has no second buffer: this area holds older VRAM contents)\n");
	if (!png)
	{
		printf("  Save it with -f: ps1transfer -crash [game.map] -f frame.png\n");
		return;
	}
	vram = malloc(w * h * sizeof(uint16_t));
	usb_quiet = true;
	if (vram && USB_Process('V', (uint8_t *)vram, (x << 16) | fy, (w << 16) | h) && VramToPng(png, vram, w, h))
		printf("  Saved to %s\n", png);
	else
		printf("  Can't save it to %s\n", png);
	usb_quiet = false;
	free(vram);
}

bool	CrashReport(char *symfile, const char *frame_png)
{
	MonitorState	ms;
	static const char	*cause_str[] = {"Interrupt", "TLB Modify", "TLB miss on load", "TLB miss on store",
							"Address error on load", "Address error on store", "Bus error on instruction fetch",
							"Bus error on data access", "System call", "Breakpoint instruction",
							"Reserved (illegal) instruction", "Coprocessor unusable", "Integer overflow", "Reserved"};
	static const char	*reg_names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
							"t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9",
							"k0", "k1", "gp", "sp", "fp", "ra"};
	uint32_t			tcbh, tcb, regs[40], stack[CRASH_STACK_SIZE / 4], sp, size, code, i, word, callers = 0;
	Symbol				*func;

	AutoLoadSymbols(symfile);

	// Registers saved by the BIOS exception handler in the current task control block
	if (!ReadWord(BIOS_TCBH_PTR, &tcbh) || !ReadWord(tcbh, &tcb) || !USB_Process('D', (uint8_t *)regs, RamAddress(tcb) + 8, sizeof(regs)))
	{
		printf("\nCan't read the registers: is the PS1 DEV CART crash screen displayed ? (needs the PS1 DEV CART firmware)\n");
		return (false);
	}
	if (!ps1_program_running)
		printf("\nNote: no program was executed since the last boot, these are the registers of the last exception.\n");
	if (USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)) && (ms.magic == MONITOR_MAGIC) &&
		(ms.version >= 5) && (ms.reason == MON_REASON_CRASH) && (ms.prof_draw & MON_CRASH_FRAME))
		CrashFrame(ms.prof_draw, frame_png);

	code = (regs[36] >> 2) & 0x1F;
	printf("\nCrash: %s (CAUSE %08X)\n", cause_str[(code < 13) ? code : 13], regs[36]);
	printf("PC  ");
	PrintAddress(regs[32]);
	if (regs[36] & 0x80000000)
		printf("    (in the branch delay slot: the faulting instruction is at PC + 4)\n");
	printf("RA  ");
	PrintAddress(regs[31]);

	printf("\nRegisters:\n");
	regs[0] = 0;	// r0 is always 0 (the BIOS uses this TCB slot for something else)
	for (i = 0; i < 32; i++)
		printf("  %-4s %08X%s", reg_names[i], regs[i], ((i & 3) == 3) ? "\n" : "");
	printf("  hi   %08X  lo   %08X  sr   %08X\n", regs[33], regs[34], regs[35]);

	// Return addresses on the stack: values just after a jal/jalr instruction (functions save ra on the stack)
	sp = RamAddress(regs[29]);
	if ((sp < RAM_START) || (sp >= RAM_END) || (sp & 3))
	{
		printf("\nInvalid stack pointer, no call chain.\n");
		return (true);
	}
	size = RAM_END - sp;
	if (size > CRASH_STACK_SIZE)
		size = CRASH_STACK_SIZE;
	if (!USB_Process('D', (uint8_t *)stack, sp, size))
		return (false);

	// Call stack from the function prologues (frame size and saved ra), the stack scan below is used if it stops early
	if (UnwindStack(regs) > 1)
		goto report_end;

	// With symbols, a return address must follow a "jal" to the function reached so far (start of the chain: PC),
	// so return addresses of calls already finished, still in the stack, are skipped.
	// A "jalr" (call through a function pointer) can't be checked: shown with "?".
	func = FindSymbol(RamAddress(regs[32]));
	printf("\n%s call chain, stack scan (most recent first):\n", symbol_count ? "Checked" : "Probable");
	for (i = 0; (i < size / 4 + 1) && (callers < CRASH_MAX_CALLERS); i++)
	{
		uint32_t	value = (i == 0) ? regs[31] : stack[i - 1];
		bool		is_jal, is_jalr;

		if ((value & 3) || (RamAddress(value) < RAM_START + 8) || (RamAddress(value) >= RAM_END) ||
			((i > 0) && (value == regs[31])) || (symbol_count && !FindSymbol(value)) || !ReadWord(value - 8, &word))
			continue;
		is_jal = ((word >> 26) == 3);
		is_jalr = (((word >> 26) == 0) && ((word & 0x3F) == 9));
		if (!is_jal && !is_jalr)
			continue;
		if (symbol_count && is_jal && func && ((((value - 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)) != func->adrs))
			continue;	// Calls another function: stale return address
		if (i == 0)
			printf("  ra        %c ", is_jalr ? '?' : ' ');
		else
			printf("  sp+0x%03X  %c ", (i - 1) * 4, is_jalr ? '?' : ' ');
		PrintAddress(value);
		func = FindSymbol(value);
		callers++;
	}

report_end:
	if (!symbol_count)
		printf("\nGive a symbol file to see function names: ps1transfer -crash game.map (.map, .sym or .elf),\n"
			"or put it next to the executable run with -e (same name) to load it automatically.\n");
	printf("\nThe crash screen is still displayed: read more memory with -d, a screenshot with -v, or reset with -r.\n");

	return (true);
}

/****************************************/

