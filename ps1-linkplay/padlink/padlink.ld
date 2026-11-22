/* The pad play driver in Crash Team Racing (USA): in GhostReplay_ThTick (0x80026ED8-0x80027837), Time Trial's ghost
   replay, never run outside Time Trial (GhostReplay_Init1 returns unless (gameMode1 & 0x20022000) == 0x20000). */
MEMORY {
	G (rx) : ORIGIN = 0x80026ED8, LENGTH = 0x960
}
SECTIONS {
	.pl : { *(.text.pl) *(.rodata*) } > G
	.stray : { *(.text) *(.text.*) *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) } > G
	/DISCARD/ : { *(.reginfo) *(.MIPS.*) *(.pdr) *(.gnu.attributes) *(.comment) *(.note*) }
}
