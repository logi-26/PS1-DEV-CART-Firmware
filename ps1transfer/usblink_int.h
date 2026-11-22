/*
* PS1 DEV CART Transfer tool: what the USB link files share (usblink.c was split: usblink_vlink.c,
* usblink_net.c, usblink_xserve.c). The functions other files of ps1transfer use are in usblink.h.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_USBLINK_INT_H_
#define _PS1TRANSFER_USBLINK_INT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET	sock_t;
#define	CloseSocket	closesocket
#else
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
typedef int		sock_t;
#define	INVALID_SOCKET	(-1)
#define	CloseSocket	close
#endif

#include "usblink.h"
#include "../ps1-tools/linktest/usblink.h"

/****************************************/
// Constants

// usblink.c
// Rings: at the address published at LINK_BUFFERS_ADRS (psxcomb USB link), else at USBLINK_ADRS (link tester)
#define	LINK_FIELD(_e_, _f_)	((_e_)->base + offsetof(UsbLink, _f_))

#define	LINK_TX(_e_)			((_e_)->base + offsetof(UsbLink, tx))

#define	LINK_RX(_e_)			((_e_)->base + offsetof(UsbLink, tx) + (_e_)->ring)

#define	RELAY_CHUNK			4096		// Bytes copied at most per direction and turn

// Kernel hook params (0x8000DF70): is_enabled, tty_mode, ...; MonitorState.link_buffers (0x8000DF38): SioBuffers
#define	HOOK_PARAMS_ADRS	0x8000DF70

#define	LINK_BUFFERS_ADRS	0x8000DF38

#define	TTY_MODE_LINK		3

// tty driver buffers of the kernel hook (SioBuffers in kernel_hook/siotty.h): masked ring positions, and frame units
// (what the program wrote during one frame, given to the other program one unit per frame like a serial cable)
#define	TTY_RX_SIZE			256

#define	TTY_TX_SIZE			64

#define	TTY_UNITS			8

#define	TTY_PC_PART			(16 + TTY_UNITS * 2 + TTY_RX_SIZE)	// Written by the computer

#define	TTY_PS1_PART		(16 + TTY_UNITS * 2 + TTY_TX_SIZE)	// Written by the PS1

// Computer part: rx_head, tx_tail, rx_unit_head, tx_unit_tail, rx_units[], rx_buffer[]
#define	PC_RX_HEAD(_e_)			U32((_e_)->pc)

#define	PC_TX_TAIL(_e_)			U32((_e_)->pc + 4)

#define	PC_RX_UNIT_HEAD(_e_)	U32((_e_)->pc + 8)

#define	PC_TX_UNIT_TAIL(_e_)	U32((_e_)->pc + 12)

#define	PC_RX_UNIT(_e_, _i_)	(*(uint16_t *)((_e_)->pc + 16 + (_i_) * 2))

#define	PC_RX_BUF(_e_)			((_e_)->pc + 16 + TTY_UNITS * 2)

// PS1 part: rx_tail, tx_head, rx_unit_tail, tx_unit_head, tx_units[], tx_buffer[]
#define	PS1_RX_TAIL(_e_)		U32((_e_)->ps1)

#define	PS1_TX_HEAD(_e_)		U32((_e_)->ps1 + 4)

#define	PS1_RX_UNIT_TAIL(_e_)	U32((_e_)->ps1 + 8)

#define	PS1_TX_UNIT_HEAD(_e_)	U32((_e_)->ps1 + 12)

#define	PS1_TX_UNIT(_e_, _i_)	(*(uint16_t *)((_e_)->ps1 + 16 + (_i_) * 2))	// End position of a unit sent

#define	PS1_TX_BUF(_e_)			((_e_)->ps1 + 16 + TTY_UNITS * 2)

#define	U32(_p_)	(*(uint32_t *)(_p_))

// Rings read/written with the kernel hook's exchange ('X'), not D/U: one USB write that the PS1 never waits in for the
// computer (D/U wait for its acknowledgements, the program stopped meanwhile: a few milliseconds each, the program's
// frames slowed by each turn). A packet received wrong: transferred again.
#define	XFER_PACKET		2048		// (MAX_PACKET_SIZE: the exchange's packets)

#define	QUEUE_SIZE	4096	// Power of 2

#define	QUEUE_UNITS	256		// Power of 2

