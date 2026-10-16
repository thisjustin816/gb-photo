#include <gbdk/platform.h>

#if defined(COLOR_PHOTOS)

#include "buffers.h"
#include "color_photo.h"

static uint8_t copy_bank, copy_write;
static uint16_t copy_address, copy_size;

static void copy_buffer(void) NONBANKED NAKED {
    __asm
        ld hl, #_copy_address
        ld a, (hl+)
        ld e, a
        ld d, (hl)
        ld hl, #_copy_size
        ld a, (hl+)
        ld c, a
        ld b, (hl)
        ld hl, #_frame_buffer
        ld a, (_copy_write)
        or a
        ld a, (_copy_bank)
        ldh (_SVBK_REG), a
        jr z, 2$
        ld a, (_copy_write)
        cp #2
        jr z, 4$
1$:
        ld a, (hl+)
        ld (de), a
        inc de
        dec bc
        ld a, b
        or c
        jr nz, 1$
        jr 3$
2$:
        ld a, (de)
        ld (hl+), a
        inc de
        dec bc
        ld a, b
        or c
        jr nz, 2$
        jr 3$
4$:
        ld c, #4
5$:
        ld a, (hl+)
        ld (de), a
        inc de
        ld a, (hl+)
        ld (de), a
        ld a, e
        add #15
        ld e, a
        jr nc, 6$
        inc d
6$:
        dec c
        jr nz, 5$
3$:
        ld a, #1
        ldh (_SVBK_REG), a
        ret
    __endasm;
}

void color_buffer_read(uint8_t bank, uint16_t offset, uint16_t size) BANKED {
    if (!size) return;
    copy_bank = bank;
    copy_address = 0xD000 + offset;
    copy_size = size;
    copy_write = false;
    // The caller's globals and stack remain in WRAM bank 1.
    CRITICAL { copy_buffer(); }
}

void color_buffer_write(uint8_t bank, uint16_t offset, uint16_t size) BANKED {
    if (!size) return;
    copy_bank = bank;
    copy_address = 0xD000 + offset;
    copy_size = size;
    copy_write = true;
    CRITICAL { copy_buffer(); }
}

void color_tile_row_write(uint16_t offset) BANKED {
    copy_bank = COLOR_TILES_BANK;
    copy_address = 0xD000 + offset;
    copy_write = 2;
    CRITICAL { copy_buffer(); }
}

#endif
