#include "Core/gb.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); exit(1); } } while (0)

static uint32_t pixels[160 * 144];
static uint8_t *sram;
static bool pending_capture;
static unsigned requests[4], invalid_requests;
static uint8_t requested_channel;
static const char *symbols_path;
static uint8_t camera_bank, sensor[54], locked_sensor[54];
static bool sensor_locked;
static uint8_t *wram;
static unsigned hicolor_address, palette_writes;
static const char *scenario = "normal";
static bool host_handshake = true;
static unsigned frame_number, prompt_frame, prompt_count, prompt_mask, message_address;
static uint8_t physical_channel, prompt_channel;
static bool prompt_pending, roll_full_message;
static unsigned first_active_frame;
static unsigned cancel_channel = 2;
static unsigned printed_green_rows;
static bool advance_entry;
static uint32_t transition_frames[100][160 * 144];
static unsigned transition_count;
static bool record_transition;

static void printed_image(GB_gameboy_t *gb, uint32_t *image, uint8_t height,
                          uint8_t top, uint8_t bottom, uint8_t exposure)
{
    (void)gb; (void)top; (void)bottom; (void)exposure;
    for (unsigned y = 0; y < height; y++) {
        bool matches = true;
        for (unsigned x = 16; x < 144; x++) {
            uint32_t expected = (((x - 16) / 16) & 2) ? 0xFFFFFF : 0;
            if (image[y * 160 + x] != expected) { matches = false; break; }
        }
        printed_green_rows += matches;
    }
}

static void be32(FILE *file, uint32_t value)
{
    fputc(value >> 24, file); fputc(value >> 16, file); fputc(value >> 8, file); fputc(value, file);
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t size)
{
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc;
}

static void chunk(FILE *file, const char *type, const uint8_t *data, size_t size)
{
    be32(file, size);
    fwrite(type, 1, 4, file);
    if (size) fwrite(data, 1, size, file);
    be32(file, crc_update(crc_update(0xFFFFFFFF, (const uint8_t *)type, 4), data, size) ^ 0xFFFFFFFF);
}

static void screenshot(const char *path)
{
    const size_t raw_size = 144 * (1 + 160 * 3);
    uint8_t *raw = malloc(raw_size), *encoded = malloc(raw_size + 32);
    CHECK(raw && encoded, "allocate PNG buffers");
    size_t offset = 0;
    uint32_t a = 1, b = 0;
    for (unsigned y = 0; y < 144; y++) {
        raw[offset++] = 0;
        for (unsigned x = 0; x < 160; x++) {
            uint32_t pixel = pixels[y * 160 + x];
            raw[offset++] = pixel >> 16; raw[offset++] = pixel >> 8; raw[offset++] = pixel;
        }
    }
    for (size_t i = 0; i < raw_size; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    offset = 0;
    encoded[offset++] = 0x78; encoded[offset++] = 0x01;
    for (size_t pos = 0; pos < raw_size;) {
        unsigned size = raw_size - pos > 65535 ? 65535 : raw_size - pos;
        encoded[offset++] = pos + size == raw_size;
        encoded[offset++] = size; encoded[offset++] = size >> 8;
        encoded[offset++] = ~size; encoded[offset++] = (~size) >> 8;
        memcpy(encoded + offset, raw + pos, size);
        pos += size; offset += size;
    }
    uint32_t adler = (b << 16) | a;
    encoded[offset++] = adler >> 24; encoded[offset++] = adler >> 16;
    encoded[offset++] = adler >> 8; encoded[offset++] = adler;
    FILE *file = fopen(path, "wb");
    CHECK(file, "open PNG output");
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const uint8_t header[] = {0, 0, 0, 160, 0, 0, 0, 144, 8, 2, 0, 0, 0};
    fwrite(signature, 1, sizeof(signature), file);
    chunk(file, "IHDR", header, sizeof(header));
    chunk(file, "IDAT", encoded, offset);
    chunk(file, "IEND", NULL, 0);
    CHECK(!ferror(file), "write complete PNG");
    fclose(file);
    free(raw); free(encoded);
}

static bool memory_write(GB_gameboy_t *gb, uint16_t address, uint8_t value)
{
    (void)gb;
    if (address >= 0x4000 && address < 0x6000) camera_bank = value;
    if (camera_bank == 16 && address >= 0xA000 && address < 0xA036) sensor[address - 0xA000] = value;
    if (address == 0xFF69 && hicolor_address && wram[hicolor_address - 0xC000]) {
        if ((GB_read_memory(gb, 0xFF41) & 3) != 0) {
            screenshot("build/tests/hblank-failure.png");
            fprintf(stderr, "Palette write at LY=%u STAT=%02x PC=%04x BCPS=%02x LYC=%u SVBK=%u\n",
                    GB_read_memory(gb, 0xFF44), GB_read_memory(gb, 0xFF41), GB_get_registers(gb)->pc,
                    GB_read_memory(gb, 0xFF68), GB_read_memory(gb, 0xFF45), GB_read_memory(gb, 0xFF70));
            CHECK(false, "HiColor palette writes stay inside HBlank");
        }
        CHECK(GB_read_memory(gb, 0xFF4D) & 0x80, "HiColor runs in double speed");
        palette_writes++;
    }
    return true;
}

static unsigned symbol(const char *wanted)
{
    FILE *file = fopen(symbols_path, "r");
    CHECK(file, "open ROM symbols");
    char line[256], name[128];
    unsigned address, bank;
    while (fgets(line, sizeof(line), file)) {
        if ((sscanf(line, "DEF %127s 0x%x", name, &address) == 2 ||
             sscanf(line, "%x:%x %127s", &bank, &address, name) == 3) && !strcmp(name, wanted)) {
            fclose(file);
            return address;
        }
    }
    fprintf(stderr, "Missing symbol: %s\n", wanted);
    exit(1);
}

static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    (void)gb;
    return (r << 16) | (g << 8) | b;
}