#define	NET_CHUNK	4096		// Bytes moved at most per direction and turn

// usblink_vlink.c
#define	VL_BUF		0x8000D810

#define	VL_RX		128

#define	VL_TX		128

#define	VL_LN		16

#define	VL_MAGIC	0x4B4E4C56

#define	VL_PC		36				// rx_head, tx_tail, ln_head, credit_in, abort, ln_ring[16] (after rx[])

#define	VL_PC_ADRS	(VL_BUF + 4 + VL_RX)

#define	VL_ABORT_MS	1500			// A console waiting that long with nothing moving for it: its wait ended (abort)

#define	VL_WAIT_SEEN_MS	200

#define	VL_GONE_MS	100				// Then (the other side gone, until something comes from it): its next waits sooner,

#define	VL_PS1		40				// rx_tail, tx_head, ln_tail, my_lines, saved, peer_lines, peer_seen, credit_used,

// The console's part read with as much of its send area as needed, so that the answer ('G', the written packet's CRC,
// the bytes, their CRC) fills whole USB packets (62 bytes of data each for the FT245R): a last partial packet waits for
// the latency timer (1 ms). 59, 121 or 183 bytes (the last: 15 bytes after the area, its stats: not used)
#define	VL_USB_DATA	62

#define	VL_PS1_READ_MAX	(3 * VL_USB_DATA - 3)

#define	VL_STREAM	65536			// Power of 2: records on their way to the other console

#define	VL_FRAME	0x801B4EB8		// The game's frame counter (25 a second)

// Each game frame both consoles exchange a 6-byte header (BD DA len AF DE) twice, the second always the same as the
// first, then the payload: 6 one-way messages, each crossing this computer. The reader's next 6-byte read after a
// header is answered at once with a copy of it (its request still goes to the writer as usual), and the writer's
// second header, when it comes, is dropped (checked equal): nearly 4 crossings a frame instead of 6. Only on request:
// given in advance (BILL's first version), a copy nobody asked for (leaving a link game) shifted every message after
// it, and the game, reading garbage, wrote over its own interrupt callbacks.
#define	VL_COPY_MS	200				// A copy kept that long for the reader's request

// The payloads too (VLINK_TUNE "pcopy 1"): every message is sent twice, identical (header, header, payload, payload:
// traced 2026-11-13, 860 of 860 pairs equal, orders too). The reader's request for as many bytes as the message it was
// just given is answered with a copy, and the writer's repeat dropped when it comes (checked equal: dup_bad otherwise)
#define	VL_COPY_MAX	96

#define	VL_IS_HEADER(_p_)	(((_p_)[0] == 0xBD) && ((_p_)[1] == 0xDA) && ((_p_)[4] == 0xAF) && ((_p_)[5] == 0xDE))

#define	VL_MARKS	8192			// Power of 2

// Trace (VLINK_TUNE "trace S": S seconds of events, then written to vlink_trace.txt in the current folder): each
// exchange and record with its time. Kinds: X exchange ended (a = receive bytes given in it, b = the console's waiting
// flag), D data written by the console (a = length), C a read's request (a = bytes), K a request given to the console
// (a = bytes), H a header copy given
#define	VL_TRACE_MAX	400000

#define	VL_HOLD_MS	500

// The contents of the data records written by each console while tracing (vlink_trace.txt "M" lines: hex bytes)
#define	VL_MSG_MAX		60000

#define	VL_MSG_BYTES	96

#define	VL_IN_AVAIL(_e_)		((_e_)->in->head - (_e_)->in->tail)

#define	VL_IN_BYTE(_e_, _i_)	((_e_)->in->buf[((_e_)->in->tail + (_i_)) & (VL_STREAM - 1)])

// The symmetric exchange (vlink.c vlink_free_write), only during a game (VLINK_TUNE "sym 1"): the game's exchange
// function also carries the setup screen's last step, which deadlocked with both consoles writing first. So its two
// instructions (VLINK_SYM_WORDS: "adrs:orig:new,..." from usb_link.py) are written into both games while both
// consoles write in-game headers (none at the setup screen), and the originals back as soon as either stops. A frame
// with one console switched and the other not can't deadlock (the switched one's write goes out unasked).
#define	VL_SYM_WORDS	4

#define	VL_STALL_MS		30000		// (see VlPatience)

