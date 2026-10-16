#ifndef COLOR_UI_H
#define COLOR_UI_H

#include <gbdk/platform.h>

#if defined(COLOR_PHOTOS)
void color_ui_begin(void) BANKED;
void color_ui_end(void) BANKED;
void color_ui_title(void) BANKED;
void color_ui_hub(void) BANKED;
void color_ui_clear(void) BANKED;
void color_ui_camera(void) BANKED;
#else
#define color_ui_begin()
#define color_ui_end()
#define color_ui_title()
#define color_ui_hub()
#define color_ui_clear()
#define color_ui_camera()
#endif

#endif