static void capture_request(GB_gameboy_t *gb)
{
    (void)gb;
    if (!strcmp(scenario, "gray") || !strcmp(scenario, "compat")) {
        requested_channel = 0;
        requests[0]++;
        pending_capture = true;
        return;
    }
    uint8_t *request = sram + 0xF0;
    if (memcmp(request, "RGB?", 4) || request[4] != 1 || request[5] > 3) {
        invalid_requests++;
        requested_channel = 0;
    } else {
        for (unsigned i = 6; i < 16; i++) CHECK(request[i] == 0, "reply and reserved bytes cleared before each exposure");
        requested_channel = request[5];
        requests[requested_channel]++;
        if (requested_channel) {
            if (sensor_locked) CHECK(!memcmp(sensor + 1, locked_sensor + 1, 53), "sensor settings locked across RGB exposures");
            else { memcpy(locked_sensor, sensor, sizeof(sensor)); sensor_locked = true; }
        }
        if (host_handshake || !strcmp(scenario, "bad-version")) {
            memcpy(request + 6, "HOST", 4);
            request[10] = host_handshake ? 1 : 2;
            request[11] = 0;
        }
    }
    pending_capture = true;
}

static uint8_t camera_pixel(GB_gameboy_t *gb, uint8_t x, uint8_t y)
{
    (void)gb;
    (void)y;
    uint8_t bar = x / 16;
    uint8_t r = (bar & 4) ? 255 : 0;
    uint8_t g = (bar & 2) ? 255 : 0;
    uint8_t b = (bar & 1) ? 255 : 0;
    switch (host_handshake ? requested_channel : physical_channel) {
        case 1: return r;
        case 2: return g;
        case 3: return b;
        default: return ((unsigned)r + g + b) / 3;
    }
}

static uint8_t memory_read(GB_gameboy_t *gb, uint16_t address, uint8_t value)
{
    if (!strcmp(scenario, "fresh-sensor") && !requests[0] && camera_bank == 0 &&
        address >= 0xA100 && address < 0xAF00) return 0x22;
    GB_registers_t *regs = GB_get_registers(gb);
    if (advance_entry && address == 0x100 && (regs->pc == address || regs->pc == address + 1)) {
        // The shared CGB bootstrap needs Advance's B=1 entry convention.
        regs->bc |= 0x100;
        advance_entry = false;
    }
    if (address != message_address || (regs->pc != address && regs->pc != address + 1)) return value;
    char text[64];
    unsigned i;
    for (i = 0; i < sizeof(text) - 1; i++) {
        text[i] = GB_safe_read_memory(gb, regs->de + i);
        if (!text[i]) break;
    }
    text[i] = 0;
    static const char *prompts[] = {"Red filter, then A", "Green filter, then A", "Blue filter, then A"};
    for (i = 0; i < 3; i++) {
        if (strcmp(text, prompts[i])) continue;
        CHECK(i == prompt_count, "filter prompts appear in red, green, blue order");
        prompt_count++;
        prompt_mask |= 1 << i;
        physical_channel = prompt_channel = i + 1;
        prompt_frame = frame_number;
        prompt_pending = true;
        fprintf(stderr, "PROMPT: %s\n", text);
    }
    if (!strcmp(text, "Need 3 free slots!")) {
        roll_full_message = prompt_pending = true;
        prompt_channel = 0;
        prompt_frame = frame_number;
    }
    return value;
}

static void frames(GB_gameboy_t *gb, unsigned count)
{
    while (count--) {
        GB_run_frame(gb);
        frame_number++;
        if (record_transition && transition_count < 100)
            memcpy(transition_frames[transition_count++], pixels, sizeof(pixels));
        if (hicolor_address && wram[hicolor_address - 0xC000] && !first_active_frame) first_active_frame = frame_number;
        if (pending_capture) {
            pending_capture = false;
            GB_camera_updated(gb);
        }
        if (prompt_pending) {
            GB_key_t key = (!strncmp(scenario, "cancel", 6) && prompt_channel == cancel_channel) ? GB_KEY_B : GB_KEY_A;
            if (frame_number == prompt_frame + 40 && prompt_channel) {
                char path[80];
                snprintf(path, sizeof(path), "build/tests/filter-%u.png", prompt_channel);
                screenshot(path);
            }
            if (frame_number == prompt_frame + 60) GB_set_key_state(gb, key, true);
            if (frame_number >= prompt_frame + 63) {
                GB_set_key_state(gb, key, false);
                prompt_pending = false;
            }
        }
    }
}

static void press(GB_gameboy_t *gb, GB_key_t key)
{
    GB_set_key_state(gb, key, true);
    frames(gb, 3);
    GB_set_key_state(gb, key, false);
    frames(gb, 90);
}

static void empty_roll(void)
{
    for (unsigned echo = 0; echo < 2; echo++) {
        uint8_t *vector = sram + 0x11B2 + echo * 37;
        memset(vector, 255, 30);
        if (!strcmp(scenario, "no-space")) {
            for (unsigned slot = 0; slot < 28; slot++) vector[slot] = slot;
        }
        memcpy(vector + 30, "Magic", 5);
        uint8_t sum = 0x2F, xor = 0x15;
        for (unsigned slot = 0; slot < 30; slot++) { sum += vector[slot]; xor ^= vector[slot]; }
        vector[35] = sum;
        vector[36] = xor;
    }
    if (strcmp(scenario, "normal") && strcmp(scenario, "gray")) {
        memcpy(sram + 11 * 0x2000 + 0xFEB, "RGB\x01\x01\x01\x00", 7);
    }
}

static unsigned photo_count(void)
{
    unsigned count = 0;
    for (unsigned slot = 0; slot < 30; slot++) count += sram[0x11B2 + slot] < 30;
    return count;
}

static void checksum(unsigned address, unsigned length)
{
    uint8_t sum = 0x2F, xor = 0x15;
    for (unsigned i = 0; i < length; i++) { sum += sram[address + i]; xor ^= sram[address + i]; }
    CHECK(!memcmp(sram + address + length, "Magic", 5), "stock checksum magic");
    CHECK(sram[address + length + 5] == sum && sram[address + length + 6] == xor, "stock additive and XOR checksums");
}

