/*
* PS1 DEV CART Transfer tool: GDB remote debugging server
*
* "ps1transfer -gdb [port] -f game.exe" loads the program stopped at its entry point and serves the GDB remote
* protocol on 127.0.0.1:port (default 3333), like the GDB server of PCSX-Redux (same registers description), so
* gdb-multiarch ("target remote") and VS Code can debug the program on the console.
*
* The PS1 side is the debug monitor (monitor/monitor.c): when the program stops (breakpoint, hardware breakpoint,
* pause, crash) it sends a 'B' request with the address of the saved registers, then answers the memory commands
* ('D'/'U', also used for the registers) until it is told to continue ('C').
*
* - Software breakpoints: break instructions written in the program code. GDB inserts/removes its breakpoints
*   around each stop: they are only written to the PS1 when the program resumes (changes only), and memory
*   reads show the original instructions.
* - Single step: breakpoints on the next instruction(s) (both ways of a branch), as the R3000 has no trace mode.
* - One hardware breakpoint (hbreak) and one watchpoint (watch/rwatch/awatch) with the COP0 debug registers.
* - Pause (Ctrl+C / VS Code pause): a 'P' byte, the program stops at the next interrupt.
* - printf of the program: shown here and sent to GDB (Debug Console in VS Code).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET	sock_t;
#define	CloseSocket	closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int		sock_t;
#define	INVALID_SOCKET	(-1)
#define	CloseSocket	close
#endif

#include "gdbserver.h"
#include "../monitor/monitor.h"

#define	BP_INSN			0x0005000D	// "break 5", like GDB for MIPS
#define	MAX_BP			1024
#define	PACKET_MAX		0x1000		// Announced to GDB: memory reads of up to 2KB (one USB packet)
#define	CONSOLE_MAX		256			// printf text sent to GDB in 'O' packets of up to this size
#define	STOP_TIMEOUT	10			// Seconds to wait for the stop at the entry point

#define	DCIC_EXEC		(1 << 24)
#define	DCIC_DATA		(1 << 25)
#define	DCIC_READ		(1 << 26)
#define	DCIC_WRITE		(1 << 27)
// Enable bits: 23 and 31, and both 29 and 30 (tested on a console: without bit 29 no breakpoint triggers, the
// cartridge boot code also sets both). DCIC_HIT_DATA: status bit of a data breakpoint.
#define	DCIC_ENABLE		((1 << 23) | (1 << 29) | (1 << 30) | (1 << 31))
#define	DCIC_HIT_DATA	(1 << 2)

typedef struct
{
	uint32_t	adrs, orig;
	bool		want;		// Inserted by GDB (Z0)
	bool		temp;		// Single step
	bool		inserted;	// Break instruction written in the PS1 memory
	bool		orig_ok;
} Breakpoint;

static sock_t		client = INVALID_SOCKET;
static bool			noack, session;		// session: GDB has sent a packet (not a port probe)
static bool			running, gdb_waiting, stepping;
static uint32_t		regs[MON_REGS], regs_adrs;
static bool			regs_valid, regs_dirty;
static int			stop_reason, stop_exc;
static uint32_t		stop_dcic;
static Breakpoint	bps[MAX_BP];
static int			n_bps;
static bool			hw_exec_on, hw_exec_temp, hw_watch_on, hw_dirty;
static uint32_t		hw_exec, hw_watch, hw_watch_len;
static int			hw_watch_type;		// 2: write, 3: read, 4: access (Z packet type)
static char			console[CONSOLE_MAX + 1];
static int			console_len;

enum	{SERVE, QUIT_DETACH, QUIT_KILL};

// Registers description (same as PCSX-Redux, so GDB uses the same registers numbers): 32 GPR, sr, lo, hi, bad,
// cause, pc, then the FPU registers GDB requires for MIPS (always 0)
static const char	target_xml[] =
	"<?xml version=\"1.0\"?><!DOCTYPE feature SYSTEM \"gdb-target.dtd\"><target version=\"1.0\">"
	"<architecture>mips:3000</architecture><osabi>none</osabi>"
	"<feature name=\"org.gnu.gdb.mips.cpu\">"
	"<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
	"<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
	"<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/>"
	"<reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
	"<reg name=\"r12\" bitsize=\"32\"/><reg name=\"r13\" bitsize=\"32\"/><reg name=\"r14\" bitsize=\"32\"/>"
	"<reg name=\"r15\" bitsize=\"32\"/><reg name=\"r16\" bitsize=\"32\"/><reg name=\"r17\" bitsize=\"32\"/>"
	"<reg name=\"r18\" bitsize=\"32\"/><reg name=\"r19\" bitsize=\"32\"/><reg name=\"r20\" bitsize=\"32\"/>"
	"<reg name=\"r21\" bitsize=\"32\"/><reg name=\"r22\" bitsize=\"32\"/><reg name=\"r23\" bitsize=\"32\"/>"
	"<reg name=\"r24\" bitsize=\"32\"/><reg name=\"r25\" bitsize=\"32\"/><reg name=\"r26\" bitsize=\"32\"/>"
	"<reg name=\"r27\" bitsize=\"32\"/><reg name=\"r28\" bitsize=\"32\"/><reg name=\"r29\" bitsize=\"32\"/>"
	"<reg name=\"r30\" bitsize=\"32\"/><reg name=\"r31\" bitsize=\"32\"/>"
	"<reg name=\"lo\" bitsize=\"32\" regnum=\"33\"/><reg name=\"hi\" bitsize=\"32\" regnum=\"34\"/>"
	"<reg name=\"pc\" bitsize=\"32\" regnum=\"37\"/></feature>"
	"<feature name=\"org.gnu.gdb.mips.cp0\"><reg name=\"status\" bitsize=\"32\" regnum=\"32\"/>"
	"<reg name=\"badvaddr\" bitsize=\"32\" regnum=\"35\"/><reg name=\"cause\" bitsize=\"32\" regnum=\"36\"/></feature>"
	"<feature name=\"org.gnu.gdb.mips.fpu\">"
	"<reg name=\"f0\" bitsize=\"32\" type=\"ieee_single\" regnum=\"38\"/><reg name=\"f1\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f2\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f3\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f4\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f5\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f6\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f7\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f8\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f9\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f10\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f11\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f12\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f13\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f14\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f15\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f16\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f17\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f18\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f19\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f20\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f21\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f22\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f23\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f24\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f25\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f26\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f27\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f28\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f29\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"f30\" bitsize=\"32\" type=\"ieee_single\"/><reg name=\"f31\" bitsize=\"32\" type=\"ieee_single\"/>"
	"<reg name=\"fcsr\" bitsize=\"32\" group=\"float\"/><reg name=\"fir\" bitsize=\"32\" group=\"float\"/>"
	"</feature></target>";

/****************************************/
// PS1 memory

