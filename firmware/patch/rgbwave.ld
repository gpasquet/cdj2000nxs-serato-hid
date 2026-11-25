/* Code couleur ajouté au programme d'affichage : zone 0xFD6000 (hors des blocs du flux LDR
 * d'origine ; c'est aussi la zone utilisée par LNNRT, validée sur platine). */
ENTRY(_rgb_decode)
_orig_copy_body = 0x00D49398;    /* suite de la routine de copie d'origine */
SECTIONS
{
    . = 0x00FD6000;
    .text   : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    /* variables sans valeur initiale : dans le bloc chargé, donc mises à zéro au démarrage (la
     * mémoire de la platine n'est pas effacée ; une .bss à part garderait ce qui s'y trouvait) */
    .data   : { *(.data .data.*) *(.bss .bss.* COMMON) }
    _rgb_end = .;
    /DISCARD/ : { *(.comment) *(.note*) }
}
