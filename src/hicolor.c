#pragma bank 255

#include <gbdk/platform.h>

#if defined(COLOR_PHOTOS)

#include <string.h>

#include "buffers.h"
#include "color_photo.h"
#include "hicolor.h"
#include "gbprinter.h"
#include "musicmanager.h"
#include "palette.h"
#include "screen.h"
#include "state_camera.h"
#include "state_gallery.h"
#include "systemdetect.h"
#include "hicolor_distance.h"

bool hicolor_active = false;
bool hicolor_deferred = false;

static bool cache_valid, cache_reuse, has_color, restore_slow;
static uint8_t cache_key, cache_palette, loaded_row;
static uint8_t attributes[16 * 16];
static uint8_t source_tiles[3][256], colors[2][32];
static struct {
    uint8_t histogram[64], errors[64], mapping[64];
} quant;
#define histogram quant.histogram
#define errors quant.errors
#define mapping quant.mapping
static uint8_t choices[4], quant_index, quant_weight, quant_next;
static uint8_t previous_histogram[64], pack_count;
static bool histogram_valid;
static uint16_t rgb_colors[64];

void hicolor_stop(void) BANKED {
    if (!_is_COLOR) return;
    if (hicolor_active) {
        CRITICAL {
            hicolor_active = false;
            LYC_REG = 95;
            STAT_REG = STATF_LYC;
        }
        sync_vblank();
        VBK_REG = 1;
        fill_bkg_rect(0, 0, DEVICE_SCREEN_WIDTH, DEVICE_SCREEN_HEIGHT, 0);
        VBK_REG = 0;
        palette_apply();
    }
    if (restore_slow) {
        CPU_SLOW();
        music_setup_timer_ex(false);
        restore_slow = false;
    }
}

void hicolor_invalidate(void) BANKED {
    cache_valid = false;
}

void hicolor_begin(uint8_t key) BANKED {
    if (!_is_COLOR) return;
    hicolor_stop();
    restore_slow = !_is_CPU_FAST;
    if (restore_slow) {
        CPU_FAST();
        music_setup_timer_ex(true);
    }
    cache_reuse = cache_valid && key == cache_key && cache_palette == OPTION(cgb_palette_idx);
    if (cache_reuse) return;
    cache_key = key;
    cache_palette = OPTION(cgb_palette_idx);
    cache_valid = has_color = false;
    memset(attributes, 0, sizeof(attributes));
    memset(frame_buffer, 0, CAMERA_IMAGE_SIZE);
    color_buffer_write(COLOR_PALETTES_BANK, 0, 144 * 16);
    color_buffer_write(COLOR_TILES_BANK, 0, CAMERA_IMAGE_SIZE);
    color_buffer_write(COLOR_TILES_BANK, CAMERA_IMAGE_SIZE, 4096 - CAMERA_IMAGE_SIZE);
    const uint16_t *palette = (const uint16_t *)&BkgPalette[0];
    for (uint8_t color = 0; color != 64; color++) {
        rgb_colors[color] = (palette[color >> 4] & 0x001F) |
                            (palette[(color >> 2) & 3] & 0x03E0) |
                            (palette[color & 3] & 0x7C00);
    }
}

static void load_row(uint8_t row) {
    if (loaded_row == row) return;
    for (uint8_t channel = 0; channel != 3; channel++) {
        color_buffer_read(COLOR_STAGE_BANK + channel, (uint16_t)row << 8, 256);
        memcpy(source_tiles[channel], frame_buffer, 256);
    }
    loaded_row = row;
}

