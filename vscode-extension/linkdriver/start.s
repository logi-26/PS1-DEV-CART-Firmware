# The link driver's entry, at its load address (DRIVER_ADDR in the Makefile): installs yario_emu.c's serial port tty

	.text
	.set	noreorder
	.globl	_start
_start:
	j	YarioEmuInstall
	nop
