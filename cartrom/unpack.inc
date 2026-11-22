# PS1 DEV CART: the firmware unpacked at boot (packed by rom_packer/main.c, see it for the format).
# unpack: $a0 the packed data, $a1 where it goes, $a2 the end of the output, $a3 the end of the packed data's first
# part, $t8 its second part (read from there once $a0 reaches $a3: the packer cuts the data before an item, never
# before a flag byte, so this is checked before each item). Uses $t0-$t7 (noreorder: delay slots and load delays
# written out).
#
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.

unpack:
	move	$t3, $0					# Flag bits left
unpack_loop:
	sltu	$t0, $a1, $a2
	beqz	$t0, unpack_done
	nop
	bnez	$t3, unpack_bit
	nop
	lbu		$t2, 0($a0)				# Flags: 1 a byte as it is, 0 a copy (bit 0 first)
	addiu	$a0, 1
	li		$t3, 8
unpack_bit:
	bne		$a0, $a3, unpack_item
	nop
	move	$a0, $t8				# The first part read: the second one
unpack_item:
	andi	$t4, $t2, 1
	srl		$t2, $t2, 1
	beqz	$t4, unpack_copy
	addiu	$t3, -1					# (delay slot)
	lbu		$t5, 0($a0)				# A byte
	addiu	$a0, 1
	sb		$t5, 0($a1)
	b		unpack_loop
	addiu	$a1, 1					# (delay slot)
unpack_copy:
	lbu		$t5, 0($a0)				# Offset - 1 (12 bits), length - 3 (4 bits)
	lbu		$t6, 1($a0)
	addiu	$a0, 2
	andi	$t7, $t6, 0x0F
	sll		$t7, $t7, 8
	or		$t7, $t7, $t5
	addiu	$t7, 1
	subu	$t7, $a1, $t7			# From the output already written
	srl		$t6, $t6, 4
	addiu	$t6, 3
unpack_copy_loop:
	lbu		$t5, 0($t7)
	addiu	$t7, 1
	addiu	$t6, -1
	sb		$t5, 0($a1)
	bnez	$t6, unpack_copy_loop
	addiu	$a1, 1					# (delay slot)
	b		unpack_loop
	nop
unpack_done:
	jr		$ra
	nop
