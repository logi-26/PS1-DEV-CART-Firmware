/*
* PS1 DEV CART debug monitor, linked at MONITOR_ADDR (cartridge flash, or RAM for firmware tests)
* The monitor runs from flash: no writable variables (its state is in the kernel hook RAM)
*/

OUTPUT_FORMAT("elf32-littlemips")
ENTRY(MonitorEntry)

SECTIONS {
    . = MONITOR_ADDR;
    .text : {
        KEEP(*(.start))
        /* Profiler sampler, copied to RAM by the monitor (see ProfFast in monitor.c) */
        . = ALIGN(4);
        __prof_start = .;
        KEEP(*(.prof_ram))
        __prof_end = .;
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.* .rodata1)
    }
    .data : {
        *(.data .data.* .sdata .sdata.*)
    }
    .bss : {
        *(.bss .bss.* .sbss .sbss.* COMMON)
    }
    ASSERT(__prof_end - __prof_start <= 672 - 32, "monitor: the profiler sampler is bigger than PROF_CODE_MAX")
    ASSERT(mon_regs == mon_stack + 512, "monitor: the profiler's RAM area (monitor stack, registers) is not in one piece")
    ASSERT(SIZEOF(.data) == 0, "monitor: no writable variables (it runs from flash)")
    ASSERT(SIZEOF(.bss) == 0, "monitor: no writable variables (it runs from flash)")
    ASSERT(. - MONITOR_ADDR <= 0x5F00, "monitor: bigger than its place in the cartridge flash (0x19000-0x1EF00, see cartrom/rom.s)")

    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.note*) }
}