// RAM (2MB, any segment), scratchpad, BIOS ROM (read only). Other addresses (I/O, unmapped) could crash the PS1.
static bool	ValidRange(uint32_t adrs, uint32_t len, bool write)
{
	uint32_t	phys = adrs & 0x1FFFFFFF;

	if ((adrs >= 0xC0000000) || !len || (adrs + len < adrs))
		return (false);
	if (phys + len <= 0x00200000)
		return (true);
	if ((phys >= 0x1F800000) && (phys + len <= 0x1F800400) && (adrs < 0xA0000000))
		return (true);
	return (!write && (phys >= 0x1FC00000) && (phys + len <= 0x1FC80000));
}

// Code addresses where a break instruction can be written (RAM above the kernel area)
static bool	CodeWritable(uint32_t adrs)
{
	return (!(adrs & 3) && ValidRange(adrs, 4, true) && ((adrs & 0x1FFFFFFF) >= 0x10000) && ((adrs & 0x1FFFFFFF) < 0x200000));
}

static bool	ReadMemRaw(uint32_t adrs, uint8_t *buf, uint32_t len)
{
	uint32_t	n;

	for (; len; adrs += n, buf += n, len -= n)
	{
		n = (len > 2048) ? 2048 : len;
		if (!USB_Process('D', buf, adrs, n))
			return (false);
	}
	return (true);
}

static bool	WriteMemRaw(uint32_t adrs, uint8_t *buf, uint32_t len)
{
	uint32_t	n;

	for (; len; adrs += n, buf += n, len -= n)
	{
		n = (len > 2048) ? 2048 : len;
		if (!USB_Process('U', buf, adrs, n))
			return (false);
	}
	return (true);
}

static bool	SameWord(uint32_t a, uint32_t b)	// Same RAM word through any segment
{
	return ((a & 0x1FFFFFFC) == (b & 0x1FFFFFFC));
}

