#include <libps.h>
#include <sys/ioctl.h>
#include "yario_emu.h"

// SIO1 (serial port) registers
#define SIO1_DATA   (*(volatile u_char *)0x1f801050)
#define SIO1_STAT   (*(volatile u_short *)0x1f801054)
#define SIO1_MODE   (*(volatile u_short *)0x1f801058)
#define SIO1_CTRL   (*(volatile u_short *)0x1f80105a)
#define SIO1_BAUD   (*(volatile u_short *)0x1f80105e)

#define STAT_TXRDY  0x0001
#define STAT_RXRDY  0x0002
#define STAT_CTS    0x0100

#define CTRL_TXEN   0x0001
#define CTRL_DTR    0x0002
#define CTRL_RXEN   0x0004
#define CTRL_ACK    0x0010
#define CTRL_RTS    0x0020
#define CTRL_RESET  0x0040

#define MODE_8N1_X16    0x4e        // 8 data bits, no parity, 1 stop bit, baud x16
#define SIO_CLOCK_X16   2116800     // 33868800 / 16
#define TX_TIMEOUT      100000      // polls to wait for the other side (CTS)

#define FILE_NBLOCK     0x0004      // file flag set by ioctl(fd, FIOCNBLOCK, 1)
#define DEVICE_CHAR     0x0001

// The BIOS's file and device structures
typedef struct {
    u_long flags, deviceId;
    u_char *buffer;
    u_long count, offset, deviceFlags, error;
    void *device;
    u_long length, lba, fd;
} KernelFile;

typedef struct {
    char *name;
    u_long flags, blockSize;
    char *desc;
    void (*init)();
    int (*open)();
    int (*action)();
    int (*close)();
    int (*ioctl)();
    int (*read)();
    int (*write)();
    int (*other[9])();              // erase ... check: unused for a tty
} KernelDevice;

// BIOS B0 calls: AddDevice (0x47) and DelDevice (0x48)
int YarioEmuAddDevice(KernelDevice *device);
int YarioEmuDelDevice(char *name);
asm(".text\n"
    ".set noreorder\n"
    "YarioEmuAddDevice:\n"
    "    li $10, 0xb0\n"
    "    jr $10\n"
    "    li $9, 0x47\n"
    "YarioEmuDelDevice:\n"
    "    li $10, 0xb0\n"
    "    jr $10\n"
    "    li $9, 0x48\n"
    ".set reorder\n");

static u_short ctrlBits = CTRL_TXEN | CTRL_DTR | CTRL_RXEN | CTRL_RTS;

static int TtyDummy(void) { return 0; }

static void TtyInit(void) {
    SIO1_CTRL = CTRL_RESET;
    SIO1_MODE = MODE_8N1_X16;
    SIO1_BAUD = SIO_CLOCK_X16 / 115200;
    SIO1_CTRL = ctrlBits;
}

static int TtyOpen(KernelFile *file, char *name, int mode) { return 0; }

static int TtyRead(KernelFile *file) {
    int n = 0;

    // Rewriting the control register sends our RTS/DTR to the other emulator
    // again (PCSX-Redux only sends them once it is connected)
    SIO1_CTRL = ctrlBits;

    while (n < (int)file->count) {
        if (SIO1_STAT & STAT_RXRDY) {
            file->buffer[n++] = SIO1_DATA;
        } else if (file->flags & FILE_NBLOCK) {
            break;                  // non-blocking: return what has arrived
        }
    }
    return n;
}

static int TtyWrite(KernelFile *file) {
    int n, wait;

    for (n = 0; n < (int)file->count; n++) {
        for (wait = 0; (SIO1_STAT & (STAT_TXRDY | STAT_CTS)) != (STAT_TXRDY | STAT_CTS); wait++) {
            if (wait == TX_TIMEOUT) return n;   // the other side isn't receiving
        }
        SIO1_DATA = file->buffer[n];
    }
    return n;
}

static int TtyAction(KernelFile *file, int action) {
    return (action == 1) ? TtyRead(file) : TtyWrite(file);
}

static int TtyIoctl(KernelFile *file, int cmd, int arg) {
    switch (cmd) {
        case FIOCNBLOCK:
            if (arg) file->flags |= FILE_NBLOCK;
            else file->flags &= ~FILE_NBLOCK;
            break;
        case TIOCBAUD:
            if (arg > 0) SIO1_BAUD = SIO_CLOCK_X16 / arg;
            break;
        case TIOCLEN:               // stop bits << 16 | character length
            SIO1_MODE = (SIO1_MODE & ~0xcc) | ((arg & 3) << 2) | (((arg >> 16) & 3) << 6);
            break;
        case TIOCPARITY:            // 0: none, 1: even, 3: odd
            SIO1_MODE = (SIO1_MODE & ~0x30) | ((arg & 1) << 4) | ((arg & 2) << 4);
            break;
        case TIOCDTR:
            ctrlBits = arg ? (ctrlBits | CTRL_DTR) : (ctrlBits & ~CTRL_DTR);
            SIO1_CTRL = ctrlBits;
            break;
        case TIOCRTS:
            ctrlBits = arg ? (ctrlBits | CTRL_RTS) : (ctrlBits & ~CTRL_RTS);
            SIO1_CTRL = ctrlBits;
            break;
        case TIOCFLUSH:
            while (SIO1_STAT & STAT_RXRDY) (void)SIO1_DATA;
            break;
        case TIOERRRST:
            SIO1_CTRL = ctrlBits | CTRL_ACK;
            break;
        default:                    // TIOCRAW, TIOCREOPEN, FIOCSCAN, ...: nothing to do
            break;
    }
    return 0;
}

static KernelDevice ttyDevice = {
    "tty", DEVICE_CHAR, 1, "YarIO emulator SIO1 tty",
    TtyInit, TtyOpen, TtyAction, TtyDummy, TtyIoctl, TtyDummy, TtyDummy,
    { TtyDummy, TtyDummy, TtyDummy, TtyDummy, TtyDummy, TtyDummy, TtyDummy, TtyDummy, TtyDummy }
};

// PS1 DEV CART ("-c sio" or "-c link") has already installed a Net Yaroze compatible tty driver (serial port or USB
// link): found by its description in the BIOS's device table (kernel address 0x150: the table, 0x154: its size)
static int CartTtyInstalled(void) {
    static const char cartDesc[] = "PS1 DEV CART SIO tty";
    KernelDevice *device = *(KernelDevice **)0x80000150;
    u_long size = *(u_long *)0x80000154;
    u_long i, j;

    for (i = 0; i + sizeof(KernelDevice) <= size; i += sizeof(KernelDevice), device++) {
        if (!device->name || !device->desc) continue;
        for (j = 0; cartDesc[j] && device->desc[j] == cartDesc[j]; j++);
        if (!cartDesc[j]) return 1;
    }
    return 0;
}

void YarioEmuInstall(void) {
    static int installed = 0;
    if (installed) return;
    installed = 1;

    // The cartridge's driver is kept: the same build runs in an emulator and on a PS1 DEV CART
    if (CartTtyInstalled()) return;

    EnterCriticalSection();
    YarioEmuDelDevice("tty");       // the BIOS's console
    YarioEmuAddDevice(&ttyDevice);  // copied into the BIOS's device table; calls TtyInit
    ExitCriticalSection();
}