static void sample_colors(int16_t y, uint8_t stripe, bool thumbnail, uint8_t *dest) {
    uint8_t height = thumbnail ? 28 : 112;
    if (y < 0 || y >= height) { memset(dest, 63, 32); return; }
    uint8_t sy = thumbnail ? (y << 2) : y;
    load_row(sy >> 3);
    uint8_t row_offset = (sy & 7) << 1;
    const uint8_t *red = source_tiles[0] + row_offset;
    const uint8_t *green = source_tiles[1] + row_offset;
    const uint8_t *blue = source_tiles[2] + row_offset;
    if (!thumbnail) {
        for (uint8_t tile = 0; tile != 4; tile++) {
            uint8_t offset = ((stripe << 2) + tile) << 4;
            uint8_t r0 = red[offset], r1 = red[offset + 1];
            uint8_t g0 = green[offset], g1 = green[offset + 1];
            uint8_t b0 = blue[offset], b1 = blue[offset + 1];
            for (uint8_t bit = 0; bit != 8; bit++) {
                uint8_t color = 0;
                if (r0 & 0x80) color |= 16;
                if (r1 & 0x80) color |= 32;
                if (g0 & 0x80) color |= 4;
                if (g1 & 0x80) color |= 8;
                if (b0 & 0x80) color |= 1;
                if (b1 & 0x80) color |= 2;
                *dest++ = color;
                r0 <<= 1; r1 <<= 1;
                g0 <<= 1; g1 <<= 1;
                b0 <<= 1; b1 <<= 1;
            }
        }
        return;
    }
    for (uint8_t x = 0; x != 32; x++) {
        uint8_t sx = thumbnail ? (x << 2) : ((stripe << 5) + x);
        uint8_t offset = (sx >> 3) << 4;
        uint8_t mask = 0x80 >> (sx & 7), color = 0;
        if (red[offset] & mask) color |= 16;
        if (red[offset + 1] & mask) color |= 32;
        if (green[offset] & mask) color |= 4;
        if (green[offset + 1] & mask) color |= 8;
        if (blue[offset] & mask) color |= 1;
        if (blue[offset + 1] & mask) color |= 2;
        *dest++ = color;
    }
}

static uint8_t histogram_build(void) NONBANKED NAKED {
    __asm
        ld hl, #_colors
        ld b, #64
        ld c, #0
1$:
        ld a, (hl+)
        push hl
        add #<_quant
        ld l, a
        ld a, #>_quant
        adc #0
        ld h, a
        inc (hl)
        ld a, (hl)
        cp c
        jr c, 2$
        jr z, 2$
        ld c, a
        ld a, l
        sub #<_quant
        ld (_quant_next), a
2$:
        pop hl
        dec b
        jr nz, 1$
        ld a, (_quant_next)
        ret
    __endasm;
}

static uint8_t quantize_pass(const uint8_t *distances) NONBANKED NAKED {
    distances;
    __asm
        ld hl, #_quant
        ld b, #64
        ld c, #0
        xor a
        ld (_quant_weight), a
        ld (_quant_next), a
1$:
        ld a, (hl)
        or a
        jr z, 8$
        push hl
        ld a, l
        add #64
        ld l, a
        jr nc, 2$
        inc h
2$:
        ld a, (de)
        cp (hl)
        jr nc, 4$
        ld (hl), a
        push af
        ld a, l
        add #64
        ld l, a
        jr nc, 3$
        inc h
3$:
        ld a, (_quant_index)
        ld (hl), a
        pop af
        jr 5$
4$:
        ld a, (hl)
5$:
        pop hl
        cp c
        jr c, 8$
        jr nz, 6$
        ld a, (_quant_weight)
        cp (hl)
        jr nc, 8$
        jr 7$
6$:
        ld c, a
7$:
        ld a, (hl)
        ld (_quant_weight), a
        ld a, #64
        sub b
        ld (_quant_next), a
8$:
        inc hl
        inc de
        dec b
        jr nz, 1$
        ld a, (_quant_next)
        ret
    __endasm;
}

static void quantize(void) {
    memset(histogram, 0, sizeof(histogram));
    uint8_t dominant = histogram_build();
    if (histogram_valid && !memcmp(histogram, previous_histogram, sizeof(histogram))) return;
    memcpy(previous_histogram, histogram, sizeof(histogram));
    histogram_valid = true;
    choices[0] = dominant;
    memset(errors, 255, sizeof(errors));
    for (quant_index = 0; quant_index != 4; quant_index++) {
        uint8_t next = quantize_pass(hicolor_distances + ((uint16_t)choices[quant_index] << 6));
        if (quant_index != 3) choices[quant_index + 1] = next;
    }
}

static void pack_colors(uint8_t *dest, const uint8_t *source) NONBANKED NAKED {
    dest; source;
    __asm
        ld h, b
        ld l, c
        ld a, #4
        ld (_pack_count), a
1$:
        ld b, #0
        ld c, #0
        .rept 8
            ld a, (hl+)
            push hl
            add #<(_quant+128)
            ld l, a
            ld a, #>(_quant+128)
            adc #0
            ld h, a
            ld a, (hl)
            rrca
            rl b
            rrca
            rl c
            pop hl
        .endm
        ld a, b
        ld (de), a
        inc de
        ld a, c
        ld (de), a
        inc de
        ld a, (_pack_count)
        dec a
        ld (_pack_count), a
        jp nz, 1$
        ret
    __endasm;
}

