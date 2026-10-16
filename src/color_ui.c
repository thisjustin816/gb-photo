#pragma bank 255

#if defined(COLOR_PHOTOS)
#include <gbdk/platform.h>
#include <gbdk/metasprites.h>
#include <gb/gbdecompress.h>
#include <string.h>

#include "buffers.h"
#include "color_photo.h"
#include "color_ui.h"
#include "hicolor.h"
#include "palette.h"
#include "screen.h"
#include "color_ui_data.h"

uint8_t color_ui_depth;

void color_ui_begin(void) BANKED {
    static const palette_color_t black[32] = {0};
    if (color_ui_depth++) return;
    sync_vblank();
    hicolor_stop();
    set_bkg_palette(0, 8, (const palette_color_t *)black);
    set_sprite_palette(0, 8, (const palette_color_t *)black);
}

void color_ui_end(void) BANKED {
    if (--color_ui_depth) return;
    sync_vblank();
    palette_apply();
    if (hicolor_pending) hicolor_start();
}

void color_ui_clear(void) BANKED {
    hide_sprites_range(4, 40);
    VBK_REG = 1;
    fill_bkg_rect(0, 0, 20, 18, 0);
    VBK_REG = 0;
    palette_reload();
    palette_apply();
}

void color_ui_title(void) BANKED {
    gb_decompress(title_tiles_0, frame_buffer);
    screen_load_image(0, 0, 20, 6, frame_buffer);
    gb_decompress(title_tiles_6, frame_buffer);
    screen_load_image(0, 6, 20, 6, frame_buffer);
    gb_decompress(title_tiles_12, frame_buffer);
    screen_load_image(0, 12, 20, 4, frame_buffer);
    gb_decompress(title_attrs, frame_buffer);
    VBK_REG = 1;
    set_bkg_tiles(0, 0, 20, 16, frame_buffer);
    gb_decompress_sprite_data(0, title_patterns);
    VBK_REG = 0;
    gb_decompress(title_palettes, (uint8_t *)&BkgPalette[1]);
    gb_decompress(title_sprite_palettes, (uint8_t *)&SprPalette[2]);
    memcpy(&shadow_OAM[4], title_sprites, sizeof(title_sprites));
    palette_apply();
}

void color_ui_hub(void) BANKED {
    gb_decompress(hub_tiles_0, frame_buffer);
    for (uint8_t row = 0; row != 2; ++row)
        screen_load_image(6, 3 + row, 10, 1, frame_buffer + row * 288 + 80);
    screen_load_image(1, 5, 18, 4, frame_buffer + 576);
    screen_restore_rect(6, 3, 10, 2);
    screen_restore_rect(1, 5, 18, 4);
    gb_decompress(hub_attrs, frame_buffer);
    VBK_REG = 1;
    set_bkg_tiles(6, 3, 10, 1, frame_buffer + 5);
    set_bkg_tiles(6, 4, 10, 1, frame_buffer + 23);
    set_bkg_tiles(1, 5, 18, 4, frame_buffer + 36);
    VBK_REG = 0;
    gb_decompress(hub_palettes, (uint8_t *)&BkgPalette[1]);
    palette_apply();
}

void color_ui_camera(void) BANKED {
    static const uint16_t colors[] = {RGB_BLACK, RGB8(216,64,56), RGB8(64,160,88), RGB8(64,128,192)};
    static const uint8_t badge[] = {
        0xe0,0x07,0x90,0x08,0x90,0x08,0xe0,0x0b,0xa0,0x09,0x90,0x09,0x90,0x07,0,0,
        0x38,0x38,0x24,0x24,0x24,0x24,0x38,0xb8,0x24,0xa4,0x24,0xa4,0x38,0x38,0,0
    };
    if (!color_enabled) return;
    memcpy(&BkgPalette[5], colors, sizeof(colors));
    palette_apply();
    screen_load_image(12, 0, 2, 1, badge);
    screen_restore_rect(12, 0, 2, 1);
    VBK_REG = 1;
    fill_bkg_rect(12, 0, 2, 1, 5);
    VBK_REG = 0;
}
#endif