// Memory as GDB sees it: original instructions where break instructions are written
static bool	ReadMem(uint32_t adrs, uint8_t *buf, uint32_t len)
{
	int			i, k;
	uint32_t	a;

	if (!ReadMemRaw(adrs, buf, len))
		return (false);
	for (i = 0; i < n_bps; i++)
		if (bps[i].inserted)
			for (k = 0; k < 4; k++)
			{
				a = (bps[i].adrs & 0x1FFFFFFF) + k;
				if ((a >= (adrs & 0x1FFFFFFF)) && (a < (adrs & 0x1FFFFFFF) + len))
					buf[a - (adrs & 0x1FFFFFFF)] = bps[i].orig >> (k * 8);
			}
	return (true);
}

static bool	WriteMem(uint32_t adrs, uint8_t *buf, uint32_t len)
{
	int	i;

	// Written over a break instruction: it is gone, the new code becomes the original one
	for (i = 0; i < n_bps; i++)
		if (((bps[i].adrs & 0x1FFFFFFF) + 4 > (adrs & 0x1FFFFFFF)) && ((bps[i].adrs & 0x1FFFFFFF) < (adrs & 0x1FFFFFFF) + len))
			bps[i].inserted = bps[i].orig_ok = false;
	return (WriteMemRaw(adrs, buf, len));
}

static bool	ReadWord(uint32_t adrs, uint32_t *value)
{
	uint8_t	b[4];

	if (!ValidRange(adrs, 4, false) || !ReadMem(adrs, b, 4))
		return (false);
	*value = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
	return (true);
}

static bool	FetchRegs(void)
{
	if (!regs_valid)
	{
		if (!ReadMemRaw(regs_adrs, (uint8_t *)regs, MON_REGS * 4))
			return (false);
		regs[0] = 0;
		regs_valid = true;
		regs_dirty = false;
	}
	return (true);
}

/****************************************/
// Breakpoints

static Breakpoint	*FindBp(uint32_t adrs)
{
	int	i;

	for (i = 0; i < n_bps; i++)
		if (SameWord(bps[i].adrs, adrs))
			return (&bps[i]);
	return (NULL);
}

static Breakpoint	*AddBp(uint32_t adrs)
{
	Breakpoint	*bp = FindBp(adrs);

	if (!bp && (n_bps < MAX_BP))
	{
		bp = &bps[n_bps++];
		memset(bp, 0, sizeof(*bp));
		bp->adrs = adrs;
	}
	return (bp);
}

// Writes/removes the break instructions so the PS1 memory matches what GDB wants, before resuming
static bool	SyncBreakpoints(void)
{
	int			i, j;
	uint32_t	insn = BP_INSN;
	Breakpoint	*bp;

	for (i = 0; i < n_bps; i++)
	{
		bp = &bps[i];
		if ((bp->want || bp->temp) && !bp->inserted)
		{
			if (!bp->orig_ok && !ReadMemRaw(bp->adrs, (uint8_t *)&bp->orig, 4))
				return (false);
			bp->orig_ok = true;
			if (bp->orig != BP_INSN)
			{
				if (!WriteMemRaw(bp->adrs, (uint8_t *)&insn, 4))
					return (false);
				bp->inserted = true;
			}
		}
		else if (!bp->want && !bp->temp && bp->inserted)
		{
			if (!WriteMemRaw(bp->adrs, (uint8_t *)&bp->orig, 4))
				return (false);
			bp->inserted = false;
		}
	}
	// Forget the breakpoints not used anymore (the original instructions stay known while they are inserted)
	for (i = j = 0; i < n_bps; i++)
		if (bps[i].want || bps[i].temp || bps[i].inserted)
			bps[j++] = bps[i];
	n_bps = j;
	return (true);
}

// COP0 debug registers applied by the monitor when the program resumes
static bool	SyncHardware(void)
{
	uint32_t	cop0[5] = {0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0};	// bpc, bpcm, bda, bdam, dcic
	uint32_t	size;

	if (!hw_dirty)
		return (true);
	if (hw_exec_on || hw_exec_temp)
	{
		cop0[0] = hw_exec;
		cop0[4] |= DCIC_EXEC;
	}
	if (hw_watch_on)
	{
		// The address mask covers the smallest aligned block holding the watched bytes (GDB checks if a watched
		// value really changed)
		for (size = 4; (hw_watch & ~(size - 1)) + size < hw_watch + hw_watch_len; size <<= 1)
			;
		cop0[2] = hw_watch & ~(size - 1);
		cop0[3] = ~(size - 1);
		cop0[4] |= DCIC_DATA | ((hw_watch_type != 3) ? DCIC_WRITE : 0) | ((hw_watch_type != 2) ? DCIC_READ : 0);
	}
	if (cop0[4])
		cop0[4] |= DCIC_ENABLE;
	if (!WriteMemRaw(MONITOR_STATE_ADRS + offsetof(MonitorState, bpc), (uint8_t *)cop0, sizeof(cop0)))
		return (false);
	hw_dirty = false;
	return (true);
}

