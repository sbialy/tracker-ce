ENTRY(bench_run)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 3M }
SECTIONS
{
  .text : { *(.text*) *(.rodata*) } > RAM
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