// Net Play's automatic link play: the Manager starts the game's link play on the console (no button pressed there),
// and the host's game with the settings chosen in the Manager. Two lists of steps from the game's patch package, each
// run on each console of this computer when asked in VLINK_TUNE (acted on when N changes):
//   "auto N V0 ... V15"      VLINK_AUTO_STEPS (-vlinkauto): the link play started (Red Alert: main menu -> LINK GAME
//                            -> country and color -> START SETUP: the console waits on the link page, its RTS up:
//                            "VLINK OFFER");
//   "hostauto N V0 ... V15"  VLINK_HOST_STEPS (-vlinkhostauto): the host's part once paired (Red Alert: the setup
//                            screen showing, the game settings written into its items, a short wait, START).
// The values: the game's options chosen in the Manager, in the order of its patch package (up to 16; missing: 0).
// Steps separated by ',', each a kind letter then its words separated by '/', ADDR=VALUE in hex (VALUE "$0" to "$15":
// the values given):
//   w  wait until all its words hold (read together);
//   a  arm: written (written back to 0 if the run fails: a driver's one-shot request, e.g. "choose LINK GAME");
//   s  write;
//   d  wait: "dMS" (hex milliseconds).
// A run always starts at the first step (not at a later "w" already holding: the game leaves a page's values in memory
// after it, Red Alert's link page "showing" at its main menu, and a second run wrote START into a page not there), and
// fails after VL_AUTO_MS (Red Alert's start: the game not at its main menu: the attract movie, a game running...).
#define	VL_AUTO_STEPS	16

#define	VL_AUTO_WORDS	12

#define	VL_AUTO_MS		60000

#define	VL_AUTO_VARS	16

#define	VL_AUTO_LISTS	3			// 0 the start, 1 the host's part, 2 kept (VlKeep: each second, not asked)

#define	VL_AUTO_RUNS	2			// (the lists run when asked)

// Words of the game watched once a second, each change printed ("VLINK BEN: watch main menu 0 -> 1000"): where the
// game is (its menus), to follow a failed start in the log. VLINK_WATCH (-vlinkwatch, from the game's patch
// package): "ADDR=name,ADDR=name,..." (hex addresses, up to VL_WATCH_MAX)
#define	VL_WATCH_MAX	8

// usblink_net.c
// -vlinkhost PORT / -vlinkjoin HOST:PORT: the virtual link cable between two computers, each with its own cartridge
// (Net Play): this computer's half carries its console as -vlink does, the other console is represented by a VlEnd
// kept up to date from the other half's status messages (in a game, symmetric exchange on, lines, frame counter), so
// that all of -vlink's logic (copies, double delivery, the symmetric switch, line holding, waits ended) runs as it is.
// TCP frames: type ('H' hello, 'R' records, 'S' status), length (2 bytes), data.
// The host carries its console from the start, while it waits for the other player: what the console sends is
// dropped until then (at record boundaries), and a console holding RTS for VL_OFFER_MS with nobody connected is
// waiting on its link page ("VLINK OFFER": a game offered, for the Manager's lobby; "VLINK WITHDRAW" when it stops).
#define	VL_NET_VERSION	"VLNK 1"

#define	VL_NET_STATUS_MS	100

#define	VL_OFFER_MS		300				// (longer than the link handshake's one-frame RTS pulse)

/****************************************/
// Types

// usblink.c
enum	{END_RINGS, END_TTY};

typedef struct
{
	void		*handle;
	char		*name;
	int			kind;
	uint32_t	bytes;				// Received this second
	// END_RINGS: free-running indexes, rings read/written with D/U
	UsbLink		h;
	uint32_t	base, ring;			// Address of the rings header, ring size
	uint32_t	tx_tail;			// Next byte to read from its send ring
	uint32_t	rx_head;			// Next byte to write in its receive ring
	bool		tx_tail_changed, rx_head_changed;
	// END_TTY: the computer's half kept here and written at once, the PS1's half read at once
	uint32_t	tty_adrs;
	uint8_t		pc[TTY_PC_PART];
	uint8_t		ps1[TTY_PS1_PART];
	bool		pc_changed;
} LinkEnd;

typedef struct
{
	uint8_t				buf[QUEUE_SIZE];
	volatile uint32_t	head, tail;		// Free-running
	uint16_t			units[QUEUE_UNITS];
	volatile uint32_t	uhead, utail;	// Free-running
} Queue;

