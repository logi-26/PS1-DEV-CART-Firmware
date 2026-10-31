/*
* PS1 DEV CART debug monitor
* Definitions shared by the kernel hook, the monitor, the firmware and ps1transfer
*
* The kernel hook (kernel RAM, nearly full) only holds the entry points: exception vectors, register save area,
* stack, and the state below at a fixed address. The monitor itself (crash screen, stopped program loop) is a
* separate binary executed from the cartridge flash (or from RAM for tests), see monitor.c.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _MONITOR_H_
#define _MONITOR_H_

#define	MONITOR_STATE_ADRS	0x8000DF00	// MonitorState (kernel hook RAM, see kernel_hook/ps-exe_noheader_kernel_hook.ld)
#define	MONITOR_MAGIC		0x31474244	// "DBG1": the kernel hook supports the debug monitor
#define	MONITOR_ROM_ADRS	0x1F019000	// Monitor in the cartridge flash (after the menu images, see cartrom/rom.s)
#define	MONITOR_RAM_ADRS	0x801C0000	// Monitor copied to RAM by a firmware test build running from RAM

// Why the program stopped (MonitorState.reason)
#define	MON_REASON_NONE		0
#define	MON_REASON_BREAK	1	// Break instruction (software breakpoint), exception vector 0x80000080
#define	MON_REASON_DEBUG	2	// COP0 hardware breakpoint/watchpoint, debug vector 0x80000040
#define	MON_REASON_PAUSE	3	// Pause asked by the computer ('P' byte while the program runs)
#define	MON_REASON_CRASH	4	// Unresolved exception (BIOS A(40h)), e.g. bus error, or a break before the vectors are set
#define	MON_REASON_SERVICE	5	// 'M' byte while the program runs: one command (e.g. hot reload), then the program goes on
#define	MON_REASON_PROFILE	6	// Profiler on (MonitorState.prof_on): called at each ReturnFromException, samples at the timer ticks
#define	MON_REASON_PAD_SETUP	7	// Remote pad (EXEC_PAD): called by the firmware before it executes the program
#define	MON_REASON_SIO_SETUP	8	// Serial port / USB link tty driver ("-c sio", "-c link"; regs = 1: USB link) installed
#define	MON_REASON_SIO_UPDATE	9	// Its work at each exception (kernel_hook/siotty.h: the driver is in the monitor)
#define	MON_REASON_CD_SETUP	10	// Disc booted by the firmware: Load and LoadExec of "cdrom:" files by the monitor (see below)

// Execute command param (tty mode byte): debug session, the hook installs the exception vectors
#define	EXEC_DEBUG			0x80

// Commands of the stopped program loop, besides 'D'/'U' memory, 'V' VRAM and 'R' reset (same packet as usb.h)
#define	MON_CMD_CONTINUE	'C'	// Resume the program (cache flushed, COP0 debug registers applied)
#define	MON_CMD_PAUSE		'P'	// Single byte sent to the running program: stop it (debug session only)
#define	MON_CMD_SERVICE		'M'	// Single byte sent to the running program: the monitor runs the command packet that follows
#define	MON_CMD_VRAM_WRITE	'W'	// param = x << 16 | y, size = w << 16 | h: VRAM write, MON_VRAM_LINES() lines per packet
#define	MON_CMD_SPU_WRITE	'S'	// param = SPU RAM address, size = bytes: SPU RAM write, MON_SPU_PACKET bytes per packet
#define	MON_CMD_GPU_INFO	'X'	// size = 16: the GPU's draw area top left, bottom right, drawing offset and version (GP1
								// 10h info 3, 4, 5, 7), 4 words: the frame capture (monitor version 8)
#define	MON_CMD_SPU_READ	'Y'	// param = SPU RAM address (multiple of 64), size = bytes (multiple of 64): SPU RAM read
								// (DMA), MON_SPU_PACKET bytes per packet; param MON_SPU_REGS: the SPU registers
								// (0x1F801C00-0x1F801DFF, size MON_SPU_REGS_SIZE) instead (monitor version 10)
#define	MON_CMD_EXCHANGE	'Z'	// param = address, size = write size << 16 | read size: the USB link exchange (the kernel
								// hook's 'X': written at the address, then what follows read), by the kernel hook and
								// the monitor (also while the program is stopped, monitor version 12)
#define	MON_SPU_REGS		0xFFFFFFFF
#define	MON_SPU_REGS_SIZE	512
#define	MON_SPU_RAM_SIZE	0x80000

// Lines of a VRAM write packet (at most 2KB), SPU write packet size
#define	MON_VRAM_LINES(_w_)	((2048 / ((_w_) * 2)) ? (2048 / ((_w_) * 2)) : 1)
#define	MON_SPU_PACKET		1024

// Monitor version (header word 2, copied in MonitorState.version): 1 = 'M' service, VRAM/SPU write;
// 2 = live screenshots ('V') only through the 'M' service (the kernel hook doesn't have them any more);
// 3 = profiler (MON_REASON_PROFILE); 4 = escaped answers ('Q' before a command, see usb.h); 5 = crash screen drawn in
// the program's draw area, its last frame kept in VRAM (MON_CRASH_FRAME); 6 = the kernel hook ends with the
// ReturnFromException stored at 0x8000DF7C (psxdevcart: the hook run from a BIOS interrupt handler, PsyQo programs);
// 7 = profiler: GPUSTAT of the program in MonitorState.prof_gpustat (video mode: PAL/NTSC frame budget);
// 8 = remote pad (EXEC_PAD, MonitorState.pad_buf), GPU info (MON_CMD_GPU_INFO), the serial port / USB link tty
// driver (moved from the kernel hook: MON_REASON_SIO_SETUP, MON_REASON_SIO_UPDATE);
// 9 = remote pad: player 2 too (MonitorState.pad_buf2, the active port in pad_port, both reusing profiler fields:
// see the remote pad comment below); 10 = SPU RAM and registers read (MON_CMD_SPU_READ: the SPU RAM viewer);
// 11 = remote pad devices: mouse, NeGcon, analog pads, GunCon (PadDevice); 12 = the USB link exchange also while the
// program is stopped (MON_CMD_EXCHANGE: the relay of a debug session, ps1transfer -gdb -relay); 13 = Load and LoadExec
// of "cdrom:" files for a disc the firmware booted (MON_REASON_CD_SETUP, see below)
#define	MONITOR_VERSION		13

// Disc booted by the firmware (MON_REASON_CD_SETUP, before it executes the game): the BIOS CD driver isn't there
// (PSn00bSDK removes it, and the firmware reads the disc with polled drive commands, see cdread.h), so a game loading
// another executable through the BIOS (Load A(42h), LoadExec A(51h), e.g. Fighting Force after its loading screen)
// got garbage. The monitor points both A0 table entries to its own code (in flash): a "cdrom:" file is read the
// firmware's way, then LoadExec goes on to the BIOS Exec; another device goes to the BIOS Load, kept in
// MonitorState.pad_orig (the remote pad's: only set up for an executable the computer runs, never for a disc). The
// hooks stay for the executables that follow.

// Profiler ("ps1transfer -prof"). A root counter not used by the program interrupts it (rate set by the computer):
// the exception vector goes to the monitor's sampler (copied to RAM), which sends a sample to the computer at each tick, without waiting (dropped if the USB FIFO is full), between
// the printf text: Data Link Escape (16), a type byte, then the payload:
//	'p'	sample: sequence (1), PROF_F_* flags (1), PC (4), RA (4), SP & 0xFFFFFF (3), draw area changes (1)
//	'v'	every 32 samples: current envelope volume of the 24 SPU voices (24 x 2), ENDX (4), SPUCNT (2)
//	'm'	end of a RAM scan pass (only if the firmware filled the free RAM before executing the program): words written
//		by the program since it started (4), longest run of untouched words (4)
//	'c'	the program reprogrammed the profiler's root counter (counter number, 1): profiling stops
// The computer reads the packets that arrive before the answer to its commands (see ps1transfer/profiler.c).
#define	PROF_SAMPLE			'p'
#define	PROF_VOICES			'v'
#define	PROF_MEMORY			'm'
#define	PROF_CONFLICT		'c'
#define	PROF_SAMPLE_SIZE	14
#define	PROF_VOICES_SIZE	(24 * 2 + 4 + 2)
#define	PROF_MEMORY_SIZE	8
#define	PROF_CONFLICT_SIZE	1
#define	PROF_VOICES_EVERY	32		// Samples between two calls of the monitor in flash: 'v' packet, RAM scan
#define	PROF_SCAN_WORDS		2048	// RAM words checked at each of these calls (a 1.7MB pass: about 7 seconds at 1kHz)
#define	PROF_AREA			672		// Sampler copied to the kernel hook's monitor stack and register save area that
									// follows it (MON_STACK_SIZE + MON_REGS * 4, only used by debug sessions: no
									// profiling then), the saved registers and the packet at the end
#define	PROF_FREE_PATTERN	0x45455246	// "FREE": free RAM filled by the firmware (Execute with EXEC_PROF_FILL)

// Sample flags (state of the hardware at the tick)
#define	PROF_F_GPU_BUSY		(1 << 0)	// GPU busy: drawing (GPUSTAT bit 26 = 0) or its DMA running
#define	PROF_F_DMA_GPU		(1 << 1)	// DMA channel 2 (GPU: ordering table, LoadImage...)
#define	PROF_F_DMA_SPU		(1 << 2)	// DMA channel 4
#define	PROF_F_DMA_CD		(1 << 3)	// DMA channel 3
#define	PROF_F_DMA_MDEC		(1 << 4)	// DMA channels 0/1
#define	PROF_F_DMA_OTC		(1 << 5)	// DMA channel 6 (ordering table clear)
#define	PROF_F_GPU_CMD		(1 << 6)	// GPU executing a command (GPUSTAT bit 26 = 0)
#define	PROF_F_CD_BUSY		(1 << 7)	// CD-ROM controller busy with a command

// Execute command param (tty mode byte): the firmware fills the free RAM above the program with PROF_FREE_PATTERN
// (not kept in KernelHookParams.tty_mode)
#define	EXEC_PROF_FILL		0x40

// Remote pad ("ps1transfer -pad"): either controller port driven from the computer, for programs reading the pads
// with the BIOS (InitPAD/StartPAD: PsyQ's PadInit, Net Yaroze, PSn00bSDK's InitPAD). Execute command param bit (not
// kept in tty_mode): the firmware calls the monitor (MON_REASON_PAD_SETUP), which points the BIOS function InitPAD
// (B0 table entry 0x12) to its own code: the buffers of both ports are kept (pad_buf: port 1, pad_buf2: port 2, see
// below), then the BIOS InitPAD is called (pad_orig). The kernel hook then calls the monitor at each exception
// (prof_on set, prof_armed PROF_ARMED_PAD): when the computer has pressed buttons (prof_seq bit 0), they are added to
// the buffer of the port currently selected (pad_port, 0 or 1; the computer's "padport" command, see
// ps1transfer/memlive.c) the BIOS has filled (a digital pad: status 0, ID 0x41, buttons active low), prof_target
// holding them (PsyQ pad bits: PADLup...). Only one port is driven at a time (switching is instant: the next frame
// uses the new port's buffer); a real controller on the other port keeps working normally.
// No profiling meanwhile (the same fields): pad_buf2 reuses prof_draw, pad_port reuses prof_frames (monitor version
// 9; an older monitor only drives port 1 and doesn't keep pad_buf2/pad_port, see ps1transfer/memlive.c).
// Devices (monitor version 11): the computer chooses the controller the program sees (PadDevice.type, its ID byte,
// buf[1]); the whole reply is written then: buttons (prof_target: ~(buf[2] << 8 | buf[3])), then 4 bytes: analog
// pads RX RY LX LY, NeGcon twist I II L, GunCon X and Y (16-bit), mouse the motion of the frame (dx, dy: from the
// totals of the computer, each given once, when the BIOS has filled the buffer again: our reply overwritten).
#define	EXEC_PAD			0x20
#define	PROF_ARMED_PAD		5
#define	PAD_DIGITAL			0x41
#define	PAD_MOUSE			0x12
#define	PAD_NEGCON			0x23
#define	PAD_ANALOG_JOY		0x53
#define	PAD_ANALOG			0x73
#define	PAD_GUNCON			0x63

// Stop notification sent by the PS1 (like a PCDRV request): 'B', param = regs address, size = reason << 8 | ExcCode
#define	MON_NOTIFY_STOP		'B'

// Program registers saved when stopped, same layout as the BIOS TCB registers
#define	MON_REG_EPC			32
#define	MON_REG_HI			33
#define	MON_REG_LO			34
#define	MON_REG_SR			35
#define	MON_REG_CAUSE		36
#define	MON_REGS			40

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef struct
{
	uint32_t	magic;		// MONITOR_MAGIC
	uint32_t	entry;		// Monitor entry, set by the firmware: void Monitor(int reason, uint32_t *regs)
	uint32_t	regs;		// Registers of the stopped program (MON_REG_* layout), 0 while it runs
	uint8_t		reason;		// MON_REASON_* of the stop
	uint8_t		debug;		// Debug session: exception vectors installed
	uint8_t		version;	// MONITOR_VERSION of the monitor (0: first version, no 'M' service), set by the firmware
	uint8_t		fw_version;	// Firmware version, major << 4 | minor (0x01 = 0.1, 0: unknown), set by the firmware
	uint32_t	bpc, bpcm, bda, bdam, dcic;	// COP0 debug registers applied when the program resumes (set by the computer)
	uint32_t	dcic_stop;	// DCIC status bits (0-5) when the program stopped: which hardware breakpoint triggered
	uint32_t	chain80[4];	// Original code of the exception vector 0x80000080 (jump to the BIOS exception handler)
	uint32_t	link_buffers;	// SioBuffers of the kernel hook's tty driver (USB link "-c link", ps1transfer -relay)
	// Profiler (monitor version 3), set by the computer unless noted
	uint8_t		prof_on;		// Root counter used + 1, 0 = off
	uint8_t		prof_seq;		// Sample sequence number (monitor)
	uint8_t		prof_frames;	// Draw area changes (monitor): frames drawn by a double buffered program
	uint8_t		prof_armed;		// 0: set up at the next exception, 1: running, 2: counter used by the program (stop),
								// 3: stop (computer), 4: SPU voices and RAM scan due
	uint16_t	prof_target;	// Root counter target and mode
	uint16_t	prof_mode;
	uint32_t	prof_draw;		// Last draw area (monitor); after a crash (crash screen shown): MON_CRASH_FRAME
	uint32_t	ram_lo, ram_hi;	// Free RAM filled with PROF_FREE_PATTERN by the firmware (Execute), 0 = not filled
	uint32_t	ram_pos, ram_dirty, ram_run, ram_maxrun;	// RAM scan (monitor)
	uint32_t	prof_call;		// (unused since the sampler handles the ticks itself: set by older ps1transfer)
	uint32_t	prof_gpustat;	// GPUSTAT at the last SPU voices packet (monitor version 7, 0: not known)
	// Remote pad (monitor version 8, EXEC_PAD)
	uint32_t	pad_buf;		// Pad buffer of port 1 given to InitPAD by the program, 0: not called yet
	uint32_t	pad_orig;		// The BIOS InitPAD; for a disc booted by the firmware: the BIOS Load (MON_REASON_CD_SETUP)
} MonitorState;

// Remote pad device (monitor version 11): in MonitorState.ram_lo... (the profiler's RAM scan, unused meanwhile)
typedef struct
{
	uint8_t		type;		// PAD_*: the ID byte of the reply, 0: digital pad (buttons added to a real pad's)
	uint8_t		reserved;
	int16_t		mouse_x, mouse_y;	// Mouse: motion totals (computer)
	int16_t		given_x, given_y;	// Mouse: motion given to the program (monitor)
	uint8_t		axes[4];	// Reply bytes 4-7
} PadDevice;
#define	MON_STATE_PAD_DEVICE	72	// offsetof(MonitorState, ram_lo)
#endif

// MonitorState.prof_draw after a crash (monitor version 5): the crash screen is drawn in the program's draw area
// (GPU info 3, bits 0-19: x 0-9, y 10-19), the program's display mode (GPUSTAT bits 16-22) in bits 20-26
#define	MON_CRASH_FRAME		0x80000000

#define	MON_STATE_ENTRY		4	// Offsets used by the assembly code
#define	MON_STATE_CHAIN80	40
#define	MON_STATE_PAD_BUF	104
#define	MON_STATE_PAD_ORIG	108
#define	MON_STATE_PAD_BUF2	68	// Port 2's pad buffer (monitor version 9): reuses prof_draw, see the remote pad comment

#endif
