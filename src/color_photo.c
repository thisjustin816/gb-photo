#pragma bank 255

#include <gbdk/platform.h>

#if defined(COLOR_PHOTOS)

#include <string.h>

#include "buffers.h"
#include "color_photo.h"
#include "compat.h"
#include "gbcamera.h"
#include "joy.h"
#include "menu_codes.h"
#include "menu_msgbox.h"
#include "protected.h"
#include "state_camera.h"
#include "state_gallery.h"

static volatile uint8_t AT(COLOR_REQUEST_ADDRESS) request[16];
static uint8_t AT(0xA0E0) color_options[6];

bool color_enabled = false;
uint8_t color_gallery_slots[CAMERA_MAX_IMAGE_SLOTS], color_gallery_count;

void color_settings_load(void) BANKED {
    CAMERA_SWITCH_RAM(0);
    bool valid = !memcmp(color_options, "RGBS", 4) && color_options[4] <= 1 &&
                 color_options[5] == (uint8_t)~color_options[4];
    color_enabled = _is_COLOR && (!valid || color_options[4]);
}

void color_settings_save(void) BANKED {
    CAMERA_SWITCH_RAM(0);
    memcpy(color_options, "RGBS", 4);
    color_options[4] = color_enabled;
    color_options[5] = ~color_options[4];
}

void color_request(uint8_t channel) NONBANKED {
    CAMERA_SWITCH_RAM(0);
    request[0] = 'R'; request[1] = 'G'; request[2] = 'B'; request[3] = '?';
    request[4] = 1;
    request[5] = channel;
    for (uint8_t i = 6; i != sizeof(request); i++) request[i] = 0;
    CAMERA_SWITCH_RAM(CAMERA_BANK_REGISTERS);
}

bool color_host_replied(void) NONBANKED {
    CAMERA_SWITCH_RAM(0);
    return request[6] == 'H' && request[7] == 'O' && request[8] == 'S' && request[9] == 'T' &&
           request[10] == 1 && request[11] == 0;
}

static uint8_t *marker(uint8_t slot) {
    CAMERA_SWITCH_RAM((slot >> 1) + 1);
    return (uint8_t *)(COLOR_MARKER_ADDRESS + ((slot & 1) ? 0x1000 : 0));
}

static uint16_t marker_id(uint8_t slot, uint8_t *channel) {
    uint8_t *data = marker(slot);
    if (memcmp(data, "RGB\x01", 4) || data[4] < 1 || data[4] > 3) return 0;
    *channel = data[4] - 1;
    return (uint16_t)data[5] | ((uint16_t)data[6] << 8);
}

void color_marker_clear(uint8_t slot) BANKED {
    memset(marker(slot), 0, COLOR_MARKER_SIZE);
}

bool color_group_read(uint8_t slot, uint8_t channels[COLOR_CHANNELS]) BANKED {
    if (!_is_COLOR || slot >= CAMERA_MAX_IMAGE_SLOTS) return false;
    uint8_t channel;
    uint16_t id = marker_id(slot, &channel);
    if (!id) return false;
    memset(channels, CAMERA_IMAGE_DELETED, COLOR_CHANNELS);
    for (uint8_t i = 0; i != images_taken(); i++) {
        uint8_t member = VECTOR_GET(used_slots, i);
        if (marker_id(member, &channel) != id) continue;
        if (channels[channel] != CAMERA_IMAGE_DELETED) return false;
        channels[channel] = member;
    }
    return channels[0] != CAMERA_IMAGE_DELETED && channels[1] != CAMERA_IMAGE_DELETED && channels[2] != CAMERA_IMAGE_DELETED;
}

void color_gallery_refresh(void) BANKED {
    uint8_t channels[COLOR_CHANNELS];
    color_gallery_count = 0;
    for (uint8_t i = 0; i != images_taken(); i++) {
        uint8_t slot = VECTOR_GET(used_slots, i);
        if (color_group_read(slot, channels) && slot != channels[0]) continue;
        color_gallery_slots[color_gallery_count++] = slot;
    }
}

static uint16_t next_group_id(void) {
    uint16_t id = 0;
    bool found;
    do {
        if (!++id) ++id;
        found = false;
        for (uint8_t slot = 0, channel; slot != CAMERA_MAX_IMAGE_SLOTS; slot++) {
            if (marker_id(slot, &channel) == id) { found = true; break; }
        }
    } while (found);
    return id;
}

static bool exposure(uint8_t channel) {
    color_request(channel);
    SHADOW.CAM_REG_CAPTURE = CAM_REG_CAPTURE = CAM00F_POSITIVE | CAM00F_CAPTURING;
    bool canceled = false;
    do {
        PROCESS_INPUT();
        if (KEY_PRESSED(J_B)) canceled = true;
    } while (image_is_capturing());
    return !canceled;
}

bool color_capture(void) BANKED {
    static const uint8_t * const prompts[COLOR_CHANNELS] = {
        "Red filter, then A", "Green filter, then A", "Blue filter, then A"
    };
    if (images_free() < COLOR_CHANNELS) {
        MessageBox("Need 3 free slots!");
        return false;
    }
    while (image_is_capturing());
    JOYPAD_RESET();
    bool filters = false;
    for (uint8_t channel = 1; channel <= COLOR_CHANNELS; channel++) {
        if (!filters) {
            if (!exposure(channel)) return false;
            filters = !color_host_replied();
        }
        if (filters) {
            if (MessageBox(prompts[channel - 1]) != MENU_RESULT_YES) return false;
            JOYPAD_RESET();
            if (!exposure(channel)) return false;
        }
        get_flipped_last_seen_image(OPTION(flip_live_view), true);
        color_buffer_write(COLOR_STAGE_BANK + channel - 1, 0, CAMERA_IMAGE_SIZE);
    }
    uint16_t id = next_group_id();
    for (uint8_t channel = 0; channel != COLOR_CHANNELS; channel++) {
        uint8_t slot = VECTOR_POP(free_slots);
        color_buffer_read(COLOR_STAGE_BANK + channel, 0, CAMERA_IMAGE_SIZE);
        CAMERA_SWITCH_RAM((slot >> 1) + 1);
        memcpy((slot & 1) ? image_second : image_first, frame_buffer, CAMERA_IMAGE_SIZE);
        protected_generate_thumbnail(slot);
        protected_capture_metadata_write(slot);
        uint8_t *data = marker(slot);
        memcpy(data, "RGB\x01", 4);
        data[4] = channel + 1;
        data[5] = id;
        data[6] = id >> 8;
        protected_modify_slot(slot, images_taken());
        VECTOR_ADD(used_slots, slot);
    }
    color_gallery_refresh();
    return true;
}

#endif
