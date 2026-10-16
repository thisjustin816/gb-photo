#include <gbdk/platform.h>

#if defined(COLOR_PHOTOS)

#include "hicolor.h"

static uint16_t hicolor_stack;

void hicolor_scanline(void) NONBANKED NAKED {
    __asm
        ld (_hicolor_stack), sp
        ld sp, #0xD010
        ld hl, #_BCPD_REG
        ld b, #1
        ld a, #5
        ldh (_SVBK_REG), a
1$:
        ld a, b
        and #1
        swap a
        or #(BCPSF_AUTOINC | 8)
        ld c, a
2$:
        ldh a, (_LY_REG)
        inc a
        cp b
        jr nz, 2$
3$:
        ldh a, (_STAT_REG)
        and #3
        jr nz, 3$
        ld a, c
        ldh (_BCPS_REG), a
        // 16 bytes take 224 CPU clocks (112 dots in double speed).
        .rept 8
            pop de
            ld (hl), e
            ld (hl), d
        .endm
        ld a, b
        cp #96
        jr nz, 4$
        ldh a, (_LCDC_REG)
        or #LCDCF_BG8000
        ldh (_LCDC_REG), a
4$:
        inc b
        ld a, b
        cp #144
        jr nz, 1$
5$:
        ldh a, (_LY_REG)
        cp #143
        jr nz, 5$
6$:
        ldh a, (_STAT_REG)
        and #3
        jr nz, 6$
        ldh a, (_LCDC_REG)
        and #~LCDCF_BG8000
        ldh (_LCDC_REG), a
        ld a, #1
        ldh (_SVBK_REG), a
        ld hl, #_hicolor_stack
        ld a, (hl+)
        ld h, (hl)
        ld l, a
        ld sp, hl
        ret
    __endasm;
}

#endif
