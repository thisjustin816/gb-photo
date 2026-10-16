#ifndef __STATE_GALLERY_H_INCLUDE__
#define __STATE_GALLERY_H_INCLUDE__

#include <stdint.h>

#include "gbcamera.h"
#include "vector.h"
#include "menus.h"

#include "globals.h"

#include "misc_assets.h"
#include "color_photo.h"

BANKREF_EXTERN(state_gallery)

VECTOR_DECLARE_EXTERN(used_slots, uint8_t, CAMERA_MAX_IMAGE_SLOTS);
VECTOR_DECLARE_EXTERN(free_slots, uint8_t, CAMERA_MAX_IMAGE_SLOTS);

inline uint8_t images_taken(void) {
    return VECTOR_LEN(used_slots);
}

inline uint8_t images_total(void) {
    return CAMERA_MAX_IMAGE_SLOTS;
}

inline uint8_t images_free(void) {
    return VECTOR_LEN(free_slots);
}

inline uint8_t gallery_count(void) {
#if defined(COLOR_PHOTOS)
    return color_gallery_count;
#else
    return images_taken();
#endif
}

inline uint8_t gallery_slot(uint8_t index) {
#if defined(COLOR_PHOTOS)
    return color_gallery_slots[index];
#else
    return VECTOR_GET(used_slots, index);
#endif
}

inline void gallery_show_progressbar(uint8_t x, uint8_t value, uint8_t size) {
    misc_render_progressbar(value, size, text_buffer);
    menu_text_out(x, 17, HELP_CONTEXT_WIDTH, WHITE_ON_BLACK, ITEM_DEFAULT, text_buffer);
}

uint8_t gallery_print_picture(uint8_t image_no, uint8_t frame_no) BANKED;
uint8_t gallery_transfer_picture(uint8_t image_no) BANKED;

void gallery_toss_images(void) BANKED;
void gallery_delete_picture(uint8_t image_no) BANKED;

#endif