static void store_row(uint8_t y, uint8_t zone, uint8_t row) {
    pack_colors(frame_buffer, colors[row]);
    uint16_t offset = ((uint16_t)((y >> 3) - 1) << 8) + ((uint16_t)zone << 6) + ((y & 7) << 1);
    color_tile_row_write(offset);
}

static void upload(uint8_t x, uint8_t y, uint8_t width, uint8_t height) {
    for (uint8_t row = 0; row != height; row++) {
        color_buffer_read(COLOR_TILES_BANK, (uint16_t)(y + row - 1) << 8, 256);
        screen_load_image(x, y + row, width, 1, frame_buffer + ((x - 2) << 4));
    }
    screen_restore_rect(x, y, width, height);
    VBK_REG = 1;
    for (uint8_t row = 0; row != height; row++) {
        set_bkg_tiles(x, y + row, width, 1, attributes + ((y + row - 1) << 4) + x - 2);
    }
    VBK_REG = 0;
}

bool hicolor_draw(uint8_t slot, uint8_t x, uint8_t y, bool thumbnail) BANKED {
    uint8_t channels[3];
    if (!_is_COLOR || !color_group_read(slot, channels)) return false;
    uint8_t width = thumbnail ? 4 : 16, height = thumbnail ? 4 : 14;
    if (!cache_reuse) {
        for (uint8_t channel = 0; channel != 3; channel++) {
            CAMERA_SWITCH_RAM((channels[channel] >> 1) + 1);
            memcpy(frame_buffer, (channels[channel] & 1) ? image_second : image_first, CAMERA_IMAGE_SIZE);
            color_buffer_write(COLOR_STAGE_BANK + channel, 0, CAMERA_IMAGE_SIZE);
        }
        loaded_row = 255;
        uint8_t top = y << 3, bottom = (y + height) << 3;
        uint8_t last_progress = 255;
        uint16_t total = (uint16_t)width * height * 2;
        for (uint8_t stripe = 0; stripe != (width >> 2); stripe++) {
            uint8_t zone = ((x - 2) >> 2) + stripe;
            int16_t first = (zone < 2) ? top : (top - 1);
            for (int16_t line = first; line < bottom; line += 2) {
                int16_t relative = line - top - (thumbnail ? 2 : 0);
                sample_colors(relative, stripe, thumbnail, colors[0]);
                sample_colors(relative + 1, stripe, thumbnail, colors[1]);
                quantize();
                for (uint8_t i = 0; i != 4; i++) {
                    uint16_t color = rgb_colors[choices[i]];
                    frame_buffer[i << 1] = color;
                    frame_buffer[(i << 1) + 1] = color >> 8;
                }
                color_buffer_write(COLOR_PALETTES_BANK, (line << 4) + ((zone & 1) << 3), 8);
                if (line >= top) store_row(line, zone, 0);
                if (line + 1 < bottom) store_row(line + 1, zone, 1);
                uint16_t completed = (uint16_t)stripe * (height << 3) + line - first;
                uint8_t progress = completed * PRN_MAX_PROGRESS / total;
                if (progress != last_progress) {
                    gallery_show_progressbar(0, progress, PRN_MAX_PROGRESS);
                    last_progress = progress;
                }
            }
        }
    }
    for (uint8_t stripe = 0; stripe != (width >> 2); stripe++) {
        uint8_t zone = ((x - 2) >> 2) + stripe;
        for (uint8_t row = 0; row != height; row++) {
            memset(attributes + ((y + row - 1) << 4) + (zone << 2), zone + 1, 4);
        }
    }
    upload(x, y, width, height);
    has_color = true;
    return true;
}

void hicolor_ui_cell(uint8_t x, uint8_t y) BANKED {
    attributes[((y - 1) << 4) + x - 2] = 0;
}

void hicolor_start(void) BANKED {
    if (!_is_COLOR || !has_color) { hicolor_stop(); return; }
    cache_valid = cache_reuse = true;
    if (hicolor_deferred) return;
    sync_vblank();
    VBK_REG = 1;
    set_bkg_tiles(2, 1, 16, 16, attributes);
    VBK_REG = 0;
    set_bkg_palette(0, 1, (const palette_color_t *)&BkgPalette[0]);
    CRITICAL {
        LCDC_REG &= ~LCDCF_BG8000;
        hicolor_active = true;
        LYC_REG = 152;
        STAT_REG = STATF_LYC;
    }
}

#endif
