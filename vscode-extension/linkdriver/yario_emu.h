#ifndef YARIO_EMU_H
#define YARIO_EMU_H

/*
 * Emulator-only link support for YarIO (build with -DYARIO_EMU).
 *
 * On a Net Yaroze the boot disc's monitor installs its "SIO console" as the
 * BIOS's tty device, so reading and writing "tty:" uses the serial port.
 * A psx.exe made by yarexe starts the program without that monitor setup, so
 * in an emulator "tty:" is the BIOS's own console (which emulators redirect
 * to their log). YarioEmuInstall() replaces it with a small tty driver that
 * uses the serial port (SIO1) directly, so two emulators linked through
 * their SIO1 (PCSX-Redux: SIO1 server/client) exchange the tty data.
 */

/************* FUNCTION PROTOTYPES *******************/
void YarioEmuInstall(void);
/*****************************************************/

#endif // YARIO_EMU_H
