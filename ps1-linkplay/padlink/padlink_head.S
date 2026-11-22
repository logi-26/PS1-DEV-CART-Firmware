# Pad play driver (padlink.c): the stubs the game's patched places jump to. Each saves what the place needs, calls the
# C function, then runs the two instructions the patch replaced and jumps back after them.
	.set	noreorder
	.section .text.pl, "ax"

# GAMEPAD_ProcessAnyoneVars (0x800262D0): its first two instructions (addiu sp,-0x20 / sw s0,0x10(sp)) replaced
	.globl	pl_frame_stub
pl_frame_stub:
	addiu	$sp, $sp, -0x20
	sw		$a0, 0x10($sp)
	sw		$a1, 0x14($sp)
	sw		$ra, 0x18($sp)
	jal		pl_frame
	nop
	lw		$a0, 0x10($sp)
	lw		$a1, 0x14($sp)
	lw		$ra, 0x18($sp)
	addiu	$sp, $sp, 0x20
	addiu	$sp, $sp, -0x20			# (replaced)
	j		0x800262D8
	sw		$s0, 0x10($sp)			# (replaced)

# The race start's frame step store (0x8003CB78: addiu v0,zero,0x20 / sw v0,0x1D04(a0)), inside main's loop: every
# register the function may hold is kept
	.globl	pl_load_stub
pl_load_stub:
	addiu	$sp, $sp, -0x50
	sw		$v1, 0x10($sp)
	sw		$a0, 0x14($sp)
	sw		$a1, 0x18($sp)
	sw		$a2, 0x1C($sp)
	sw		$a3, 0x20($sp)
	sw		$t0, 0x24($sp)
	sw		$t1, 0x28($sp)
	sw		$t2, 0x2C($sp)
	sw		$t3, 0x30($sp)
	sw		$t4, 0x34($sp)
	sw		$t5, 0x38($sp)
	sw		$t6, 0x3C($sp)
	sw		$t7, 0x40($sp)
	sw		$t8, 0x44($sp)
	sw		$t9, 0x48($sp)
	sw		$ra, 0x4C($sp)			# (before the jal: its delay slot would see the new ra)
	jal		pl_load
	nop
	lw		$v1, 0x10($sp)
	lw		$a0, 0x14($sp)
	lw		$a1, 0x18($sp)
	lw		$a2, 0x1C($sp)
	lw		$a3, 0x20($sp)
	lw		$t0, 0x24($sp)
	lw		$t1, 0x28($sp)
	lw		$t2, 0x2C($sp)
	lw		$t3, 0x30($sp)
	lw		$t4, 0x34($sp)
	lw		$t5, 0x38($sp)
	lw		$t6, 0x3C($sp)
	lw		$t7, 0x40($sp)
	lw		$t8, 0x44($sp)
	lw		$t9, 0x48($sp)
	lw		$ra, 0x4C($sp)
	addiu	$sp, $sp, 0x50
	addiu	$v0, $zero, 0x20		# (replaced)
	j		0x8003CB80
	sw		$v0, 0x1D04($a0)		# (replaced)
