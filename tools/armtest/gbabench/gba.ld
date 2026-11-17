ENTRY(_start)
MEMORY { rom : ORIGIN = 0x08000000, LENGTH = 32M
         iwram : ORIGIN = 0x03000000, LENGTH = 28K
         ewram : ORIGIN = 0x02000000, LENGTH = 256K }
SECTIONS {
  .text : { KEEP(*(.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); } > rom
  .iwram : { __iwram_start = .; *(.iwram*) *(.data*) . = ALIGN(4); __iwram_end = .; } > iwram AT > rom
  __iwram_lma = LOADADDR(.iwram);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; } > iwram
  .ewram (NOLOAD) : { __ebss_start = .; *(.ewram*) . = ALIGN(4); __ebss_end = .; } > ewram
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
