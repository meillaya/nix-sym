/*
 * Layout for fixtures/aarch64/fixture.c, loaded as a non-PIE ET_EXEC at
 * 0x200000.  The read-only metadata (build-id note, rodata, .eh_frame) stays
 * in the first page; the text is forced to the next page so its file-backed
 * mapping has page offset 1 instead of 0.  Without that, the aarch64 kernel
 * maps both the header page and the text page from file offset 0, and libdwfl
 * reports two overlapping modules for the same executable.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x200000 + SIZEOF_HEADERS;
  .note.gnu.build-id : { *(.note.gnu.build-id) }
  .rodata : { *(.rodata .rodata.*) }
  .eh_frame_hdr : { *(.eh_frame_hdr) }
  .eh_frame : { *(.eh_frame) }
  . = ALIGN(0x1000);
  .text : { *(.text .text.*) }
  /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.ARM.attributes) }
}