/****************************************/
// GDB connection

static int	Hex(char c)
{
	if ((c >= '0') && (c <= '9'))
		return (c - '0');
	if ((c >= 'a') && (c <= 'f'))
		return (c - 'a' + 10);
	if ((c >= 'A') && (c <= 'F'))
		return (c - 'A' + 10);
	return (-1);
}

static uint32_t	ParseHex(const char **p)
{
	uint32_t	v = 0;

	while (Hex(**p) >= 0)
		v = (v << 4) | Hex(*(*p)++);
	return (v);
}

static void	SendRaw(const char *data, int len)
{
	if (client != INVALID_SOCKET)
		send(client, data, len, 0);
}

static void	SendPacket(const char *data)
{
	char			*pkt;
	int				len = strlen(data), i;
	unsigned char	sum = 0;

	if (!(pkt = malloc(len + 5)))
		return;
	pkt[0] = '$';
	for (i = 0; i < len; i++)
		sum += (unsigned char)data[i];
	memcpy(pkt + 1, data, len);
	sprintf(pkt + 1 + len, "#%02x", sum);
	SendRaw(pkt, len + 4);
	free(pkt);
}

static void	HexWord(char *out, uint32_t v)	// Little endian, as GDB expects target order
{
	sprintf(out, "%02x%02x%02x%02x", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
}

// printf of the program: on the console, and to GDB while it waits for the program
static void	FlushConsole(void)
{
	char	*pkt;
	int		i;

	if (!console_len)
		return;
	if (session && running && gdb_waiting && (pkt = malloc(console_len * 2 + 2)))
	{
		pkt[0] = 'O';
		for (i = 0; i < console_len; i++)
			sprintf(pkt + 1 + i * 2, "%02x", (unsigned char)console[i]);
		SendPacket(pkt);
		free(pkt);
	}
	console_len = 0;
}

static void	ConsoleChar(char c)
{
	putchar(c);
	console[console_len++] = c;
	if ((c == '\n') || (console_len >= CONSOLE_MAX))
		FlushConsole();
}

// GDB register number to saved register index (-1: not saved, reads 0)
static int	RegIndex(int n)
{
	static const int	cp0[6] = {MON_REG_SR, MON_REG_LO, MON_REG_HI, -1, MON_REG_CAUSE, MON_REG_EPC};

	if (n < 32)
		return (n);
	return ((n < 38) ? cp0[n - 32] : -1);
}

static void	StopReply(char *out)
{
	static const int	signals[32] = {
		// Interrupt, TLB x3, address error load/store, bus error x2, syscall, break, reserved instruction, cop unusable, overflow
		5, 11, 11, 11, 10, 10, 10, 10, 5, 5, 4, 4, 8, 11, 11, 11,
		11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11};
	int	sig = 5;

	if (stop_reason == MON_REASON_PAUSE)
		sig = 2;	// SIGINT
	else if (stop_reason == MON_REASON_CRASH)
		sig = signals[stop_exc & 31];
	if ((stop_reason == MON_REASON_DEBUG) && (stop_dcic & DCIC_HIT_DATA) && hw_watch_on)
		sprintf(out, "T05%swatch:%08x;", (hw_watch_type == 3) ? "r" : ((hw_watch_type == 4) ? "a" : ""), hw_watch);
	else
		sprintf(out, "S%02x", sig);
}

// Next instruction(s) after the one at pc (both ways of a branch, after its delay slot)
static int	NextPCs(uint32_t pc, uint32_t insn, uint32_t *out)
{
	uint32_t	op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
	uint32_t	branch = pc + 4 + ((int32_t)(int16_t)(insn & 0xFFFF) << 2);

	switch (op)
	{
		case 0:		// SPECIAL: JR, JALR
			if (((insn & 63) == 8) || ((insn & 63) == 9))
			{
				out[0] = regs[rs];
				return (1);
			}
			break;
		case 1:		// REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
			if ((rt == 0) || (rt == 1) || (rt == 0x10) || (rt == 0x11))
			{
				out[0] = pc + 8;
				out[1] = branch;
				return (2);
			}
			break;
		case 2:		// J, JAL
		case 3:
			out[0] = ((pc + 4) & 0xF0000000) | ((insn & 0x03FFFFFF) << 2);
			return (1);
		case 4:		// BEQ, BNE, BLEZ, BGTZ
		case 5:
		case 6:
		case 7:
			out[0] = pc + 8;
			out[1] = branch;
			return (2);
		case 0x10:	// BCzF/BCzT
		case 0x11:
		case 0x12:
		case 0x13:
			if (rs == 8)
			{
				out[0] = pc + 8;
				out[1] = branch;
				return (2);
			}
			break;
	}
	out[0] = pc + 4;
	return (1);
}

static bool	Resume(bool step, const char *args)
{
	uint32_t	pc, insn, next[2];
	int			i, n;
	Breakpoint	*bp;

	if (!FetchRegs())
		return (false);
	if (*args)
	{
		regs[MON_REG_EPC] = ParseHex(&args);
		regs_dirty = true;
	}
	pc = regs[MON_REG_EPC];
	if (!ReadWord(pc, &insn))
		insn = 0;

	// A break instruction of the program itself (not a breakpoint): continue after it
	if (((insn & 0xFC00003F) == 0x0000000D) && !FindBp(pc))
	{
		regs[MON_REG_EPC] = pc += 4;
		regs_dirty = true;
		if (!step || !ReadWord(pc, &insn))
			insn = 0;
	}

	if (step)
	{
		n = NextPCs(pc, insn, next);
		for (i = 0; i < n; i++)
		{
			if (CodeWritable(next[i]) && (bp = AddBp(next[i])))
				bp->temp = true;
			else if (!hw_exec_on)	// BIOS ROM, kernel: hardware breakpoint
			{
				hw_exec = next[i];
				hw_exec_temp = hw_dirty = true;
			}
			else
				return (false);
		}
	}

	if (!SyncBreakpoints() || !SyncHardware())
		return (false);
	if (regs_dirty && !WriteMemRaw(regs_adrs, (uint8_t *)regs, (MON_REG_CAUSE + 1) * 4))
		return (false);
	regs_valid = regs_dirty = false;
	if (!USB_Process(MON_CMD_CONTINUE, NULL, 0, 0))
		return (false);
	running = gdb_waiting = true;
	stepping = step;
	return (true);
}

// Stop notification from the PS1 (registers address, reason << 8 | ExcCode)
static void	OnStop(void)
{
	int		i;
	char	reply[64];

	running = false;
	regs_adrs = mon_stop_regs;
	regs_valid = false;
	stop_reason = (mon_stop_info >> 8) & 0xFF;
	stop_exc = mon_stop_info & 31;
	stop_dcic = 0;
	if (stop_reason == MON_REASON_DEBUG)
		ReadMemRaw(MONITOR_STATE_ADRS + offsetof(MonitorState, dcic_stop), (uint8_t *)&stop_dcic, 4);
	// Single step breakpoints are removed (in the PS1 memory at the next resume, GDB doesn't see them)
	for (i = 0; i < n_bps; i++)
		bps[i].temp = false;
	if (hw_exec_temp)
	{
		hw_exec_temp = false;
		hw_dirty = true;
	}
	stepping = false;
	FlushConsole();
	if (gdb_waiting && (client != INVALID_SOCKET))
	{
		StopReply(reply);
		SendPacket(reply);
		gdb_waiting = false;
	}
}

// Text and requests from the PS1: printf, PCDRV file server, stop notification
static bool	PollUSB(void)
{
	uint8_t	c;
	int		n;

	for (n = 0; (n < 4096) && USB_ReadByteNoWait(&c); n++)
	{
		if (c == 16)	// Data Link Escape
		{
			PC_FileServer();
			if (mon_stop_pending)
			{
				mon_stop_pending = false;
				OnStop();
				return (true);
			}
		}
		else
			ConsoleChar(c);
	}
	FlushConsole();
	return (n != 0);
}

static void	ReadRegsReply(char *out)
{
	int	i, r;

	for (i = 0; i < 72; i++)
	{
		r = RegIndex(i);
		HexWord(out + i * 8, (r >= 0) ? regs[r] : 0);
	}
	out[72 * 8] = 0;
}

static int	Breakpoints(const char *p, char *out)
{
	bool		add = (*p++ == 'Z');
	int			type = ParseHex(&p);
	uint32_t	adrs, kind;
	Breakpoint	*bp;

	if (*p++ != ',')
		return (0);
	adrs = ParseHex(&p);
	if (*p++ != ',')
		return (0);
	kind = ParseHex(&p);
	if (*p == ';')	// Conditions on the target: not supported
		return (0);

	switch (type)
	{
		case 0:		// Software breakpoint
			if (add)
			{
				if (!CodeWritable(adrs) || !(bp = AddBp(adrs)))
					return (strcpy(out, "E01"), 1);
				bp->want = true;
			}
			else if ((bp = FindBp(adrs)))
				bp->want = false;
			break;
		case 1:		// Hardware breakpoint (one)
			if (add && hw_exec_on && !SameWord(hw_exec, adrs))
				return (strcpy(out, "E01"), 1);
			hw_exec = adrs;
			hw_exec_on = add;
			hw_dirty = true;
			break;
		case 2:		// Watchpoints (one): write, read, access
		case 3:
		case 4:
			if (add && ((hw_watch_on && ((hw_watch != adrs) || (hw_watch_type != type))) || !kind || (kind > 4096)))
				return (strcpy(out, "E01"), 1);
			hw_watch = adrs;
			hw_watch_len = kind;
			hw_watch_type = type;
			hw_watch_on = add;
			hw_dirty = true;
			break;
		default:
			return (0);
	}
	strcpy(out, "OK");
	return (1);
}

static void	MonitorCommand(const char *hex, char *out)
{
	char	cmd[256], msg[512];
	int		i;

	for (i = 0; (i < sizeof(cmd) - 1) && (Hex(hex[0]) >= 0) && (Hex(hex[1]) >= 0); i++, hex += 2)
		cmd[i] = (Hex(hex[0]) << 4) | Hex(hex[1]);
	cmd[i] = 0;
	if (!strcmp(cmd, "reset"))
		strcpy(msg, "Use \"kill\" to reset the PS1 (the debug session ends).\n");
	else if (!strcmp(cmd, "cache flush"))
		strcpy(msg, "The instruction cache is flushed each time the program resumes.\n");
	else
		sprintf(msg, "PS1 DEV CART monitor: %d breakpoint(s), hardware breakpoint %s, watchpoint %s.\n",
			n_bps, hw_exec_on ? "on" : "off", hw_watch_on ? "on" : "off");
	out[0] = 'O';
	for (i = 0; msg[i]; i++)
		sprintf(out + 1 + i * 2, "%02x", (unsigned char)msg[i]);
	SendPacket(out);
	strcpy(out, "OK");
}

// Handles a GDB packet while the program is stopped, the reply is in out ("" = not supported)
static int	HandlePacket(char *pkt, char *out)
{
	const char	*p = pkt + 1;
	uint32_t	adrs, len, i, v;
	uint8_t		*buf;
	int			n;

	out[0] = 0;
	switch (pkt[0])
	{
		case '?':
			StopReply(out);
			break;

		case 'g':
			if (!FetchRegs())
				return (strcpy(out, "E01"), SERVE);
			ReadRegsReply(out);
			break;

		case 'G':
			if (!FetchRegs())
				return (strcpy(out, "E01"), SERVE);
			for (i = 0; (i < 72) && p[0]; i++, p += 8)
			{
				n = RegIndex(i);
				v = (Hex(p[0]) << 4 | Hex(p[1])) | (Hex(p[2]) << 4 | Hex(p[3])) << 8 |
					(Hex(p[4]) << 4 | Hex(p[5])) << 16 | (uint32_t)(Hex(p[6]) << 4 | Hex(p[7])) << 24;
				if ((n > 0) && (n != MON_REG_SR) && (n != MON_REG_CAUSE))
					regs[n] = v;
			}
			regs_dirty = true;
			strcpy(out, "OK");
			break;

		case 'p':
			if (!FetchRegs())
				return (strcpy(out, "E01"), SERVE);
			n = RegIndex(ParseHex(&p));
			HexWord(out, (n >= 0) ? regs[n] : 0);
			break;

		case 'P':
			if (!FetchRegs())
				return (strcpy(out, "E01"), SERVE);
			n = RegIndex(ParseHex(&p));
			if (*p++ == '=')
			{
				v = ParseHex(&p);
				v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);	// Sent in target order
				if ((n > 0) && (n != MON_REG_SR) && (n != MON_REG_CAUSE))
				{
					regs[n] = v;
					regs_dirty = true;
				}
			}
			strcpy(out, "OK");
			break;

		case 'm':
			adrs = ParseHex(&p);
			len = (*p++ == ',') ? ParseHex(&p) : 0;
			if (len > PACKET_MAX / 2 - 8)
				len = PACKET_MAX / 2 - 8;
			if (!ValidRange(adrs, len, false) || !(buf = malloc(len)))
				return (strcpy(out, "E01"), SERVE);
			if (ReadMem(adrs, buf, len))
				for (i = 0; i < len; i++)
					sprintf(out + i * 2, "%02x", buf[i]);
			else
				strcpy(out, "E02");
			free(buf);
			break;

		case 'M':
			adrs = ParseHex(&p);
			len = (*p++ == ',') ? ParseHex(&p) : 0;
			if ((*p++ != ':') || !ValidRange(adrs, len, true) || (strlen(p) < len * 2) || !(buf = malloc(len)))
				return (strcpy(out, "E01"), SERVE);
			for (i = 0; i < len; i++)
				buf[i] = (Hex(p[i * 2]) << 4) | Hex(p[i * 2 + 1]);
			strcpy(out, WriteMem(adrs, buf, len) ? "OK" : "E02");
			free(buf);
			break;

		case 'c':
		case 's':
			if (!Resume(pkt[0] == 's', p))
			{
				strcpy(out, "E01");
				break;
			}
			return (-1);	// No reply until the program stops

		case 'Z':
		case 'z':
			if (!Breakpoints(pkt, out))
				out[0] = 0;
			break;

		case 'D':
			strcpy(out, "OK");
			return (QUIT_DETACH);

		case 'k':
			return (QUIT_KILL);

		case 'H':
		case 'T':
			strcpy(out, "OK");
			break;

		case 'q':
			if (!strncmp(pkt, "qSupported", 10))
				sprintf(out, "PacketSize=%x;qXfer:features:read+;QStartNoAckMode+", PACKET_MAX);
			else if (!strncmp(pkt, "qXfer:features:read:target.xml:", 31))
			{
				p = pkt + 31;
				adrs = ParseHex(&p);
				len = (*p++ == ',') ? ParseHex(&p) : 0;
				if (adrs >= sizeof(target_xml) - 1)
					strcpy(out, "l");
				else
				{
					if (len > PACKET_MAX - 16)
						len = PACKET_MAX - 16;
					n = (sizeof(target_xml) - 1 - adrs > len) ? len : sizeof(target_xml) - 1 - adrs;
					out[0] = (adrs + n < sizeof(target_xml) - 1) ? 'm' : 'l';
					memcpy(out + 1, target_xml + adrs, n);
					out[n + 1] = 0;
				}
			}
			else if (!strcmp(pkt, "qAttached"))
				strcpy(out, "1");
			else if (!strcmp(pkt, "qC"))
				strcpy(out, "QC1");
			else if (!strcmp(pkt, "qfThreadInfo"))
				strcpy(out, "m1");
			else if (!strcmp(pkt, "qsThreadInfo"))
				strcpy(out, "l");
			else if (!strncmp(pkt, "qSymbol", 7))
				strcpy(out, "OK");
			else if (!strncmp(pkt, "qRcmd,", 6))
				MonitorCommand(pkt + 6, out);
			break;

		case 'Q':
			if (!strcmp(pkt, "QStartNoAckMode"))
			{
				SendPacket("OK");
				noack = true;
				return (-1);
			}
			break;

		case 'v':
			if (!strncmp(pkt, "vKill", 5))
				return (QUIT_KILL);
			break;
	}
	return (SERVE);
}

