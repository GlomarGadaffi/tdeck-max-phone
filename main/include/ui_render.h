#ifndef UI_RENDER_H
#define UI_RENDER_H

// Pure 1-bit drawing for the 240x320 e-paper: no ESP-IDF, no panel, no
// globals. Everything draws into a caller-owned framebuffer, so the same code
// runs on the device (epaper_display.cpp owns the real buffer) and in the
// host tests under test/.
//
// Framebuffer format is the panel's own: 30 bytes per row, 320 rows, MSB =
// leftmost pixel, bit SET = WHITE (the GDEQ031T10 convention). Portrait,
// no rotation (UI_DESIGN 0.1).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_W        240
#define UI_H        320
#define UI_STRIDE   (UI_W / 8)            // 30
#define UI_FB_SIZE  (UI_STRIDE * UI_H)    // 9600

void ui_clear(uint8_t *fb);
void ui_set_pixel(uint8_t *fb, int x, int y, bool black);
void ui_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool black);

// ── F_NUM: the 5x7 dialpad font (0-9 * # +) ──────────────────────────────
// True if F_NUM has a glyph for c. Must agree with s_keymap
// (tca8418_keypad.cpp) and is_dial_char() (app_main.cpp): a character the
// keypad can produce but the display cannot draw looks like a dropped key.
bool ui_num_has_glyph(char c);
// Draws s left to right from (x, y); a character with no glyph leaves a
// blank cell so the rest of the string does not shift. Stops at the panel
// edge. Pitch is 5*scale + spacing.
void ui_draw_num(uint8_t *fb, int x, int y, const char *s, int scale, int spacing);

// ── F_UI: the 8x16 ASCII font (UI_DESIGN 7, #37) ─────────────────────────
// Any byte outside 0x20-0x7E draws as '?', so an unexpected caller-ID byte
// is visible rather than silently missing. Clipped at the panel edge.
void ui_draw_text(uint8_t *fb, int x, int y, const char *s, int scale);
int  ui_text_width(const char *s, int scale);
// Horizontally centred on the panel.
void ui_draw_text_centred(uint8_t *fb, int y, const char *s, int scale);

#ifdef __cplusplus
}
#endif

#endif // UI_RENDER_H
