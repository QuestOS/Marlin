OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS {
  physical PT_LOAD FILEHDR PHDRS;
  virtualtext PT_LOAD;
  virtualdata PT_LOAD;
}
SECTIONS {
  /* Place per-CPU data in phantom segment, to be dynamically allocated */
  . = 0x0;
  _percpustart = .;
  .percpu (NOLOAD) : { *(.percpu) } : NONE
  _percpuend = ALIGN(0x1000);
  _percpu_pages_plus_one = (_percpuend - _percpustart) / 0x1000 + 1;

  /* Bootstrap physical section, at virtual and physical address 0x100000 */
  _physicalbootstrapstart = 0x100000;
  . = _physicalbootstrapstart + SIZEOF_HEADERS;
  .physicaltext : {   boot/boot.o boot/multiboot.o (.text) } : physical
  .physicalrodata : { boot/boot.o boot/multiboot.o (.rodata) } : physical
  .physicaldata : {   boot/boot.o boot/multiboot.o (.data) } : physical
  .physicalbss : {    boot/boot.o boot/multiboot.o (.bss COMMON) } : physical
  _physicalkernelstart = ALIGN(0x1000);
  _bootstrap_pages = (_physicalkernelstart - _physicalbootstrapstart) / 0x1000;

  /* Permanent virtual section, at virtual address 0xFFC00000 */
  /* Maps kernel in upper 4MB of virtual address space on 32-bit machines */
  . = 0xFFC00000;
  _kernelstart = .;

  .text : AT ( _physicalkernelstart )
             { *(.text) } : virtualtext
  . = ALIGN(32);
  .rodata : AT ( ( LOADADDR(.text) + SIZEOF(.text) + 31 ) & 0xFFFFFFE0 )
    { *(.rodata*) } : virtualtext

  . = ALIGN(32);
  /* Insert a read-only null-terminated array of pointers to constructors */
  _percpu_ctor_list = .;
  .ctors : AT ( ( LOADADDR (.rodata) + SIZEOF (.rodata) + 31 ) & 0xFFFFFFE0 )
              { *(.percpu.ctor); LONG(0) } : virtualtext

  . = ALIGN(32);
  /* Insert a read-only null-terminated array of pointers to modules */
  _module_ptr_list = .;
  .moduleptrs : AT ( ( LOADADDR (.ctors) + SIZEOF (.ctors) + 31 ) & 0xFFFFFFE0 )
                   { *(.module.ptrs); LONG(0) } : virtualtext

  . = ALIGN(0x1000);
   _kernel_readwrite = .;


  /* Place shared drivers data in separate pages for Quest virtual
   * extension. We also have to exclude them in the kernel readwrite
   * sections below. */
  . = ALIGN(0x1000);
  _shared_driver_data = .;
  _shared_driver_data_physical =
      (LOADADDR(.moduleptrs) + SIZEOF(.moduleptrs) + 0xFFF ) & 0xFFFFF000;
  .sddata : AT ( _shared_driver_data_physical )
               { shared.o (.data) } : virtualdata
  _shared_driver_data_end = ALIGN(0x1000);
  _shared_driver_data_pages = (_shared_driver_data_end - _shared_driver_data) / 0x1000;

  . = ALIGN(0x1000);
  .data : AT ( ( LOADADDR(.sddata) + SIZEOF(.sddata) + 0xFFF ) & 0xFFFFF000 )
             { *( EXCLUDE_FILE ( shared.o ) .data) } : virtualdata

  . = ALIGN(0x1000);
  .bss : AT ( ( LOADADDR(.data) + SIZEOF(.data) + 0xFFF ) & 0xFFFFF000 )
            { *( EXCLUDE_FILE ( shared.o ) .bss COMMON)} : virtualdata

  /* Place shared drivers data in separate pages for Quest virtual
   * extension. We also have to exclude them in the kernel readwrite
   * sections above. */
  . = ALIGN(0x1000);
  _shared_driver_bss = .;
  _shared_driver_bss_physical =
      (LOADADDR(.bss) + SIZEOF(.bss) + 0xFFF ) & 0xFFFFF000;
  .sdbss : AT ( _shared_driver_bss_physical )
              { shared.o (.bss COMMON)} : virtualdata
  _shared_driver_bss_end = ALIGN(0x1000);
  _shared_driver_bss_pages = (_shared_driver_bss_end - _shared_driver_bss) / 0x1000;


  _kernelend = ALIGN(0x1000);
  _readwrite_pages = ( _kernelend - _kernel_readwrite ) / 0x1000;
  _readonly_pages = ( _kernel_readwrite - _kernelstart ) / 0x1000;

  . = ALIGN(0x1000);
  .eh_frame (NOLOAD) : AT ( _kernelend )
                       { *(.eh_frame) } : NONE
}