/****************************************/

static bool	WaitReadable(sock_t s, int ms)
{
	fd_set			set;
	struct timeval	tv = {0, ms * 1000};

	FD_ZERO(&set);
	FD_SET(s, &set);
	return (select((int)s + 1, &set, NULL, NULL, &tv) > 0);
}

static void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

// Waits for the stop notification of the PS1 (false after the timeout)
static bool	WaitStop(int seconds)
{
	time_t	start = time(NULL);

	while (running)
	{
		if (!PollUSB())
			SleepMs(1);
		if (time(NULL) - start > seconds)
			return (false);
	}
	return (true);
}

// Breakpoints and hardware breakpoints removed, program resumed (GDB detached or disconnected)
static void	EndSession(void)
{
	int	i;

	for (i = 0; i < n_bps; i++)
		bps[i].want = bps[i].temp = false;
	hw_exec_on = hw_exec_temp = hw_watch_on = false;
	hw_dirty = true;
	gdb_waiting = false;
	if (running && n_bps)	// The breakpoints can only be removed while the program is stopped
	{
		USB_WriteBytes((uint8_t *)"P", 1);
		if (!WaitStop(3))
		{
			printf("\nWarning: the program runs with %d breakpoint(s) left, reset the PS1 if it stops.\n", n_bps);
			return;
		}
	}
	if (!running && Resume(false, ""))
		printf("\nDebugger detached, the program runs.\n");
	gdb_waiting = false;
}

