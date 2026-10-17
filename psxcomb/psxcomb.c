/*
* PS1 DEV CART psxcomb: link cable library for PSn00bSDK, see psxcomb.h
*
* Serial link (same signals as the PlayStation link cable library, so it can talk to a game that uses it):
* - 8 bits, no parity, 2 stop bits, 9600 bps by default; one byte per handshake by default (CombSetPacketSize).
* - Receiving: the serial interrupt takes the bytes of a packet, then toggles DTR: that is the "next packet" signal
*   for the other console. RTS is on while a read is waiting.
* - Sending: a byte is sent only once DSR (the other console's DTR) has toggled since the previous one. The original
*   library waits for this in the transmit interrupt, which it then disables, so a write could stop for ever when the
*   acknowledge came late. Here a root counter 2 interrupt checks the line while a write is in progress (about twice
*   per byte time, at least every 40us): no interrupt storm, no lost write.
* - Errors (parity, overrun, framing) reset the controller and cancel the read (event EvSpERROR).
*
* USB link (CombSetLink(COMB_LINK_USB)): two rings in RAM, relayed by "ps1transfer -n SERIAL1 -relay SERIAL2"
* (format in ps1-tools/linktest/usblink.h, their address published at 0x8000DF38 for the relay). A 1kHz root counter 2
* interrupt keeps the kernel hook answering the relay, and ends the reads/writes: a write ends once all its bytes are
* in the ring (it waits while the ring is full), a read once all its bytes have arrived.
*
* Root counter 2 is used while a serial write is in progress, or all the time with the USB link.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psxcomb.h"

// Serial port (SIO1) registers
#define	SIO1_DATA		(*(volatile uint8_t *)0x1F801050)
#define	SIO1_STATUS		(*(volatile uint32_t *)0x1F801054)
#define	SIO1_MODE		(*(volatile uint16_t *)0x1F801058)
#define	SIO1_CONTROL	(*(volatile uint16_t *)0x1F80105A)
#define	SIO1_BAUD		(*(volatile uint16_t *)0x1F80105E)
#define	CTRL_TXEN		0x0001
#define	CTRL_DTR		0x0002
#define	CTRL_RXEN		0x0004
#define	CTRL_ACK		0x0010		// Acknowledge the interrupt and the errors
#define	CTRL_RTS		0x0020
#define	CTRL_RESET		0x0040
#define	CTRL_RXLEVEL	0x0300		// Receive interrupt after 1/2/4/8 bytes
#define	CTRL_TXIEN		0x0400
#define	CTRL_RXIEN		0x0800
#define	STAT_ERRORS		(COMB_PERROR | COMB_OE | COMB_FE)
#define	BAUD_CLOCK		2073600		// Baud reload = BAUD_CLOCK / rate (x16 mode)

// Root counter 2: system clock / 8
#define	T2_VALUE		(*(volatile uint16_t *)0x1F801120)
#define	T2_MODE			(*(volatile uint16_t *)0x1F801124)
#define	T2_TARGET		(*(volatile uint16_t *)0x1F801128)
#define	T2_RUN			0x0258		// Reset and interrupt at target, repeat, system clock / 8
#define	T2_TICKS_PER_MS	4234

// Kernel hook of the cartridge firmware: USB service, link pointer for the relay
#define	HOOK_USB_PROCESS	(*(int (**)(uint8_t, uint32_t, uint8_t *, uint32_t, uint32_t *))0x8000DF74)
#define	HOOK_LINK_POINTER	(*(volatile uint32_t *)0x8000DF38)
#define	USB_STATUS			(*(volatile uint8_t *)0x1F060000)
#define	USB_READY			(1 << 2)	// Active low
#define	USB_NO_DATA			(1 << 0)	// Nothing received from the computer

// USB link rings (ps1-tools/linktest/usblink.h: magic, version, indexes, ring size, then the tx and rx rings)
#define	USBLINK_MAGIC		0x4B4E4C55	// "ULNK"
#define	USBLINK_VERSION		1
#ifndef COMB_USB_RING
#define	COMB_USB_RING		4096		// Bytes per direction, power of 2
#endif

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	uint32_t	tx_head;		// Written by the PS1 after the bytes
	uint32_t	tx_tail;		// Written by the relay once it has taken them
	uint32_t	rx_head;		// Written by the relay after the bytes
	uint32_t	rx_tail;		// Written by the PS1 once it has read them
	uint32_t	ring_size;
	uint32_t	reserved;
	uint8_t		tx[COMB_USB_RING];
	uint8_t		rx[COMB_USB_RING];
} CombRings;

typedef struct
{
	volatile int		busy;
	uint8_t				*volatile ptr;
	volatile int32_t	left;
	volatile uint32_t	expect;	// Write: DSR level that allows the next byte
} Transfer;

typedef long	(*WaitFunc)(long rw, long count);

static Transfer		rd, wr;
static uint16_t		sh_mode = 0xCE, sh_ctrl = CTRL_TXEN | CTRL_RXEN, sh_baud = BAUD_CLOCK / 9600;
static int			packet = 1;			// Bytes per handshake
static WaitFunc		wait_cb;
static int			installed, link = COMB_LINK_SERIAL, timer_on;
static uint32_t		old_link_pointer;
static CombRings	rings;

static void	TimerUpdate(void);

/****************************************/
// Kernel hook and USB link

