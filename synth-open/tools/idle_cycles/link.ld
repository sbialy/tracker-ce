ENTRY(bench_run)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 3M }
SECTIONS
{
  .text : { *(.text*) *(.audio_ram_functions*) *(.audio_flash_functions*) *(.rodata*) } > RAM
  .init_array : { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; } > RAM
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(.dmabuffers*) *(COMMON) } > RAM
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