// GDB packets received: Ctrl+C while the program runs, packets while it is stopped (GDB doesn't send packets
// while the program runs, all-stop mode)
static int	ProcessRx(char *rx, int *rx_len, char *pkt, char *out)
{
	int	i, n;

	while (*rx_len)
	{
		if (rx[0] != '$')
		{
			if ((rx[0] == 3) && running)
				USB_WriteBytes((uint8_t *)"P", 1);	// The program stops at its next interrupt
			memmove(rx, rx + 1, --(*rx_len));
			continue;
		}
		for (i = 1; (i < *rx_len) && (rx[i] != '#'); i++)
			;
		if (i + 2 >= *rx_len)	// Incomplete
		{
			if (*rx_len >= PACKET_MAX * 2)
				*rx_len = 0;
			return (SERVE);
		}
		if (running)
			return (SERVE);
		memcpy(pkt, rx + 1, i - 1);
		pkt[i - 1] = 0;
		memmove(rx, rx + i + 3, *rx_len -= i + 3);
		if (!noack)
			SendRaw("+", 1);
		session = true;
		n = HandlePacket(pkt, out);
		if ((n == SERVE) || (n == QUIT_DETACH))
			SendPacket(out);
		if (n == QUIT_DETACH)
		{
			EndSession();
			return (n);
		}
		if (n == QUIT_KILL)
		{
			SendPacket("OK");
			printf("\nDebugger killed the program: resetting the PS1.\n");
			USB_Process('R', NULL, 0, 0);
			return (n);
		}
	}
	return (SERVE);
}