typedef struct
{
	LinkEnd			*e;
	Queue			*in, *out;			// Bytes for this console, bytes from it
	volatile int	failed;
	volatile uint32_t	turns;
} TtyThread;

typedef bool	(*ExchangeFn)(void *handle, uint32_t adrs, uint8_t *wdata, uint32_t wsize, uint8_t *rdata,
	uint32_t rsize);

// usblink_vlink.c
typedef struct
{
	uint8_t				buf[VL_STREAM];
	volatile uint32_t	head, tail;
	// Timing (VTIME lines): the first records put in while none were stamped, when they came from the console
	// Network delay simulated (VLINK_TUNE "delay_us N"): where the stream's head was when (marks written as records
	// go in); the other side only takes what went in delay_us ago or more (ok_pos)
	volatile uint32_t	mk_pos[VL_MARKS];
	volatile uint64_t	mk_us[VL_MARKS];
	volatile uint32_t	mk_head, mk_tail, ok_pos;
	volatile int		stamped;
	volatile uint32_t	stamp_pos;
	volatile uint64_t	stamp_us;
} VlStream;

// Timing, per console, reset each second (VTIME lines): exchanges served while it waited in the driver (its waiting
// flag up in the answer) or not (another driver call or an interrupt), each one's time; crossings: records taken from
// the other console until the exchange that gave them to this one ended
typedef struct
{
	uint32_t	n, max;
	uint64_t	sum;
} VlTime;

typedef struct
{
	uint32_t	us;
	uint8_t		end, kind;
	uint16_t	a, b;
} VlEv;

typedef struct
{
	uint32_t	us;
	uint8_t		end;
	uint16_t	len;
	uint8_t		data[VL_MSG_BYTES];
} VlMsg;

typedef struct VlEnd_s
{
	void				*handle;
	char				*name;
	uint8_t				area[VL_RX + VL_PC], *pc;	// The receive area (its last rx_new bytes to be written) and the
	uint8_t				ps1[VL_PS1_READ_MAX];		// computer's counters; the console's counters and send area
	uint32_t			rx_new;			// Bytes put at the end of the receive area for the next exchange
	uint32_t			ps1_read;		// Bytes read of the console's part in the last exchange
	VlStream			*in, *out;		// Records for this console, from it
	uint32_t			data_left;		// Bytes of the data record being put in its receive ring
	volatile int		failed, reset;
	volatile uint32_t	turns, bytes_in, bytes_out, frame, aborts;
	volatile uint32_t	frame_ms;		// Last time its game frame counter was seen moving (in a game: payload copies)
	int					copy_hdr;		// The copy kept is a header's
	uint32_t			moved_ms;		// Last time something was carried for or from it (abort)
	int					gone;			// Its last wait ended by an abort, nothing from the other side since
	// Second headers (see VL_IS_HEADER): the other's last header (copy_pending: this console may ask for its copy;
	// copy_used: given, the other's next header is dropped), this console's records parsed as they go out
	int					out_state, out_need, out_credit;
	// Early credit for the responder's second header (VLINK_TUNE "fake 1"): each frame the responder reads the
	// initiator's first header, writes its own, reads the second (given the copy here at once), then writes its second
	// header, which is dropped (the other side gets a copy). That write waited for the other's request (credit), which
	// comes only once the other has the first header: ~8 ms a frame. So: when this console gets the copy having written
	// one header (the responder), it gets 6 bytes of credit at once (owed: taken from the other's next request), and the
	// other console drops its next header (drop_hdr: before its copy is asked for, h2_gone then; or after, copy_used)
	struct VlEnd_s		*peer;
	int					out_len, out_pos, hdrs_written;
	uint8_t				out_msg[VL_MSG_BYTES];		// (trace: the data record's bytes)
	uint8_t				out_hdr[6];
	uint32_t			hdr_ms, owed_ms, drop_ms;
	int32_t				owed;
	volatile int		drop_hdr;
	int					h2_gone;
	int					held_line;		// The other's last line change held back (-1 none): see vl_hold
	int					sym_on;			// The symmetric exchange's instructions written in its game (see VlSym)
	int					net;			// -vlinkhost / -vlinkjoin: the other console is on another computer
	uint32_t			fps;			// Its game frames in the last second (the other computer's: its status)
	// Double delivery during the symmetric exchange: every in-game message is sent twice, identical; each one is given
	// with its copy at once (the console's two reads served by one crossing) and the sender's repeat dropped: 2
	// network delays a frame instead of 4 (measured 15.7 frames a second at 10 ms one-way with 4). Started at a
	// frame's first header (after a payload), so as to stay on the pairs.
	int					dbl_on, dbl_wait, last_rx_len;
	uint32_t			dbl_len;
	uint8_t				dbl_msg[VL_COPY_MAX];
	uint32_t			held_ms;
	int					copy_pending, copy_used;
	uint32_t			copy_ms;
	uint8_t				in_hdr[VL_COPY_MAX];	// The last message given to it (a copy of it may be asked for)
	uint32_t			copy_len;
	volatile uint32_t	dup_ok, dup_bad;
	int					pc_dirty;		// Its part changed since the last exchange (to be written at once)
	uint64_t			wait_us;		// (the same in microseconds: pacing)
	uint32_t			wait_ms;		// Last time it was seen waiting (its flag is up only during half of its USB
										// services in a wait: "waiting" = seen in the last VL_WAIT_SEEN_MS)
	uint64_t			deliver_us;		// Timing: stamp of records given in the coming exchange (0 none)
	int					id;				// 0 the first console, 1 the other (trace)
	volatile VlTime		t_wait, t_other, t_cross;
	// Timing of its reads that waited (asked the other side for bytes): from their request taken here until the
	// exchange that gave the last of those bytes ended (the time the console sits in the driver for the other side)
	uint64_t			req_us;
	int32_t				req_left;
	int					req_done;
	volatile VlTime		t_read;
	// Net Play's automatic link play (see VlAuto): the run asked last, the step reached, when it started, last check
	int					stall_said;		// "the other console is busy" printed for this wait (see VlPatience)
	uint32_t			watch_val[8];	// (VlWatch: the game's words last read, VL_WATCH_MAX)
	uint8_t				watch_seen[8];
	unsigned int		auto_seen[2];	// (VL_AUTO_LISTS)
	int					auto_on, auto_step, auto_armed, auto_list;
	uint32_t			auto_ms, auto_check_ms, auto_until;
} VlEnd;