// Commands of the computer waiting in the USB FIFO, a few at most (interrupts off)
static void	HookService(void)
{
	int	(*f)(uint8_t, uint32_t, uint8_t *, uint32_t, uint32_t *) = HOOK_USB_PROCESS;
	int	i;

	if ((uint32_t)f - 0x8000C000 >= 0x2000)
		return;
	for (i = 0; (i < 4) && !(USB_STATUS & (USB_READY | USB_NO_DATA)); i++)
		f(0, 0, NULL, 0, NULL);
}

static void	UsbAttach(void)
{
	memset(&rings, 0, offsetof(CombRings, tx));
	rings.version = USBLINK_VERSION;
	rings.ring_size = COMB_USB_RING;
	old_link_pointer = HOOK_LINK_POINTER;
	HOOK_LINK_POINTER = (uint32_t)&rings;
	rings.magic = USBLINK_MAGIC;
}

static void	UsbDetach(void)
{
	rings.magic = 0;
	if (HOOK_LINK_POINTER == (uint32_t)&rings)
		HOOK_LINK_POINTER = old_link_pointer;
}

// Received bytes to the read in progress, bytes to send to the ring, events when they end (interrupts off)
static void	UsbPoll(void)
{
	volatile CombRings	*r = &rings;

	if (rd.busy)
	{
		while (rd.left && (r->rx_head != r->rx_tail))
		{
			*rd.ptr++ = r->rx[r->rx_tail & (COMB_USB_RING - 1)];
			r->rx_tail++;
			rd.left--;
		}
		if (!rd.left)
		{
			rd.busy = 0;
			DeliverEvent(HwSIO, EvSpIOER);
		}
	}
	if (wr.busy)
	{
		while (wr.left && (r->tx_head - r->tx_tail < COMB_USB_RING))
		{
			r->tx[r->tx_head & (COMB_USB_RING - 1)] = *wr.ptr++;
			r->tx_head++;		// After the byte
			wr.left--;
		}
		if (!wr.left)		// All in the ring (a game can write the next packet at once, as each frame)
		{
			wr.busy = 0;
			DeliverEvent(HwSIO, EvSpIOEW);
		}
	}
}

/****************************************/
// Serial port

static void	SioRestart(int keep_read)
{
	uint16_t	dtr = SIO1_CONTROL & CTRL_DTR;	// (the handshake level is kept)

	SIO1_CONTROL = CTRL_RESET | CTRL_ACK;
	SIO1_MODE = sh_mode;
	SIO1_BAUD = sh_baud;
	SIO1_CONTROL = (sh_ctrl & ~(CTRL_DTR | CTRL_TXIEN | (keep_read ? 0 : (CTRL_RXIEN | CTRL_RTS)))) | dtr;
	SIO1_CONTROL |= CTRL_ACK;
}

// Next byte of the write in progress once the other console has acknowledged the previous one (interrupts off)
static void	TxPoll(uint32_t stat)
{
	if (!wr.busy || !(stat & COMB_TXRDY) || ((stat & COMB_DSR) != wr.expect))
		return;
	if (wr.left <= 0)		// Last byte acknowledged
	{
		wr.busy = 0;
		TimerUpdate();
		DeliverEvent(HwSIO, EvSpIOEW);
		return;
	}
	wr.expect ^= COMB_DSR;
	SIO1_DATA = *wr.ptr++;
	wr.left--;
}

