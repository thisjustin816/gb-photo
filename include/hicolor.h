#ifndef HICOLOR_H
#define HICOLOR_H

#include <gbdk/platform.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(COLOR_PHOTOS)
extern bool hicolor_active;
extern bool hicolor_deferred;
extern bool hicolor_pending;
void hicolor_scanline(void) NONBANKED;
void hicolor_begin(uint8_t key) BANKED;
bool hicolor_draw(uint8_t slot, uint8_t x, uint8_t y, bool thumbnail) BANKED;
void hicolor_start(void) BANKED;
void hicolor_stop(void) BANKED;
void hicolor_invalidate(void) BANKED;
void hicolor_ui_cell(uint8_t x, uint8_t y) BANKED;
#else
inline void hicolor_stop(void) {}
inline void hicolor_invalidate(void) {}
#endif

#endif