static void stock_checksums(void)
{
    checksum(0x1000, 0xD2); checksum(0x1000 + 0xD9, 0xD2);
    checksum(0x11B2, 30); checksum(0x11D7, 30);
    checksum(0x2FB8, 18); checksum(0x2FD1, 18);
    for (unsigned bank = 1; bank < 16; bank++) {
        for (unsigned half = 0; half < 2; half++) {
            unsigned address = bank * 0x2000 + half * 0x1000 + 0xF00;
            checksum(address, 0x55);
            checksum(address + 0x5C, 0x55);
            CHECK(!memcmp(sram + address, sram + address + 0x5C, 0x5C), "image metadata echo");
        }
    }
}

static uint8_t stored_pixel(unsigned slot, unsigned x, unsigned y)
{
    unsigned base = ((slot >> 1) + 1) * 0x2000 + (slot & 1) * 0x1000;
    unsigned offset = (y / 8 * 16 + x / 8) * 16 + (y & 7) * 2;
    unsigned bit = 7 - (x & 7);
    return ((sram[base + offset] >> bit) & 1) | (((sram[base + offset + 1] >> bit) & 1) << 1);
}

static void color_photos(void)
{
    unsigned channels = 0;
    uint16_t group = 0;
    CHECK(photo_count() == 3, "one color photo occupies three stock slots");
    for (unsigned slot = 0; slot < 30; slot++) {
        if (sram[0x11B2 + slot] >= 30) continue;
        unsigned base = ((slot >> 1) + 1) * 0x2000 + (slot & 1) * 0x1000;
        uint8_t *marker = sram + base + 0xFEB;
        CHECK(!memcmp(marker, "RGB\x01", 4), "color marker magic and version");
        CHECK(marker[4] >= 1 && marker[4] <= 3, "color channel marker");
        unsigned channel = marker[4] - 1;
        CHECK(!(channels & (1 << channel)), "exactly one image per channel");
        channels |= 1 << channel;
        uint16_t id = marker[5] | (marker[6] << 8);
        CHECK(id >= (!strcmp(scenario, "fresh-color") ? 1u : 2u) && (!group || group == id), "shared group ID avoids a marker in a deleted slot");
        group = id;
        for (unsigned bar = 0; bar < 8; bar++) {
            uint8_t level = (bar & (4 >> channel)) ? 0 : 3;
            for (unsigned y = 8; y < 104; y += 8) {
                CHECK(stored_pixel(slot, bar * 16 + 8, y) == level, "saved channel matches fixed RGB color bars");
            }
        }
    }
    CHECK(channels == 7, "complete RGB group");
    stock_checksums();
}

static void screen_bars(void)
{
    static const uint32_t expected[] = {0, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF};
    for (unsigned y = 16; y < 128; y++) {
        for (unsigned x = 16; x < 144; x++) {
            if (pixels[y * 160 + x] != expected[(x - 16) / 16]) {
                fprintf(stderr, "Pixel (%u,%u) = %06x, expected %06x\n", x, y, pixels[y * 160 + x], expected[(x - 16) / 16]);
                CHECK(false, "HiColor screenshot matches all eight RGB color bars");
            }
        }
    }
}

static void renderer_fixture(bool neutral)
{
    uint32_t random = 1;
    for (unsigned y = 0; y < 112; y++) {
        for (unsigned x = 0; x < 128; x++) {
            random = random * 1664525 + 1013904223;
            unsigned color = neutral ? (x / 32) * 21 : random >> 26;
            for (unsigned slot = 0; slot < 30; slot++) {
                if (sram[0x11B2 + slot] >= 30) continue;
                unsigned base = ((slot >> 1) + 1) * 0x2000 + (slot & 1) * 0x1000;
                unsigned channel = sram[base + 0xFEF] - 1;
                unsigned level = (color >> (4 - channel * 2)) & 3;
                unsigned offset = base + (y / 8 * 16 + x / 8) * 16 + (y & 7) * 2;
                unsigned mask = 0x80 >> (x & 7);
                for (unsigned bit = 0; bit < 2; bit++) {
                    sram[offset + bit] = (sram[offset + bit] & ~mask) | ((level & (1 << bit)) ? mask : 0);
                }
            }
        }
    }
}

static void renderer_pixels(GB_gameboy_t *gb, const unsigned bases[3], bool neutral)
{
    unsigned palette = symbol("_BkgPalette");
    uint64_t colors = 0;
    unsigned total_error = 0;
    for (unsigned y = 0; y < 112; y++) {
        for (unsigned x = 0; x < 128; x++) {
            uint32_t actual = pixels[(y + 16) * 160 + x + 16];
            if (neutral) {
                unsigned address = palette + (x / 32) * 2;
                unsigned color = GB_read_memory(gb, address) | (GB_read_memory(gb, address + 1) << 8);
                uint32_t expected = 0;
                for (unsigned channel = 0; channel < 3; channel++) {
                    unsigned component = (color >> (channel * 5)) & 31;
                    expected = (expected << 8) | (component << 3) | (component >> 2);
                }
                CHECK(actual == expected, "neutral RGB reproduces all four selected palette shades");
            } else {
                unsigned rendered = 0;
                for (unsigned channel = 0; channel < 3; channel++) {
                    unsigned component = (actual >> (16 - channel * 8)) & 255;
                    unsigned level = (component * 3 + 127) / 255;
                    unsigned slot = (bases[channel] / 0x2000 - 1) * 2 + ((bases[channel] & 0x1000) != 0);
                    int difference = (int)level - (3 - stored_pixel(slot, x, y));
                    total_error += difference * difference;
                    rendered = (rendered << 2) | level;
                }
                colors |= UINT64_C(1) << rendered;
            }
        }
    }
    if (!neutral) {
        unsigned unique = 0;
        while (colors) { unique += colors & 1; colors >>= 1; }
        CHECK(unique >= 16 && total_error < 6 * 128 * 112, "64-color approximation retains varied colors with bounded error");
        fprintf(stderr, "PASS: stress view has %u colors; squared level error %.2f per pixel\n", unique, total_error / (128.0 * 112));
    }
}