static void	SioIrq(void)
{
	uint32_t	stat = SIO1_STATUS;
	int			i;

	if (link != COMB_LINK_SERIAL)
	{
		SIO1_CONTROL = (SIO1_CONTROL & ~(CTRL_RXIEN | CTRL_TXIEN)) | CTRL_ACK;
		return;
	}
	if (stat & STAT_ERRORS)	// The read is cancelled, the controller reset
	{
		rd.busy = 0;
		rd.left = 0;
		SioRestart(0);
		DeliverEvent(HwSIO, EvSpERROR);
		return;
	}
	if (rd.busy && (stat & COMB_RXRDY))
	{
		for (i = 0; i < packet; i++)
		{
			uint8_t	c = SIO1_DATA;

			if (rd.left > 0)
			{
				*rd.ptr++ = c;
				rd.left--;
			}
		}
		if (rd.left <= 0)
		{
			rd.busy = 0;
			SIO1_CONTROL &= ~(CTRL_RXIEN | CTRL_RTS);
			DeliverEvent(HwSIO, EvSpIOER);
		}
		SIO1_CONTROL ^= CTRL_DTR;	// Acknowledge: the other console sends the next packet
	}
	TxPoll(SIO1_STATUS);
	SIO1_CONTROL = (SIO1_CONTROL & ~CTRL_TXIEN) | CTRL_ACK;
}

/****************************************/
// Root counter 2 interrupt: serial write in progress (line checked about twice per byte), USB link (1kHz)

static void	TimerIrq(void)
{
	if (link == COMB_LINK_USB)
	{
		HookService();
		UsbPoll();
	}
	else
		TxPoll(SIO1_STATUS);
}

static void	TimerUpdate(void)		// (interrupts off)
{
	int			want = installed && ((link == COMB_LINK_USB) || wr.busy);
	uint32_t	ticks;

	if (want == timer_on)
		return;
	timer_on = want;
	if (!want)
	{
		InterruptCallback(IRQ_TIMER2, NULL);
		T2_MODE = 0;
		return;
	}
	if (link == COMB_LINK_USB)
		ticks = T2_TICKS_PER_MS;
	else	// Half the time of a byte (11 bits), at least 40us
	{
		ticks = (uint32_t)sh_baud * 16 * 11 / 2 / 8;	// System clock / 8 ticks: baud reload * 16 per bit
		if (ticks < T2_TICKS_PER_MS * 40 / 1000)
			ticks = T2_TICKS_PER_MS * 40 / 1000;
	}
	T2_MODE = 0;
	T2_TARGET = (ticks > 0xFFFF) ? 0xFFFF : ticks;
	T2_MODE = T2_RUN;
	InterruptCallback(IRQ_TIMER2, TimerIrq);
}

/****************************************/
// Reads and writes

static long	CallWait(long rw, long *count)	// 0: stop (wait callback), else go on waiting
{
	if (wait_cb && !wait_cb(rw, (*count)++))
	{
		DeliverEvent(HwSIO, EvSpTIMOUT);
		return (0);
	}
	return (1);
}

static long	AsyncRead(uint8_t *buf, long len)
{
	if (rd.busy)
		return (-1);
	EnterCriticalSection();
	rd.ptr = buf;
	rd.left = len;
	rd.busy = 1;
	if (len <= 0)
	{
		rd.busy = 0;
		DeliverEvent(HwSIO, EvSpIOER);
	}
	else if (link == COMB_LINK_USB)
		UsbPoll();
	else
		SIO1_CONTROL |= CTRL_RXIEN | CTRL_RTS;
	ExitCriticalSection();
	return (0);
}

static long	AsyncWrite(uint8_t *buf, long len)
{
	if (wr.busy)
		return (-1);
	EnterCriticalSection();
	wr.ptr = buf;
	wr.left = len;
	wr.expect = SIO1_STATUS & COMB_DSR;
	wr.busy = 1;
	if (link == COMB_LINK_USB)
		UsbPoll();
	else
	{
		TimerUpdate();
		TxPoll(SIO1_STATUS);
	}
	ExitCriticalSection();
	return (0);
}

static long	SyncRead(uint8_t *buf, long len)
{
	long		left = len, n = 0, count = 0;
	uint32_t	stat;

	if (link == COMB_LINK_USB)
	{
		while (1)
		{
			EnterCriticalSection();
			HookService();
			while (left && (rings.rx_head != rings.rx_tail))
			{
				*buf++ = rings.rx[rings.rx_tail & (COMB_USB_RING - 1)];
				rings.rx_tail++;
				left--;
			}
			ExitCriticalSection();
			if (!left || !CallWait(1, &count))
				return (len - left);
		}
	}
	SIO1_CONTROL |= CTRL_RTS;
	while (left > 0)
	{
		stat = SIO1_STATUS;
		if (stat & STAT_ERRORS)
		{
			SioRestart(0);
			DeliverEvent(HwSIO, EvSpERROR);
			break;
		}
		if (!(stat & COMB_RXRDY))
		{
			if (!CallWait(1, &count))
				break;
			continue;
		}
		*buf++ = SIO1_DATA;
		left--;
		if (++n == packet)
		{
			n = 0;
			SIO1_CONTROL ^= CTRL_DTR;
		}
	}
	SIO1_CONTROL &= ~CTRL_RTS;
	return (len - left);
}

