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

// ── The UI model (#38, UI_DESIGN 7.1) ────────────────────────────────────
// Everything a screen shows, as data. The main loop fills one of these and
// posts it; the renderer turns it into a whole frame. The screen is an enum,
// so nothing string-matches a status text to choose a layout any more.
typedef enum {
    UI_IDLE,        // ready; number = redial target (may be empty)
    UI_DIALLING,    // number = dial buffer
    UI_CALLING,     // outgoing, waiting for answer; number = callee
    UI_INCOMING,    // ringing; number = caller ID
    UI_INCALL,      // connected; number = peer
    UI_ENDED,       // IDLE plus the result of the call that just ended (UI_DESIGN 2.4)
    // Not one of the six UI_DESIGN 4 screens: boot, no Wi-Fi, the power-off
    // prompt and the powered-off screen. Drawn on the same band grid with the
    // text supplied in notice_*.
    UI_NOTICE,
} ui_screen_t;

// The band grid (UI_DESIGN 4.0). Bands are full width and never overlap, so
// any one of them can be partially refreshed on its own (#42).
typedef enum { B_STATUS, B_LABEL, B_NUMBER, B_SUB, B_BODY, B_HINT, B_ALL } ui_band_t;

#define UI_TEXT_COLS 30   // F_UI at 1x across the panel

typedef struct {
    ui_screen_t screen;
    char        number[24];        // dial buffer, peer ext, or redial target
    char        self_ext[8];
    bool        registered, wifi_up, muted;
    uint8_t     volume;            // 0..100
    uint32_t    last_call_secs;    // UI_ENDED: duration of the call; 0 = none
    bool        last_call_failed;  // UI_ENDED: the call never connected
    // UI_NOTICE only.
    char        notice_label[16];
    char        notice_sub[UI_TEXT_COLS + 1];
    char        notice_hint[2][UI_TEXT_COLS + 1];
} ui_model_t;

// Rows of a band, inclusive. B_ALL is the whole panel.
void ui_band_rows(ui_band_t band, int *y0, int *y1);
const char *ui_band_name(ui_band_t band);
const char *ui_screen_name(ui_screen_t screen);

// Draw the whole frame for m into fb. Every band is always drawn, so the
// result does not depend on what was on the glass before; deciding which
// rows to send to the panel is the driver's job.
void ui_compose(uint8_t *fb, const ui_model_t *m);

// ── Drawing primitives ───────────────────────────────────────────────────
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
