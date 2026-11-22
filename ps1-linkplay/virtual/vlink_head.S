# Entry points at fixed offsets from the start of the blob (the patches jump here)
#   +0x00 _sio_control   +0x08 read   +0x10 write   +0x18 vlink_service (the carrier's hook: a breakpoint in the
#   emulator)   +0x20 the library's own _sio_control (its first two instructions, then the rest of it)
	.set	noreorder
#ifdef VLINK_HW
# PS1 DEV CART: no head (the patches jump to the functions, see build_hw.py)
# First in gap C (0x801548E4): the library's SIO interrupt handler was here. The library's init registers it with the
# kernel (record 0x801AD0AC: handler 0x801548E4, check 0x80154868), called whenever the SIO port flags an interrupt:
# never in the emulator, but on a console after a link game (the port's inputs unconnected), it ran the driver's code
# and froze the game (BEN at its menu, 2026-11-13). Here: the interrupt acknowledged in the port and in I_STAT.
	.section .text.c0, "ax"
	.globl	vlink_sio_irq
# Acknowledging only wasn't enough (BEN froze again at its menu, 20:02): the port's interrupts are switched off for
# good (the driver never uses the real port): SIO masked in I_MASK, its interrupt enables cleared, then acknowledged.
vlink_sio_irq:
	lui		$t0, 0x1F80
	addiu	$t2, $zero, -0x101		# (all bits but bit 8, SIO)
	lw		$t1, 0x1074($t0)		# I_MASK: SIO masked
	nop
	and		$t1, $t1, $t2
	sw		$t1, 0x1074($t0)
	lhu		$t1, 0x105A($t0)		# SIO1 control: TX, RX and DSR interrupt enables cleared, acknowledged
	nop
	andi	$t1, $t1, 0xE3FF
	ori		$t1, $t1, 0x10
	sh		$t1, 0x105A($t0)
	sw		$t2, 0x1070($t0)		# I_STAT: bit 8 (SIO) cleared, the others kept
	jr		$ra
	move	$v0, $zero
# Net Play's automatic LINK GAME: the patch of _sio_control jumps here first. The main menu (the front end overlay's
# 0x80012F10) calls _sio_control(0,0,0) every frame (its cable check) and returns to 0x80013024, where s4 holds the
# menu's choice (-1 none): s4 = 4 there = LINK GAME chosen, the menu's own path follows (its fade, then the link page).
# The computer arms it by writing that return address into the auto word (stats[6], never written by the console):
# taken once, only from that call, then cleared. (Then the computer fills in the link page: country, color, START.)
#define	VLINK_AUTO	(VLINK_BUF + 0x168)		/* (vlink.c checks the offset) */
	.section .text.b, "ax"
	.globl	vlink_auto_entry
vlink_auto_entry:
	lui		$t1, %hi(VLINK_AUTO)
	lw		$t0, %lo(VLINK_AUTO)($t1)
	nop
	bne		$ra, $t0, vlink_control		# (not armed, or another caller: the usual _sio_control)
	nop
	sw		$zero, %lo(VLINK_AUTO)($t1)	# taken once
	j		vlink_control
	addiu	$s4, $zero, 4					# the main menu's choice: LINK GAME
# The library's own _sio_control
	.section .text.c, "ax"
	.globl	vlink_orig_control
vlink_orig_control:
	addiu	$sp, $sp, -0x18
	j		0x8015435C
	sw		$s0, 0x10($sp)
	nop
#else
	.section .text.head, "ax"
	.globl	vlink_service
	.globl	vlink_orig_control
head:
	j		vlink_control
	nop
	j		vlink_read
	nop
	j		vlink_write
	nop
vlink_service:
	jr		$ra				# (the emulator: a breakpoint here)
	nop
vlink_orig_control:
	addiu	$sp, $sp, -0x18
	j		0x8015435C
	sw		$s0, 0x10($sp)
	nop
#endif