static long	SyncWrite(uint8_t *buf, long len)
{
	long		left = len, n = 0, count = 0;
	uint32_t	expect = 0;

	if (link == COMB_LINK_USB)
	{
		while (1)
		{
			EnterCriticalSection();
			HookService();
			while (left && (rings.tx_head - rings.tx_tail < COMB_USB_RING))
			{
				rings.tx[rings.tx_head & (COMB_USB_RING - 1)] = *buf++;
				rings.tx_head++;
				left--;
			}
			n = !left && (rings.tx_tail == rings.tx_head);
			ExitCriticalSection();
			if (n || !CallWait(2, &count))
				return (len - left);
		}
	}
	while (left > 0)
	{
		while ((SIO1_STATUS & (COMB_TXRDY | COMB_TXU)) != (COMB_TXRDY | COMB_TXU))
			if (!CallWait(2, &count))
				return (len - left);
		if (!n)
			expect = SIO1_STATUS & COMB_DSR;
		SIO1_DATA = *buf++;
		left--;
		if (++n == packet)	// Packet sent: wait for the other console's acknowledge
		{
			n = 0;
			while ((SIO1_STATUS & COMB_DSR) == expect)
				if (!CallWait(2, &count))
					return (len - left);
		}
	}
	return (len - left);
}

/****************************************/
// "sio" device

static int	DevInit(void)
{
	rd.busy = wr.busy = 0;
	rd.left = wr.left = 0;
	wait_cb = NULL;
	SioRestart(0);
	SIO1_CONTROL = sh_ctrl;
	InterruptCallback(IRQ_SIO1, SioIrq);
	return (0);
}

static int	DevOpen(FCB *fcb, const char *name, int mode)
{
	return (0);
}

static int	DevInout(FCB *fcb, int cmd)
{
	uint8_t	*buf = (uint8_t *)fcb->trns_addr;
	long	len = fcb->trns_len, r;
	int		async = fcb->status & O_NOWAIT;

	if (cmd == 1)
		r = async ? AsyncRead(buf, len) : SyncRead(buf, len);
	else if (cmd == 2)
		r = async ? AsyncWrite(buf, len) : SyncWrite(buf, len);
	else
	{
		fcb->lasterr = 22;		// EINVAL
		return (-1);
	}
	if (async && r)
	{
		fcb->lasterr = 16;		// EBUSY: a read/write of this kind is already in progress
		return (-1);
	}
	return (r);
}

static int	DevClose(FCB *fcb)
{
	return (0);
}

static int	DevNothing(void)
{
	return (0);
}

static DCB	dev =
{
	"sio", 3, 1, "SIO console",
	DevInit, DevOpen, DevInout, DevClose, DevNothing, DevNothing, DevNothing, DevNothing, DevNothing, DevNothing,
	DevNothing, DevNothing, DevNothing, DevNothing, DevNothing, DevNothing
};

void	AddCOMB(void)
{
	if (installed)
		return;
	EnterCriticalSection();
	AddDrv(&dev);
	DevInit();		// (also when the BIOS doesn't call it)
	installed = 1;
	if (link == COMB_LINK_USB)
		UsbAttach();
	TimerUpdate();
	ExitCriticalSection();
}

void	DelCOMB(void)
{
	if (!installed)
		return;
	EnterCriticalSection();
	DelDrv("sio");
	installed = 0;
	rd.busy = wr.busy = 0;
	SIO1_CONTROL &= ~(CTRL_RXIEN | CTRL_TXIEN | CTRL_RTS);
	InterruptCallback(IRQ_SIO1, NULL);
	TimerUpdate();
	UsbDetach();
	ExitCriticalSection();
}

void	ChangeClearSIO(long mode)
{
}

/****************************************/

static long	Status(void)
{
	long	s;

	if (link != COMB_LINK_USB)
		return (SIO1_STATUS & 0xFFFF);
	s = COMB_TXRDY | COMB_TXU | COMB_DSR | COMB_CTS;
	if (rings.rx_head != rings.rx_tail)
		s |= COMB_RXRDY;
	return (s);
}