static void ui_pixels(const char *name, unsigned x0, unsigned y0, unsigned width, unsigned height,
                      unsigned crop_x, unsigned crop_y, unsigned crop_width, unsigned crop_height)
{
    char path[128];
    snprintf(path, sizeof(path), "obj/gbc/%s-expected.rgb", name);
    FILE *file = fopen(path, "rb");
    CHECK(file, "open generated UI reference");
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
        uint8_t expected[3];
        CHECK(fread(expected, 1, 3, file) == 3, "read UI reference pixel");
        if (x < crop_x || x >= crop_x + crop_width || y < crop_y || y >= crop_y + crop_height) continue;
        uint32_t want = (expected[0] << 16) | (expected[1] << 8) | expected[2];
        CHECK((pixels[(y0+y)*160+x0+x] & 0xFFFFFF) == want, "UI preserves every authored color pixel");
    }
    CHECK(fgetc(file) == EOF, "UI reference has exact dimensions");
    fclose(file);
}

static void transition(GB_gameboy_t *gb, GB_key_t key, const char *name)
{
    uint32_t before[160 * 144];
    memcpy(before, pixels, sizeof(before));
    transition_count = 0;
    record_transition = true;
    press(gb, key);
    record_transition = false;
    unsigned bad = 0;
    for (unsigned f = 0; f < transition_count; f++) {
        unsigned old = 31, next = 31;
        bool black = true;
        for (unsigned y = 0; y < 128; y++) for (unsigned x = 0; x < 160; x++) {
            /* The hub's hand animates independently of its background. */
            if (y >= 56 && y < 96) continue;
            if (strstr(name, "flash") && x >= 32 && x < 50 && y >= 24 && y < 42) continue;
            if (strstr(name, "camera") && x >= 8 && x < 152 && y >= 8) continue;
            unsigned p = y * 160 + x;
            unsigned mask = 0x7FFF;
            for (unsigned step = 0; step < 5; step++) {
                unsigned r = mask & 31, g = (mask >> 5) & 31, b = mask >> 10;
                uint32_t rgb_mask = (((r << 3) | (r >> 2)) << 16) |
                                    (((g << 3) | (g >> 2)) << 8) | (b << 3) | (b >> 2);
                if (transition_frames[f][p] != (before[p] & rgb_mask)) old &= ~(1u << step);
                if (transition_frames[f][p] != (pixels[p] & rgb_mask)) next &= ~(1u << step);
                mask = step == 3 ? 0 : (mask & ~0x420u) >> 1;
            }
            black &= (transition_frames[f][p] & 0xFFFFFF) == 0;
        }
        if (!old && !next && !black) bad++;
        if (getenv("PHOTO_TRANSITION_FRAMES")) {
            uint32_t after[160 * 144];
            memcpy(after, pixels, sizeof(after));
            memcpy(pixels, transition_frames[f], sizeof(pixels));
            char path[160];
            snprintf(path, sizeof(path), "build/tests/%s-%03u.png", name, f);
            screenshot(path);
            memcpy(pixels, after, sizeof(pixels));
        }
    }
    fprintf(stderr, "%s: %u mixed frames out of %u\n", name, bad, transition_count);
    CHECK(!bad, "scene transition shows only complete old/new screens or black");
}

static void transitions_test(GB_gameboy_t *gb)
{
    transition(gb, GB_KEY_START, "title-hub");
    transition(gb, GB_KEY_B, "hub-title");
    transition(gb, GB_KEY_START, "title-hub-again");
    for (unsigned i = 0; i < 4; i++) press(gb, GB_KEY_RIGHT);
    transition(gb, GB_KEY_A, "hub-about");
    transition(gb, GB_KEY_B, "about-hub");
    press(gb, GB_KEY_LEFT);
    transition(gb, GB_KEY_A, "hub-settings");
    transition(gb, GB_KEY_B, "settings-hub");
    for (unsigned i = 0; i < 3; i++) press(gb, GB_KEY_LEFT);
    transition(gb, GB_KEY_A, "hub-camera");
    CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 3, "transition enters camera");
    transition(gb, GB_KEY_B, "camera-hub");
    press(gb, GB_KEY_RIGHT);
    transition(gb, GB_KEY_A, "hub-gallery");
    CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 1, "transition enters gallery");
    transition(gb, GB_KEY_B, "gallery-hub");
    press(gb, GB_KEY_RIGHT);
    transition(gb, GB_KEY_A, "hub-flash");
    CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 4, "transition enters flash storage");
    transition(gb, GB_KEY_B, "flash-hub");
    fprintf(stderr, "PASS: consecutive scene-transition frames contain no partial redraws\n");
}