int	GdbServer(int port, uint32_t entry_pc, uint32_t entry_insn)
{
	sock_t				listener;
	struct sockaddr_in	sa;
	char				*rx, *pkt, *out;
	int					rx_len = 0, n, state = SERVE, one = 1;
#ifdef _WIN32
	WSADATA				wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!(rx = malloc(PACKET_MAX * 2)) || !(pkt = malloc(PACKET_MAX * 2)) || !(out = malloc(PACKET_MAX * 2 + 64)))
		return (1);

	// The program stops on the break instruction written at its entry point, which is put back
	printf("Waiting for the program to stop at its entry point 0x%08X...\n", entry_pc);
	running = true;
	if (!WaitStop(STOP_TIMEOUT))
	{
		printf("The program didn't stop: the firmware in the cartridge needs the debug monitor.\n");
		return (1);
	}
	if (!WriteMemRaw(entry_pc, (uint8_t *)&entry_insn, 4) || !FetchRegs())
	{
		printf("USB error.\n");
		return (1);
	}
	printf("Stopped at 0x%08X.\n", regs[MON_REG_EPC]);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// Local connections only
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
	if ((listener == INVALID_SOCKET) || bind(listener, (struct sockaddr *)&sa, sizeof(sa)) || listen(listener, 1))
	{
		printf("Can't listen on port %d (already used?)\n", port);
		return (1);
	}
	printf("GDB server on port %d (gdb: target remote localhost:%d)\n", port, port);

	while (state == SERVE)
	{
		if (running)
			PollUSB();

		// New connection (VS Code first checks the port with a connection closed at once)
		if (client == INVALID_SOCKET)
		{
			if (WaitReadable(listener, running ? 2 : 20) && ((client = accept(listener, NULL, NULL)) != INVALID_SOCKET))
			{
				setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
				noack = session = false;
				rx_len = 0;
			}
			continue;
		}

		if (WaitReadable(client, running ? 2 : 20))
		{
			n = recv(client, rx + rx_len, PACKET_MAX * 2 - rx_len, 0);
			if (n <= 0)
			{
				CloseSocket(client);
				client = INVALID_SOCKET;
				if (session)	// The debugger has gone: like a detach
				{
					printf("\nDebugger disconnected.\n");
					EndSession();
					state = QUIT_DETACH;
				}
				continue;
			}
			rx_len += n;
		}
		state = ProcessRx(rx, &rx_len, pkt, out);
	}

	if (client != INVALID_SOCKET)
		CloseSocket(client);
	CloseSocket(listener);
	free(rx);
	free(pkt);
	free(out);
	return (0);
}