static long	SetLink(unsigned long l)
{
	if ((l != COMB_LINK_SERIAL) && (l != COMB_LINK_USB))
		return (-1);
	EnterCriticalSection();
	rd.busy = wr.busy = 0;
	rd.left = wr.left = 0;
	SIO1_CONTROL &= ~(CTRL_RXIEN | CTRL_TXIEN | CTRL_RTS);
	if ((l == COMB_LINK_USB) && (link != COMB_LINK_USB) && installed)
		UsbAttach();
	if ((l != COMB_LINK_USB) && (link == COMB_LINK_USB))
		UsbDetach();
	link = l;
	InterruptCallback(IRQ_TIMER2, NULL);	// Timer started again with the period of the new link
	T2_MODE = 0;
	timer_on = 0;
	TimerUpdate();
	ExitCriticalSection();
	return (0);
}

long	_comb_control(unsigned long cmd, unsigned long sub, unsigned long arg)
{
	long	r = 0;

	switch (cmd)
	{
		case 0:		// Information
			switch (sub)
			{
				case 0:	return (Status());
				case 1:	return (((SIO1_CONTROL & CTRL_DTR) ? COMB_BIT_DTR : 0) | ((SIO1_CONTROL & CTRL_RTS) ? COMB_BIT_RTS : 0));
				case 2:	return (sh_mode);
				case 3:	return (BAUD_CLOCK / sh_baud);
				case 4:	return (packet);
				case 5:	return (arg ? rd.left : wr.left);
				case 6:	return (arg ? rd.busy : wr.busy);
			}
			break;
		case 1:		// Settings
			switch (sub)
			{
				case 1:
					sh_ctrl = (SIO1_CONTROL & ~(CTRL_DTR | CTRL_RTS)) | ((arg & COMB_BIT_DTR) ? CTRL_DTR : 0) |
						((arg & COMB_BIT_RTS) ? CTRL_RTS : 0);
					SIO1_CONTROL = sh_ctrl;
					return (0);
				case 2:
					sh_mode = arg;
					SIO1_MODE = sh_mode;
					return (0);
				case 3:
					if (!arg || (BAUD_CLOCK % arg))
						return (-1);
					sh_baud = BAUD_CLOCK / arg;
					SIO1_BAUD = sh_baud;
					SIO1_CONTROL |= CTRL_ACK;
					EnterCriticalSection();
					if (timer_on && (link == COMB_LINK_SERIAL))	// (check period of the new rate)
					{
						timer_on = 0;
						InterruptCallback(IRQ_TIMER2, NULL);
						TimerUpdate();
					}
					ExitCriticalSection();
					return (0);
				case 4:
				{
					uint16_t	level;

					switch (arg)
					{
						case 1:	level = 0x000;	break;
						case 2:	level = 0x100;	break;
						case 4:	level = 0x200;	break;
						case 8:	level = 0x300;	break;
						default:	return (-1);
					}
					packet = arg;
					sh_ctrl = (SIO1_CONTROL & ~CTRL_RXLEVEL) | level;
					SIO1_CONTROL = sh_ctrl;
					return (level);
				}
			}
			break;
		case 2:		// Reset, cancel
			switch (sub)
			{
				case 0:
					EnterCriticalSection();
					SioRestart(rd.busy);
					ExitCriticalSection();
					return (0);
				case 1:
					SIO1_CONTROL |= CTRL_ACK;
					return (0);
				case 2:
					EnterCriticalSection();
					wr.busy = 0;
					wr.left = 0;
					TimerUpdate();
					ExitCriticalSection();
					return (0);
				case 3:
					EnterCriticalSection();
					SIO1_CONTROL &= ~(CTRL_RXIEN | CTRL_RTS);
					rd.busy = 0;
					rd.left = 0;
					ExitCriticalSection();
					return (0);
			}
			break;
		case 3:		// RTS / CTS
			if (sub == 0)
			{
				if (arg)
					SIO1_CONTROL |= CTRL_RTS;
				else
					SIO1_CONTROL &= ~CTRL_RTS;
				return (0);
			}
			if (sub == 1)
				return ((link == COMB_LINK_USB) || (SIO1_STATUS & COMB_CTS)) ? 1 : 0;
			break;
		case 4:		// Wait callback of the synchronous read/write
			if (sub == 0)
			{
				r = (long)wait_cb;
				wait_cb = (WaitFunc)arg;
				return (r);
			}
			break;
		case 5:
			return (0);
		case 6:		// Link (PS1 DEV CART)
			if (sub == 0)
				return (link);
			if (sub == 1)
				return (SetLink(arg));
			break;
	}
	return (r);
}
