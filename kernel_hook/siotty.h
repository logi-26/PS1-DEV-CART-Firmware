/*
* PS1 DEV CART firmware
* Serial port (SIO) TTY driver, installed by the kernel hook with ps1transfer "-c sio"
*
* The Net Yaroze boot disc monitor installs its own "tty" console driver on the serial port,
* Net Yaroze programs use it with open("tty:"), read(), write() and the SIO ioctls of <sys/ioctl.h>
* (e.g. for link play between two consoles).
* This is a small polled driver compatible with these calls, so programs can do the same when
* executed by PS1 DEV CART. The receive FIFO is also emptied by the kernel hook on every exception,
* so less data is lost when the program doesn't read often enough.
*
* USB link ("-c link"): the same driver without the serial port, for link play through the PS1 DEV CART
* cartridges and the computer ("ps1transfer -relay"): the computer writes the received bytes to rx_buffer and reads
* the sent bytes from tx_buffer (SioBuffers, address in the kernel hook params). Each side writes only its half of
* SioBuffers, the computer in one USB transfer (atomic for the program: the kernel hook runs it in an exception).
* Ring positions are masked indexes (empty: head == tail, full: head + 1 == tail).
* Frame pacing (the timing of a serial cable): each write of the program is a "unit" (e.g. a YarIO packet), the
* computer passes the units on with their size, and the receiving driver gives the program one unit per frame (frames
* counted at the vertical blanks, when the kernel hook runs). Programs like YarIO keep only the last packet read in a frame: two
* packets arriving in the same frame (USB delivery isn't as regular as a cable) would lose one and desynchronise the
* consoles. A backlog of more than 1 unit is given at once (like the bytes waiting in a serial port buffer).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#define	SIO_DATA	*((volatile uint8_t *)0x1F801050)
#define	SIO_STAT	*((volatile uint16_t *)0x1F801054)
#define	SIO_MODE	*((volatile uint16_t *)0x1F801058)
#define	SIO_CTRL	*((volatile uint16_t *)0x1F80105A)
#define	SIO_BAUD	*((volatile uint16_t *)0x1F80105E)

#define	SIO_STAT_TX_READY	(1 << 0)
#define	SIO_STAT_RX_READY	(1 << 1)
#define	SIO_STAT_RX_ERRORS	((1 << 3) | (1 << 4) | (1 << 5))	// Parity, overrun, framing
#define	SIO_STAT_DSR		(1 << 7)
#define	SIO_STAT_CTS		(1 << 8)

#define	SIO_CTRL_TX_ENABLE	(1 << 0)
#define	SIO_CTRL_DTR		(1 << 1)
#define	SIO_CTRL_RX_ENABLE	(1 << 2)
#define	SIO_CTRL_ACK		(1 << 4)
#define	SIO_CTRL_RTS		(1 << 5)	// The other console can only send when its CTS (our RTS) is on
#define	SIO_CTRL_RESET		(1 << 6)

#define	SIO_MODE_STOP_MASK	(3 << 6)	// 1:1 2:1.5 3:2 stop bits
#define	SIO_MODE_PARITY_MASK	(3 << 4)	// bit 4: enable, bit 5: odd
#define	SIO_MODE_CHAR_MASK	(3 << 2)	// 0:5 1:6 2:7 3:8 bits
#define	SIO_MODE_DEFAULT	((1 << 6) | (3 << 2) | 2)	// 8N1, baud rate factor x16

#define	SIO_CLOCK			(33868800 / 16)
#define	SIO_BAUD_DEFAULT	115200
#define	SIO_RX_BUFFER_SIZE	256		// Power of 2
#define	LINK_TX_BUFFER_SIZE	64		// Power of 2, USB link only
#define	LINK_UNITS			8		// Power of 2, frame units of the USB link
#define	GPU_STAT			*((volatile uint32_t *)0x1F801814)
#define	GPU_STAT_ODD_LINE	(1 << 31)	// 0 during the vertical blank (and on even lines)
#define	SIO_TX_TIMEOUT		100000	// Avoid freezing printf when nothing is connected (CTS off)

// ioctl commands, same values as Net Yaroze and PsyQ <sys/ioctl.h>
#define	FIOCNBLOCK	(('f' << 8) | 1)	// set non-blocking io
#define	FIOCSCAN	(('f' << 8) | 2)	// scan for input
#define	TIOCRAW		(('t' << 8) | 1)	// disable xon/xoff control (and '\n' -> "\r\n" here)
#define	TIOCFLUSH	(('t' << 8) | 2)	// flush input buffer
#define	TIOCREOPEN	(('t' << 8) | 3)	// reopen
#define	TIOCBAUD	(('t' << 8) | 4)	// set baud rate
#define	TIOCDTR		(('t' << 8) | 6)	// control DTR line
#define	TIOCRTS		(('t' << 8) | 7)	// control RTS line
#define	TIOCLEN		(('t' << 8) | 8)	// stop << 16 | char
#define	TIOCPARITY	(('t' << 8) | 9)	// parity 0:none 1:even 3:odd
#define	TIOSTATUS	(('t' << 8) | 10)	// return status
#define	TIOERRRST	(('t' << 8) | 11)	// error reset
#define	TIOEXIST	(('t' << 8) | 12)	// exist test with DSR/CTS
#define	TIORLEN		(('t' << 8) | 13)	// receive buffer length

#define	SIO_EINVAL	22

typedef struct
{
	// Written by the computer (USB link), or rx_head by SIO_Poll (serial port)
	volatile uint32_t	rx_head;
	volatile uint32_t	tx_tail;
	volatile uint32_t	rx_unit_head;
	volatile uint32_t	tx_unit_tail;
	volatile uint16_t	rx_units[LINK_UNITS];		// Sizes of the units received
	volatile uint8_t	rx_buffer[SIO_RX_BUFFER_SIZE];
	// Written by the program (driver) and the kernel hook
	volatile uint32_t	rx_tail;
	volatile uint32_t	tx_head;
	volatile uint32_t	rx_unit_tail;
	volatile uint32_t	tx_unit_head;
	volatile uint16_t	tx_units[LINK_UNITS];		// End (tx_buffer position) of each unit sent; tx_unit_head: open unit
	volatile uint8_t	tx_buffer[LINK_TX_BUFFER_SIZE];
} SioBuffers;

SioBuffers					sio_buf;	// (global: the profiler's RAM area, see monitor/monitor.h)
#define	SIO_rx_buffer		sio_buf.rx_buffer
#define	SIO_rx_head			sio_buf.rx_head		// Head is only written by SIO_Poll (or the computer), tail by the reader
#define	SIO_rx_tail			sio_buf.rx_tail
#define	SIO_RX_HEAD			(sio_buf.rx_head & (SIO_RX_BUFFER_SIZE - 1))	// Read masked: a bad value from the computer
																		// (e.g. a transfer cut off) can't loop read()
static bool					SIO_link;			// USB link: no serial port
static volatile uint32_t	link_frame;			// Vertical blanks seen by the kernel hook (USB link)
static uint32_t				link_released;		// Frame of the last unit given to the program
static uint32_t				link_allow;			// Bytes of the released units not read yet
static volatile bool		link_open;			// The unit at tx_unit_head has bytes (not closed yet: unit ring full)
static volatile bool		SIO_is_busy;				// The kernel hook doesn't poll while the program uses the driver
static uint32_t				SIO_baud = SIO_BAUD_DEFAULT;
static uint16_t				SIO_mode = SIO_MODE_DEFAULT;
static uint16_t				SIO_ctrl = SIO_CTRL_TX_ENABLE | SIO_CTRL_RX_ENABLE | SIO_CTRL_RTS;
static bool					SIO_is_raw;
bool						is_sio_tty_enabled;

static void	SIO_Delay(void)
{
	volatile int	i;

	for (i = 0; i < 1500; i++)
		;
}

// Reset the serial port with the current settings (received data are lost)
static __attribute__((noinline)) void	SIO_Init(void)
{
	if (SIO_link)	// USB link: the computer writes rx_head, received data are dropped
	{
		SIO_rx_tail = SIO_RX_HEAD;
		sio_buf.rx_unit_tail = sio_buf.rx_unit_head & (LINK_UNITS - 1);
		link_allow = 0;
		return;
	}
	SIO_CTRL = SIO_CTRL_RESET | SIO_CTRL_ACK;
	SIO_Delay();
	SIO_BAUD = SIO_CLOCK / SIO_baud;
	SIO_MODE = SIO_mode;
	SIO_CTRL = SIO_ctrl;
	SIO_rx_head = SIO_rx_tail = 0;
}

// Move received bytes from the 8 bytes hardware FIFO to the receive buffer
static __attribute__((noinline)) void	SIO_Poll(void)
{
	uint16_t	stat;
	uint32_t	next;
	uint8_t		c;

	if (SIO_link)
		return;
	while ((stat = SIO_STAT) & (SIO_STAT_RX_READY | SIO_STAT_RX_ERRORS))
	{
		if (stat & SIO_STAT_RX_READY)
		{
			c = SIO_DATA;
			next = (SIO_rx_head + 1) & (SIO_RX_BUFFER_SIZE - 1);
			if (next != SIO_rx_tail)	// Byte is dropped if the buffer is full
			{
				SIO_rx_buffer[SIO_rx_head] = c;
				SIO_rx_head = next;
			}
		}
		if (stat & SIO_STAT_RX_ERRORS)
			SIO_CTRL = SIO_ctrl | SIO_CTRL_ACK;
	}
}

static uint32_t	SIO_RxLength(void)
{
	uint32_t	n = (SIO_RX_HEAD - SIO_rx_tail) & (SIO_RX_BUFFER_SIZE - 1);

	return ((SIO_link && (n > link_allow)) ? link_allow : n);
}

// USB link, at each run of the kernel hook: frames counted at the vertical blanks
// USB link: the open unit is closed if the unit ring has room (else it grows with the next writes)
static void	LINK_CloseUnit(void)
{
	uint32_t	next = (sio_buf.tx_unit_head + 1) & (LINK_UNITS - 1);

	if (link_open && (next != (sio_buf.tx_unit_tail & (LINK_UNITS - 1))))
	{
		sio_buf.tx_unit_head = next;
		link_open = false;
	}
}

static void	LINK_Tick(void)
{
	if (!(GPU_STAT & GPU_STAT_ODD_LINE))
		link_frame++;
	LINK_CloseUnit();
}

// USB link, before a read: one more unit for the program in a new frame (more if the backlog grows)
static void	LINK_Release(void)
{
	uint32_t	t = sio_buf.rx_unit_tail;

	if (link_released == link_frame)
		return;
	while (t != (sio_buf.rx_unit_head & (LINK_UNITS - 1)))
	{
		link_allow += sio_buf.rx_units[t];
		t = (t + 1) & (LINK_UNITS - 1);
		link_released = link_frame;
		if (((sio_buf.rx_unit_head - t) & (LINK_UNITS - 1)) <= 1)
			break;
	}
	sio_buf.rx_unit_tail = t;
}

static __attribute__((noinline)) bool	SIO_SendByte(uint8_t c)
{
	uint32_t	timeout = SIO_TX_TIMEOUT, next;

	if (SIO_link)	// USB link: byte dropped if the computer doesn't keep up
	{
		next = (sio_buf.tx_head + 1) & (LINK_TX_BUFFER_SIZE - 1);
		if (next == (sio_buf.tx_tail & (LINK_TX_BUFFER_SIZE - 1)))
			return (false);
		sio_buf.tx_buffer[sio_buf.tx_head] = c;
		sio_buf.tx_head = next;
		return (true);
	}
	while (!(SIO_STAT & SIO_STAT_TX_READY))
	{
		SIO_Poll();
		if (!(timeout--))
			return (false);
	}
	SIO_DATA = c;
	return (true);
}

// USB link: the kernel hook answers the computer only at some interrupts (about once per frame), so it also answers
// at each read/write of the program (with interrupts off, like in the exception handler): lower latency
static __attribute__((noinline)) void	LINK_Serve(void)
{
	uint32_t	sr;

	__asm__ volatile("mfc0 %0, $12" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr & ~1));
	if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
		USB_Process(0, 0, NULL, 0, NULL);
	__asm__ volatile("mtc0 %0, $12; nop; nop" : : "r"(sr));
}

int	SIO_tty_open(struct File *file, const char *filename, int mode)
{
	return (0);
}

int	SIO_tty_action(struct File *file, enum FileAction action)
{
	volatile uint8_t	*buf8 = (volatile uint8_t *)file->buffer;
	uint32_t			i = 0, tx_start = sio_buf.tx_head;
	bool				serve = SIO_link;

	// USB link: the computer is answered when a read finds no more bytes (bytes arriving now are read at once; a
	// blocking read waits for them this way) and after a write (bytes written now leave at once)
	SIO_is_busy = true;
	if (action == PSXWRITE)
	{
		for (; i < file->count; i++)
		{
			if (!SIO_is_raw && (buf8[i] == '\n') && !SIO_SendByte('\r'))
				break;
			if (!SIO_SendByte(buf8[i]))
				break;
		}
	}
	else
	{
		while (i < file->count)
		{
			SIO_Poll();
			if ((SIO_rx_tail != SIO_RX_HEAD) && (!SIO_link || link_allow))
			{
				buf8[i++] = SIO_rx_buffer[SIO_rx_tail];
				SIO_rx_tail = (SIO_rx_tail + 1) & (SIO_RX_BUFFER_SIZE - 1);
				if (SIO_link)
					link_allow--;
			}
			else if (serve)		// (once for a non-blocking read, until it has its bytes for a blocking one)
			{
				LINK_Serve();
				LINK_Release();
				serve = !(file->flags & PSXF_NBLOCK);
			}
			else if (file->flags & PSXF_NBLOCK)
				break;
		}
	}
	SIO_is_busy = false;
	if (SIO_link && (action == PSXWRITE))
	{
		// The bytes written are a unit, closed at once (or at the next vertical blank if the unit ring is full).
		// Units are given by their end position: the bytes put in the buffer (a '\r' is added before each '\n' when
		// not raw) always match.
		if (sio_buf.tx_head != tx_start)
		{
			sio_buf.tx_units[sio_buf.tx_unit_head] = sio_buf.tx_head;
			link_open = true;
			LINK_CloseUnit();
		}
		LINK_Serve();
	}

	return (i);
}

int	SIO_tty_ioctl(struct File *file, int cmd, int arg)
{
	int	ret = 0, len;

	SIO_is_busy = true;
	switch (cmd)
	{
		case FIOCNBLOCK:
			if (arg)
				file->flags |= PSXF_NBLOCK;
			else
				file->flags &= ~PSXF_NBLOCK;
		break;

		case FIOCSCAN:
		case TIORLEN:
			SIO_Poll();
			ret = SIO_RxLength();
		break;

		case TIOCRAW:
			SIO_is_raw = (arg != 0);
		break;

		case TIOCFLUSH:
			SIO_Poll();
			SIO_rx_tail = SIO_RX_HEAD;
		break;

		case TIOCREOPEN:
			SIO_Init();
		break;

		case TIOCBAUD:
			if (arg <= 0)
				goto error;
			SIO_baud = arg;
			SIO_Init();
		break;

		case TIOCDTR:
		case TIOCRTS:
			if (arg)
				SIO_ctrl |= (cmd == TIOCDTR) ? SIO_CTRL_DTR : SIO_CTRL_RTS;
			else
				SIO_ctrl &= ~((cmd == TIOCDTR) ? SIO_CTRL_DTR : SIO_CTRL_RTS);
			SIO_CTRL = SIO_ctrl;
		break;

		case TIOCLEN:	// Char length is accepted as 0..3 (documented) or 5..8 (number of bits)
			len = arg & 0xF;
			if (len >= 5)
				len -= 5;
			if (len > 3)
				goto error;
			SIO_mode &= ~(SIO_MODE_STOP_MASK | SIO_MODE_CHAR_MASK);
			SIO_mode |= (len << 2) | ((((arg >> 16) & 3) ? ((arg >> 16) & 3) : 1) << 6);
			SIO_Init();
		break;

		case TIOCPARITY:
			SIO_mode = (SIO_mode & ~SIO_MODE_PARITY_MASK) | ((arg & 3) << 4);
			SIO_Init();
		break;

		case TIOSTATUS:
			ret = SIO_STAT;
		break;

		case TIOERRRST:
			SIO_CTRL = SIO_ctrl | SIO_CTRL_ACK;
		break;

		case TIOEXIST:
			ret = (SIO_STAT & (SIO_STAT_DSR | SIO_STAT_CTS)) != 0;
		break;

		default:
			goto error;
	}
	SIO_is_busy = false;
	return (ret);

error:
	SIO_is_busy = false;
	file->errno = SIO_EINVAL;
	return (-1);
}

struct Device	SIO_tty_dev =
{
	"tty",
	PSXDTTYPE_CHAR | PSXDTTYPE_CONS,
	0,
	"PS1 DEV CART SIO tty",
	(device_init)SIO_Init,
	SIO_tty_open,
	SIO_tty_action,
	(device_close)USB_tty_dummy,
	SIO_tty_ioctl,
	(device_read)USB_tty_dummy,
	(device_write)USB_tty_dummy,
	USB_tty_dummy,
	USB_tty_dummy,
	(device_firstFile)USB_tty_dummy,
	(device_nextFile)USB_tty_dummy,
	(device_format)USB_tty_dummy,
	USB_tty_dummy,
	USB_tty_dummy,
	(device_deinit)USB_tty_dummy,
	USB_tty_dummy
};

// Replace the BIOS TTY device (stdin/stdout) by the serial port driver, like the Net Yaroze boot disc
// (link: USB link instead of the serial port)
void	SetupSIOtty(bool link)
{
	SIO_link = link;
	SIO_Init();
	ReplaceTTY(&SIO_tty_dev);
	is_sio_tty_enabled = true;
}

// Called by the kernel hook on every exception
static inline void	SIO_tty_Update(void)
{
	if (is_sio_tty_enabled && SIO_link)
		LINK_Tick();
	if (is_sio_tty_enabled && !SIO_is_busy)
		SIO_Poll();
}
