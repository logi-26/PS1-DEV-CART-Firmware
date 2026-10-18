/*
* PS1 DEV CART psxpcdrv: files of the computer used by a PlayStation program, over the cartridge USB link (PCDRV)
*
* The computer side is ps1transfer in console mode ("ps1transfer -e -f game.exe -c", files in the folder given with
* -p, else the current folder). The functions have the usual PCDRV names, so programs written for a PCDRV file server
* build unchanged: libsn_usb.h and libsn.h in this folder include this file.
* PSn00bSDK (psxapi) and PsyQo/nugget (common/kernel/pcdrv.h) have their own PCDRV functions (break instructions,
* for emulators and debuggers), so the functions of this library are renamed USB_PC... by the macros below: programs
* keep using the PC... names (don't include nugget's pcdrv.h in the same source file).
* Plain C, no SDK library: works with PSn00bSDK, PsyQ and PsyQo (C++: the declarations are extern "C").
* Needs the PS1 DEV CART firmware (kernel hook) and a program started from the cartridge (PsyQo: without
* takeOverKernel(), which removes the kernel hook).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PSXPCDRV_H_
#define _PSXPCDRV_H_

#define	PCinit	USB_PCinit
#define	PCopen	USB_PCopen
#define	PCcreat	USB_PCcreat
#define	PClseek	USB_PClseek
#define	PCread	USB_PCread
#define	PCwrite	USB_PCwrite
#define	PCclose	USB_PCclose

// PCopen modes
#define	PCDRV_READ			0		// Existing file, read only
#define	PCDRV_WRITE			1		// File created or emptied, write only
#define	PCDRV_READWRITE		2		// Existing file, read and write

// PClseek origins
#define	PCDRV_SEEK_SET		0		// From the start of the file
#define	PCDRV_SEEK_CUR		1		// From the current position
#define	PCDRV_SEEK_END		2		// From the end of the file

#if defined(__cplusplus)
extern "C" {
#endif

// Checks that the computer answers (ps1transfer in console mode): 0, or -1 if it doesn't
int	PCinit(void);

// Opens a file of the computer (mode: PCDRV_READ/WRITE/READWRITE, perms unused): file number, or -1
int	PCopen(char *name, int mode, int perms);

// Creates a file, or empties an existing one, and opens it for reading and writing (perms unused): file number, or -1
int	PCcreat(char *name, int perms);

// Moves in a file (origin: PCDRV_SEEK_SET/CUR/END): new position from the start of the file, or -1
int	PClseek(int fd, int offset, int origin);

// Reads up to len bytes (any size): number of bytes read (less at the end of the file), or -1
int	PCread(int fd, char *buf, int len);

// Writes len bytes (any size): number of bytes written, or -1
int	PCwrite(int fd, char *buf, int len);

// Closes a file: 0, or -1
int	PCclose(int fd);

#if defined(__cplusplus)
}
#endif

#endif