static void font_boundary_test(GB_gameboy_t *gb)
{
    uint8_t *rom = GB_get_direct_access(gb, GB_DIRECT_ACCESS_ROM, NULL, NULL);
    const char *names[] = {"_vwf_print_shift_char_right", "_vwf_print_shift_char_left", "_vwf_swap_tiles"};
    for (unsigned direction = 0; direction < 2; direction++) {
        unsigned start = symbol(names[direction]), length = symbol(names[direction + 1]) - start;
        uint8_t code[128];
        CHECK(length < sizeof(code), "glyph shifter fits relocation fixture");
        memcpy(code, rom + start, length);
        unsigned table = 0;
        for (unsigned i = 0; i + 14 < length; i++) {
            bool match = true;
            for (unsigned j = 0; j < 7; j++)
                match &= code[i + j * 2] == 0xCB && code[i + j * 2 + 1] == (direction ? 0x27 : 0x3F);
            if (match) { table = i + 14; break; }
        }
        CHECK(table, "locate all seven glyph shift instructions");
        for (unsigned shift = 0; shift < 8; shift++) {
            /* Force the computed jump to end at xx00 for every shift count. */
            unsigned target = 0x3C00 + shift * 2, relocated = target - table;
            memcpy(rom + relocated, code, length);
            unsigned patched = 0;
            for (unsigned i = 0; i + 2 < length; i++) {
                if (code[i] == 0x21 && (unsigned)(code[i + 1] | code[i + 2] << 8) == start + table) {
                    rom[relocated + i + 1] = target;
                    rom[relocated + i + 2] = target >> 8;
                    patched++;
                }
            }
            CHECK(patched == 1, "relocate the glyph jump-table address");
            rom[0x3D00] = 0x18; rom[0x3D01] = 0xFE;
            GB_write_memory(gb, 0xFFFF, 1);
            GB_write_memory(gb, 0xFF0F, 1);
            GB_run(gb);
            GB_write_memory(gb, 0xFFFF, 0);
            GB_write_memory(gb, 0x2000, 2);
            GB_write_memory(gb, 0xFF90, 2);
            GB_write_memory(gb, symbol("_vwf_current_rotate"), shift);
            for (unsigned y = 0; y < 8; y++) {
                GB_write_memory(gb, 0xC500 + y, 0x3C);
                GB_write_memory(gb, 0xC600 + y, 0xA5 + y);
            }
            GB_write_memory(gb, 0xDF00, 0); GB_write_memory(gb, 0xDF01, 0x3D);
            GB_write_memory(gb, 0xDF02, 5);
            GB_registers_t *registers = GB_get_registers(gb);
            registers->pc = relocated; registers->sp = 0xDF00;
            registers->bc = 0xC600; registers->de = 0xC500;
            frames(gb, 1);
            if (registers->pc != 0x3D00 || registers->sp != 0xDF03)
                fprintf(stderr, "shifter=%u shift=%u PC=%04x SP=%04x\n", direction, shift, registers->pc, registers->sp);
            CHECK(registers->pc == 0x3D00 && registers->sp == 0xDF03,
                  "glyph shifter returns normally across a page boundary");
            CHECK(GB_read_memory(gb, 0xFF90) == 2, "glyph shifter restores its caller's ROM bank");
            for (unsigned y = 0; y < 8; y++) {
                uint8_t expected = (direction ? (0xA5 + y) << shift : (0xA5 + y) >> shift) ^ 0x3C;
                CHECK(GB_read_memory(gb, 0xC500 + y) == expected, "glyph shift preserves all eight rows");
            }
        }
    }
    fprintf(stderr, "PASS: both glyph shifters return correct pixels at every page-boundary offset\n");
}

