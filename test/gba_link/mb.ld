ENTRY(_start)
MEMORY { ewram : ORIGIN = 0x02000000, LENGTH = 256K  iwram : ORIGIN = 0x03000000, LENGTH = 32K }
SECTIONS {
  .text : { KEEP(*(.crt0)) *(.text*) *(.glue_7*) *(.rodata*) } > ewram
  .data : { __data_start = .; *(.data*) __data_end = .; } > ewram
  .iwram : { __iwram_start = .; *(.iwram*) __iwram_end = .; } > iwram AT > ewram
  __iwram_lma = LOADADDR(.iwram);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > ewram
  /DISCARD/ : { *(.ARM.exidx*) }
}
