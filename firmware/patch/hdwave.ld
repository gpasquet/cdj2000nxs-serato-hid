/* Code et constantes ajoutés après le programme d'origine ; tampons dans la RAM libre. */
ENTRY(_hd_send)
SECTIONS
{
    . = 0x043E0000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.* COMMON) }
    _hd_image_end = .;
    . = 0x04400000;
    .hdbss (NOLOAD) : { *(.hdbss) }
    _hd_ram_end = .;
    /DISCARD/ : { *(.comment) *(.note*) }
}
