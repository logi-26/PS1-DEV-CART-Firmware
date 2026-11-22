/* The PS1 DEV CART build of the virtual link driver: in three gaps of Red Alert Retaliation's serial library that the
   patch makes dead (the code is the game's own, which it never writes; nothing else of its RAM is used):
     A  0x80153DFC-0x801540D7  the sync read after its patched entry, and the async write start
     B  0x801540E0-0x80154353  the sync write after its patched entry
     C  0x801548E4-0x80154C7B  the async interrupt handler (only called for an async transfer: never, here) */
MEMORY {
	A (rx) : ORIGIN = 0x80153DFC, LENGTH = 0x2DC
	B (rx) : ORIGIN = 0x801540E0, LENGTH = 0x274
	C (rx) : ORIGIN = 0x801548E4, LENGTH = 0x398
}
SECTIONS {
	.a : { *(.text.a) } > A
	.b : { *(.text.b) } > B
	.c : { *(.text.c0) *(.text.c) *(.rodata*) } > C
	.stray : { *(.text) *(.text.*) *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) } > A
	/DISCARD/ : { *(.reginfo) *(.MIPS.*) *(.pdr) *(.gnu.attributes) *(.comment) *(.note*) }
}