typedef struct
{
	char		kind;
	int			n;
	uint32_t	adrs[VL_AUTO_WORDS], val[VL_AUTO_WORDS];
	signed char	var[VL_AUTO_WORDS];		// the value used ($0-$15), -1 the value written in the step
} VlAutoStep;

/****************************************/
// Globals

// usblink.c
extern volatile int stop_requested;
// The queues' lock (both consoles' threads, the network threads)
#ifdef _WIN32
extern CRITICAL_SECTION	queue_lock;
#define	LOCK()		EnterCriticalSection(&queue_lock)
#define	UNLOCK()	LeaveCriticalSection(&queue_lock)
#else
extern pthread_mutex_t	queue_lock;
#define	LOCK()		pthread_mutex_lock(&queue_lock)
#define	UNLOCK()	pthread_mutex_unlock(&queue_lock)
#endif

// usblink_vlink.c
extern volatile int vl_trace_on, vl_fake;
extern volatile uint64_t vl_trace_until, vl_trace_t0;
extern volatile int vl_lobby, vl_paired;

/****************************************/
// Functions

// usblink.c
#ifdef _WIN32
BOOL WINAPI	OnInterrupt(DWORD type);
#else
void	OnInterrupt(int sig);
#endif
bool	StopRequested(void);
void	NetSleepMs(int ms);
sock_t	NetConnect(const char *host_port);
bool	NetReadable(sock_t s, int wait);
bool	NetSendAll(sock_t s, const uint8_t *buf, uint32_t n);

// usblink_vlink.c
uint32_t	VlMs(void);
uint64_t	VlUs(void);
uint32_t	VlPatience(VlEnd *e, uint32_t now);
#ifdef _WIN32
DWORD WINAPI	VlThreadMain(LPVOID arg);
#else
void	*VlThreadMain(void *arg);
#endif
bool	VlStart(VlEnd *e);
void	VlTraceWrite(void);
void	VlTimePrint(VlEnd *e);

// usblink_net.c
bool	VlNetThere(void);
bool	VlNetRecvAll(sock_t s, uint8_t *buf, uint32_t n);
bool	VlNetSend(int type, const uint8_t *data, uint32_t n);
void	VlNetTick(VlEnd *e);

#endif
