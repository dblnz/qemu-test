/*
 * Linker script for the flat-binary MMIO register test.
 *
 * The boot stub lives at 0x7C00 (BIOS entry), followed by C code.
 * The boot signature (0x55AA) is placed at byte 510-511.
 * Output is a raw binary loaded by QEMU as a disk image.
 */

OUTPUT_FORMAT("binary")
ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .boot : {
        *(.boot)
    }

    /* Pad boot sector to 510 bytes, add signature */
    . = 0x7DFE;
    .bootsig : {
        SHORT(0xAA55)
    }

    /* C code + data immediately after boot sector */
    . = 0x7E00;
    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note.*)
    }
}