static void ui_test(GB_gameboy_t *gb)
{
    screenshot("build/tests/ui-title.png");
    ui_pixels("title", 0, 0, 160, 128, 0, 0, 160, 128);
    press(gb, GB_KEY_START);
    screenshot("build/tests/ui-hub.png");
    uint8_t lcdc = GB_read_memory(gb, 0xFF40);
    GB_write_memory(gb, 0xFF40, lcdc & ~2);
    frames(gb, 2);
    ui_pixels("hub", 8, 24, 144, 48, 40, 0, 80, 16);
    ui_pixels("hub", 8, 24, 144, 48, 0, 16, 144, 32);
    GB_write_memory(gb, 0xFF40, lcdc);
    for (unsigned i = 4; i < 40; i++) CHECK(!GB_read_memory(gb, 0xFE00+i*4), "title sprites hidden in hub");
    press(gb, GB_KEY_B);
    ui_pixels("title", 0, 0, 160, 128, 0, 0, 160, 128);
    press(gb, GB_KEY_START);
    for (unsigned i = 0; i < 4; i++) press(gb, GB_KEY_RIGHT);
    press(gb, GB_KEY_A);
    screenshot("build/tests/ui-about.png");
    press(gb, GB_KEY_B);
    press(gb, GB_KEY_B);
    press(gb, GB_KEY_A);
    frames(gb, 120);
    CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 3, "camera entered after About");
    size_t size;
    uint8_t *vram = GB_get_direct_access(gb, GB_DIRECT_ACCESS_VRAM, &size, NULL);
    for (unsigned y = 1; y < 15; y++) for (unsigned x = 2; x < 18; x++)
        CHECK(vram[0x3800+y*32+x] == 0, "UI colors do not tint the live camera image");
    screenshot("build/tests/ui-camera.png");
    press(gb, GB_KEY_SELECT);
    screenshot("build/tests/ui-popup.png");
    press(gb, GB_KEY_B);
    unsigned previews = requests[0];
    frames(gb, 120);
    if (requests[0] == previews) {
        uint16_t rom_bank;
        GB_get_direct_access(gb, GB_DIRECT_ACCESS_ROM, NULL, &rom_bank);
        screenshot("build/tests/ui-stalled.png");
        fprintf(stderr, "preview stalled PC=%04x LCDC=%02x STAT=%02x HDMA=%02x camera=%02x\n",
                GB_get_registers(gb)->pc, GB_read_memory(gb, 0xFF40), GB_read_memory(gb, 0xFF41),
                GB_read_memory(gb, 0xFF55), sensor[0]);
        fprintf(stderr, "depth=%u faded=%u speed=%02x state=%u bank=%u sp=%04x\n",
                GB_read_memory(gb, symbol("_color_ui_depth")), GB_read_memory(gb, symbol("_palette_faded")),
                GB_read_memory(gb, 0xFF4D), GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")), rom_bank, GB_get_registers(gb)->sp);
    }
    CHECK(requests[0] > previews, "preview resumes after UI transitions");
    fprintf(stderr, "PASS: exact title and hub pixels, title restoration, About, camera and popup transitions\n");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 5 && argc <= 7, "usage: color_camera ROM SYMBOLS SAVE BOOT_ROM [SCENARIO] [cgb|agb|dmg]; see tests/sameboy/README.md");
    if (argc >= 6) scenario = argv[5];
    CHECK(!strcmp(scenario, "normal") || !strcmp(scenario, "color") || !strcmp(scenario, "filters") ||
          !strcmp(scenario, "cancel") || !strcmp(scenario, "no-space") || !strcmp(scenario, "bad-version") ||
          !strcmp(scenario, "compat") || !strcmp(scenario, "gray") || !strcmp(scenario, "fresh") ||
          !strcmp(scenario, "fresh-zero") || !strcmp(scenario, "fresh-color") || !strcmp(scenario, "fresh-sensor") || !strcmp(scenario, "stress") ||
          !strcmp(scenario, "cancel-red") || !strcmp(scenario, "cancel-blue") || !strcmp(scenario, "ui") || !strcmp(scenario, "transitions") || !strcmp(scenario, "font-boundary") ||
          !strcmp(scenario, "shuffled") || !strcmp(scenario, "incomplete") || !strcmp(scenario, "duplicate") || !strcmp(scenario, "neutral"), "known scenario");
    if (!strcmp(scenario, "cancel-red")) cancel_channel = 1;
    if (!strcmp(scenario, "cancel-blue")) cancel_channel = 3;
    host_handshake = strcmp(scenario, "filters") && strncmp(scenario, "cancel", 6) && strcmp(scenario, "bad-version");
    symbols_path = argv[2];
    GB_model_t model = GB_MODEL_CGB_E;
    if (argc == 7) {
        CHECK(!strcmp(argv[6], "cgb") || !strcmp(argv[6], "agb") || !strcmp(argv[6], "dmg"), "known hardware model");
        if (!strcmp(argv[6], "agb")) model = GB_MODEL_AGB_A;
        if (!strcmp(argv[6], "dmg")) model = GB_MODEL_DMG_B;
    }
    GB_gameboy_t *gb = GB_init(GB_alloc(), model);
    advance_entry = model == GB_MODEL_AGB_A;
    CHECK(gb, "allocate SameBoy");
    GB_set_pixels_output(gb, pixels);
    GB_set_rgb_encode_callback(gb, rgb);
    GB_set_color_correction_mode(gb, GB_COLOR_CORRECTION_DISABLED);
    FILE *rom = fopen(argv[1], "rb");
    CHECK(rom && !fseek(rom, 0, SEEK_END), "open ROM for flash-layout check");
    CHECK(ftell(rom) == 0x20000, "128 KiB ROM preserves the existing flash-roll layout");
    fclose(rom);
    CHECK(GB_load_rom(gb, argv[1]) == 0, "load ROM");
    CHECK(GB_load_battery(gb, argv[3]) == 0, "load disposable save");
    CHECK(GB_load_boot_rom(gb, argv[4]) == 0, "load open-source boot ROM");
    size_t size;
    sram = GB_get_direct_access(gb, GB_DIRECT_ACCESS_CART_RAM, &size, NULL);
    CHECK(sram && size >= 0x20000, "128 KiB camera SRAM");
    uint8_t original_sram[0x20000];
    if (!strcmp(scenario, "compat")) memcpy(original_sram, sram, sizeof(original_sram));
    else if (!strncmp(scenario, "fresh", 5)) memset(sram, !strcmp(scenario, "fresh-zero") ? 0 : 0xFF, 0x20000);
    else empty_roll();
    uint8_t original_request[16];
    memcpy(original_request, sram + 0xF0, sizeof(original_request));
    wram = GB_get_direct_access(gb, GB_DIRECT_ACCESS_RAM, &size, NULL);
    CHECK(wram && size >= (model == GB_MODEL_DMG_B ? 0x2000 : 0x8000), "hardware WRAM size");
    GB_set_camera_update_request_callback(gb, capture_request);
    GB_set_camera_get_pixel_callback(gb, camera_pixel);
    GB_set_write_memory_callback(gb, memory_write);
    message_address = symbol("_MessageBox");
    GB_set_read_memory_callback(gb, memory_read);
    frames(gb, 600);
    if (!strcmp(scenario, "font-boundary")) {
        font_boundary_test(gb);
        GB_free(gb); GB_dealloc(gb);
        return 0;
    }
    if (!strncmp(scenario, "fresh", 5)) {
        screenshot("build/tests/fresh-boot.png");
        CHECK(photo_count() == 0, "fresh SRAM starts with an empty camera roll");
        CHECK(!GB_read_memory(gb, symbol("_protected_status")), "fresh SRAM is initialized without reporting corruption");
        CHECK(GB_read_memory(gb, symbol("_color_enabled")), "Color defaults to On for new settings");
        unsigned options = symbol("_camera_state");
        CHECK((GB_read_memory(gb, options + 4) & 0x51) == 0x51,
              "fresh settings enable fast printing, IR shutter, and double speed");
        CHECK(GB_read_memory(gb, options + 7) & 0x20, "fresh settings enable CGB DMA");
        CHECK(GB_read_memory(gb, 0xFF4D) & 0x80, "fresh settings apply double speed at boot");
    }
    if (model != GB_MODEL_DMG_B && strcmp(scenario, "compat") && strcmp(scenario, "fresh-color") && strcmp(scenario, "ui")) GB_write_memory(gb, symbol("_color_enabled"), 0);
    if (model == GB_MODEL_AGB_A) CHECK(GB_read_memory(gb, symbol("__is_ADVANCE")), "ROM detects Advance entry registers on AGB model");
    if (GB_read_memory(gb, symbol("_OLD_PROGRAM_STATE")) >= 5) press(gb, GB_KEY_START);
    if (!strcmp(scenario, "transitions")) {
        transitions_test(gb);
        GB_free(gb); GB_dealloc(gb);
        return 0;
    }
    if (!strcmp(scenario, "ui")) {
        ui_test(gb);
        GB_free(gb); GB_dealloc(gb);
        return 0;
    }
    if (!strcmp(scenario, "compat")) {
        press(gb, GB_KEY_START);
        press(gb, GB_KEY_RIGHT);
        press(gb, GB_KEY_A);
        CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 1, "old release gallery entered");
        CHECK(GB_read_memory(gb, symbol("_used_slots")) == 3, "unmodified release lists three ordinary photos");
        for (unsigned channel = 1; channel <= 3; channel++) {
            char path[80];
            snprintf(path, sizeof(path), "build/tests/legacy-%u.png", channel);
            screenshot(path);
            press(gb, GB_KEY_RIGHT);
        }
        stock_checksums();
        CHECK(!memcmp(original_sram, sram, sizeof(original_sram)), "old release leaves all SRAM unchanged while viewing color save");
        fprintf(stderr, "PASS: unmodified release lists three grayscale photos; all 128 KiB SRAM unchanged\n");
        GB_free(gb); GB_dealloc(gb);
        return 0;
    }
    press(gb, GB_KEY_A);
    frames(gb, 120);
    CHECK(invalid_requests == 0, "preview exposure contains RGB? version 1 request");
    CHECK(requests[0] > 0, "ordinary brightness preview observed");
    CHECK(requests[1] + requests[2] + requests[3] == 0, "Color off does not request an RGB exposure");
    fprintf(stderr, "PASS: %u ordinary preview exposures%s\n", requests[0], !strcmp(scenario, "gray") ? " on GB" : " request channel 0");
    if (strcmp(scenario, "normal") && strcmp(scenario, "gray") && strcmp(scenario, "fresh") && strcmp(scenario, "fresh-zero") && strcmp(scenario, "fresh-sensor")) {
        if (strcmp(scenario, "fresh-color")) {
            press(gb, GB_KEY_SELECT);
            for (unsigned i = 0; i < 4; i++) press(gb, GB_KEY_DOWN);
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_DOWN);
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_SELECT);
            press(gb, GB_KEY_DOWN);
            press(gb, GB_KEY_A);
            CHECK(GB_read_memory(gb, symbol("_color_enabled")), "Color setting enabled through camera menu");
        }
        uint8_t *before = malloc(15 * 0x2000);
        CHECK(before, "allocate SRAM snapshot");
        memcpy(before, sram + 0x2000, 15 * 0x2000);
        press(gb, GB_KEY_A);
        frames(gb, 1000);
        if (!strncmp(scenario, "cancel", 6) || !strcmp(scenario, "no-space")) {
            CHECK(!memcmp(before, sram + 0x2000, 15 * 0x2000), "canceled or rejected capture leaves all image SRAM unchanged");
            if (!strncmp(scenario, "cancel", 6)) CHECK(prompt_count == cancel_channel && photo_count() == 0, "B cancels the complete color group at the requested prompt");
            else CHECK(roll_full_message && photo_count() == 28 && requests[1] + requests[2] + requests[3] == 0, "fewer than three slots rejected before RGB exposure");
            fprintf(stderr, "PASS: %s leaves all image SRAM unchanged\n", scenario);
            free(before);
            GB_free(gb); GB_dealloc(gb);
            return 0;
        }
        CHECK(!memcmp(before + 0x2FF2, sram + 0x4FF2, 14) && !memcmp(before + 0xFFF2, sram + 0x11FF2, 14), "RGB markers preserve both calibration records");
        free(before);
        if (host_handshake) CHECK(requests[1] == 1 && requests[2] == 1 && requests[3] == 1, "red, green and blue requested once");
        else CHECK(prompt_mask == 7 && requests[1] == 2 && requests[2] == 1 && requests[3] == 1, "unacknowledged probe discarded before all three filter exposures");
        color_photos();
        unsigned previews = requests[0];
        frames(gb, 180);
        CHECK(requests[0] > previews, "live preview resumes after color capture");
        fprintf(stderr, "PASS: RGB exposures, locked settings, stock photos, group markers and checksums\n");
        CHECK(GB_save_battery(gb, "build/tests/color.sav") == 0, "save color fixture");
        if (!strcmp(scenario, "fresh-color")) {
            GB_reset(gb);
            CHECK(GB_load_battery(gb, "build/tests/color.sav") == 0, "reload color photo after power cycle");
            frames(gb, 600);
            CHECK(photo_count() == 3 && GB_read_memory(gb, symbol("_color_enabled")), "fresh color photo and default Color On survive power cycle");
            CHECK(!GB_read_memory(gb, symbol("_protected_status")), "color save survives power cycle without repairs");
            stock_checksums();
            fprintf(stderr, "PASS: fresh Color-On capture, responsive preview, and color save power cycle\n");
            GB_free(gb); GB_dealloc(gb);
            return 0;
        }
        bool renderer_test = !strcmp(scenario, "stress") || !strcmp(scenario, "neutral");
        if (renderer_test) renderer_fixture(!strcmp(scenario, "neutral"));
        unsigned bases[3];
        for (unsigned slot = 0; slot < 30; slot++) {
            if (sram[0x11B2 + slot] >= 30) continue;
            unsigned base = ((slot >> 1) + 1) * 0x2000 + (slot & 1) * 0x1000;
            bases[sram[base + 0xFEF] - 1] = base;
        }
        if (!strcmp(scenario, "shuffled")) {
            uint8_t record[0x1000];
            memcpy(record, sram + bases[0], sizeof(record));
            memcpy(sram + bases[0], sram + bases[2], sizeof(record));
            memcpy(sram + bases[2], record, sizeof(record));
        }
        if (!strcmp(scenario, "incomplete")) sram[bases[2] + 0xFEF] = 0;
        if (!strcmp(scenario, "duplicate")) sram[bases[2] + 0xFEF] = 1;
        hicolor_address = symbol("_hicolor_active");
        press(gb, GB_KEY_B);
        press(gb, GB_KEY_RIGHT);
        unsigned conversion_frame = frame_number;
        press(gb, GB_KEY_A);
        frames(gb, 1000);
        CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 1, "gallery entered through main menu");
        if (!strcmp(scenario, "incomplete") || !strcmp(scenario, "duplicate")) {
            CHECK(GB_read_memory(gb, symbol("_color_gallery_count")) == 3, "incomplete or ambiguous markers remain three grayscale photos");
            CHECK(!wram[hicolor_address - 0xC000], "invalid group does not enable HiColor");
            fprintf(stderr, "PASS: %s markers remain independent grayscale photos\n", scenario);
            GB_free(gb); GB_dealloc(gb);
            return 0;
        }
        CHECK(GB_read_memory(gb, symbol("_color_gallery_count")) == 1, "gallery groups RGB slots into one photo");
        if (!wram[hicolor_address - 0xC000]) {
            screenshot("build/tests/before-hicolor.png");
            fprintf(stderr, "Conversion pending at frame %u: PC=%04x ROM=%u KEY1=%02x deferred=%u first-active=%u\n",
                    frame_number - conversion_frame, GB_get_registers(gb)->pc, GB_read_memory(gb, 0xFF90),
                    GB_read_memory(gb, 0xFF4D), wram[symbol("_hicolor_deferred") - 0xC000], first_active_frame);
        }
        CHECK(wram[hicolor_address - 0xC000], "HiColor view active");
        CHECK(palette_writes > 1000, "actual palette streaming observed");
        screenshot("build/tests/hicolor.png");
        fprintf(stderr, "Conversion took %u frames\n", first_active_frame - conversion_frame);
        if (renderer_test) renderer_pixels(gb, bases, !strcmp(scenario, "neutral"));
        else screen_bars();
        CHECK(first_active_frame && first_active_frame - conversion_frame < 300, "on-device conversion finishes within five seconds");
        fprintf(stderr, "PASS: conversion in %u frames; %u HiColor palette writes in HBlank at double speed; saved hicolor.png\n",
                first_active_frame - conversion_frame, palette_writes);
        press(gb, GB_KEY_SELECT);
        CHECK(!wram[hicolor_address - 0xC000], "HiColor stops for gallery menu");
        screenshot("build/tests/gallery-menu.png");
        unsigned cache_start = frame_number;
        press(gb, GB_KEY_B);
        CHECK(wram[hicolor_address - 0xC000], "cached HiColor restores after closing menu");
        CHECK(frame_number - cache_start < 120, "cached redisplay avoids full conversion");
        if (!renderer_test) screen_bars();
        if (!strcmp(scenario, "color")) {
            GB_connect_printer(gb, printed_image, NULL);
            press(gb, GB_KEY_SELECT);
            press(gb, GB_KEY_DOWN);
            press(gb, GB_KEY_A);
            frames(gb, 2000);
            CHECK(printed_green_rows >= 112, "real printer packets contain the green channel");
            CHECK(wram[hicolor_address - 0xC000], "HiColor restored after printing");
            fprintf(stderr, "PASS: printer received %u rows from green channel\n", printed_green_rows);
            press(gb, GB_KEY_SELECT);
            for (unsigned i = 0; i < 5; i++) press(gb, GB_KEY_DOWN);
            press(gb, GB_KEY_A);
            screenshot("build/tests/delete-group.png");
            CHECK(photo_count() == 3, "delete confirmation leaves group intact");
            press(gb, GB_KEY_A);
            CHECK(photo_count() == 3, "group deletion defaults to No");
            press(gb, GB_KEY_A);
            frames(gb, 1000);
            CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 2, "thumbnail view entered");
            CHECK(wram[hicolor_address - 0xC000], "color thumbnail uses HiColor");
            screenshot("build/tests/thumbnails.png");
            press(gb, GB_KEY_A);
            CHECK(GB_read_memory(gb, symbol("_selected_images")), "one logical color thumbnail selected");
            screenshot("build/tests/selected-thumbnail.png");
            press(gb, GB_KEY_SELECT);
            press(gb, GB_KEY_A);
            screenshot("build/tests/delete-selected.png");
            press(gb, GB_KEY_B);
            CHECK(photo_count() == 3, "canceling thumbnail deletion preserves all channels");
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_SELECT);
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_RIGHT);
            press(gb, GB_KEY_A);
            frames(gb, 300);
            CHECK(photo_count() == 0 && GB_read_memory(gb, symbol("_color_gallery_count")) == 0, "confirmed thumbnail deletion removes all three channels");
            CHECK(!wram[hicolor_address - 0xC000], "empty gallery restores normal LCD operation");
            stock_checksums();
            fprintf(stderr, "PASS: color thumbnails, selection, default-No and canceled deletion, whole-group deletion\n");
            press(gb, GB_KEY_B);
            press(gb, GB_KEY_B);
            press(gb, GB_KEY_LEFT);
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_SELECT);
            press(gb, GB_KEY_A);
            CHECK(!GB_read_memory(gb, symbol("_color_enabled")), "Color switched off through camera popup");
            press(gb, GB_KEY_A);
            press(gb, GB_KEY_A);
            CHECK(photo_count() == 1, "ordinary capture reuses one deleted RGB slot");
            for (unsigned slot = 0; slot < 30; slot++) {
                if (sram[0x11B2 + slot] >= 30) continue;
                unsigned base = ((slot >> 1) + 1) * 0x2000 + (slot & 1) * 0x1000;
                static const uint8_t cleared[7] = {0};
                CHECK(!memcmp(sram + base + 0xFEB, cleared, 7), "ordinary overwrite clears the old color marker");
            }
            stock_checksums();
            fprintf(stderr, "PASS: ordinary overwrite clears a deleted group's marker\n");
        }
    } else {
        press(gb, GB_KEY_A);
        press(gb, GB_KEY_A);
        CHECK(photo_count() == 1, "Color off saves one ordinary photo");
        CHECK(requests[1] + requests[2] + requests[3] == 0, "ordinary shutter requests only channel 0");
        stock_checksums();
        if (!strcmp(scenario, "gray")) CHECK(!memcmp(original_request, sram + 0xF0, sizeof(original_request)), "GB leaves the unused handshake area unchanged");
        fprintf(stderr, "PASS: ordinary photo and stock checksums\n");
        unsigned previews = requests[0];
        frames(gb, 180);
        CHECK(requests[0] > previews, "live preview continues after saving an ordinary photo");
        press(gb, GB_KEY_B);
        press(gb, GB_KEY_RIGHT);
        press(gb, GB_KEY_A);
        CHECK(GB_read_memory(gb, symbol("_CURRENT_PROGRAM_STATE")) == 1, "camera responds after saving and opens the gallery");
        screenshot("build/tests/ordinary-saved.png");
        CHECK(GB_save_battery(gb, "build/tests/ordinary-saved.sav") == 0, "persist ordinary photo for a power cycle");
        GB_reset(gb);
        CHECK(GB_load_battery(gb, "build/tests/ordinary-saved.sav") == 0, "reload saved battery after power cycle");
        frames(gb, 600);
        CHECK(photo_count() == 1 && !GB_read_memory(gb, symbol("_protected_status")), "ordinary photo survives power cycle with valid checksums");
        stock_checksums();
        fprintf(stderr, "PASS: preview and menus respond after save; photo survives a power cycle\n");
    }
    GB_free(gb);
    GB_dealloc(gb);
    return 0;
}
