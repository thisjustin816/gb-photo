#ifndef COLOR_PHOTO_H
#define COLOR_PHOTO_H

#include <gbdk/platform.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(COLOR_PHOTOS)

#define COLOR_REQUEST_ADDRESS 0xA0F0
#define COLOR_MARKER_ADDRESS 0xAFEB
#define COLOR_MARKER_SIZE 7
#define COLOR_CHANNELS 3
#define COLOR_STAGE_BANK 2
#define COLOR_PALETTES_BANK 5
#define COLOR_TILES_BANK 6

extern bool color_enabled;
extern uint8_t color_gallery_slots[30], color_gallery_count;

void color_settings_load(void) BANKED;
void color_settings_save(void) BANKED;
void color_request(uint8_t channel) NONBANKED;
bool color_host_replied(void) NONBANKED;
bool color_capture(void) BANKED;
bool color_group_read(uint8_t slot, uint8_t channels[COLOR_CHANNELS]) BANKED;
void color_marker_clear(uint8_t slot) BANKED;
void color_gallery_refresh(void) BANKED;

void color_buffer_read(uint8_t bank, uint16_t offset, uint16_t size) BANKED;
void color_buffer_write(uint8_t bank, uint16_t offset, uint16_t size) BANKED;
void color_tile_row_write(uint16_t offset) BANKED;

#endif
#endif
