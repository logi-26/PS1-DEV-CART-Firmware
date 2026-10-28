# The loader the extension adds to the end of a game's psx.exe for link play in the emulator (Run Linked, Debug
# Linked; the game itself is unchanged): the PS-X EXE's entry point is moved here. It copies the link driver to its
# address in kernel RAM (the table after this code, filled in by the extension), clears the rest of its room,
# flushes the instruction cache, installs it (the BIOS "tty" device is then the serial port, like the Net Yaroze
# boot disc's), and starts the game at its own entry point. Position independent: it runs wherever the game ends.
#
# Table (4 words, then the driver's bytes): driver address, driver size, size to clear (from the driver address),
# the game's entry point.
#
# (Registers by number, as this assembler wants: $4/$5 a0/a1, $8-$15 t0-t7, $16-$20 s0-s4, $25 t9, $31 ra)

	.text
	.set	noreorder
	.globl	loader
loader:
	move	$16, $4			# kept for the game: the arguments of its start, its return address
	move	$17, $5
	move	$18, $31
	bal	1f
	nop
1:	addiu	$25, $31, table - 1b	# $25: the table
	lw	$19, 0($25)		# driver address
	lw	$9, 4($25)		# driver size
	lw	$10, 8($25)		# size to clear
	lw	$20, 12($25)		# the game's entry point
	addiu	$11, $25, 16		# the driver's bytes

	move	$12, $19		# clear
	addu	$13, $19, $10
2:	sw	$0, 0($12)
	addiu	$12, $12, 4
	sltu	$14, $12, $13
	bnez	$14, 2b
	nop

	move	$12, $19		# copy
	addu	$13, $11, $9
3:	lw	$14, 0($11)
	addiu	$11, $11, 4
	sw	$14, 0($12)
	sltu	$15, $11, $13
	bnez	$15, 3b
	addiu	$12, $12, 4

	li	$8, 0xa0		# FlushCache (A0 function 0x44)
	jalr	$8
	li	$9, 0x44

	jalr	$19			# install the driver
	nop

	move	$4, $16
	move	$5, $17
	jr	$20			# the game
	move	$31, $18

	.align	4			# (16: the table starts where loader.bin ends)
table:
