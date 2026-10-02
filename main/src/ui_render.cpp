// Pure 1-bit drawing for the e-paper UI -- see ui_render.h. No ESP-IDF
// headers on purpose: test/ compiles this file on the host.
#include "ui_render.h"
#include "font_ui_8x16.h"
#include <string.h>

void ui_clear(uint8_t *fb)
{
    memset(fb, 0xFF, UI_FB_SIZE);
}

void ui_set_pixel(uint8_t *fb, int x, int y, bool black)
{
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) return;
    uint8_t *byte = &fb[y * UI_STRIDE + (x / 8)];
    uint8_t mask = (uint8_t)(0x80 >> (x % 8));
    if (black) *byte &= (uint8_t)~mask;
    else *byte |= mask;
}

void ui_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool black)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            ui_set_pixel(fb, x + i, y + j, black);
}

// ── F_NUM ────────────────────────────────────────────────────────────────
// Compact 5x7 dialpad font, MSB of each row byte = leftmost column. Covers
// exactly the characters a dial buffer can contain: 0-9 plus * # +. Indexed
// through num_glyph_index() rather than `c - '0'`, because the set is not
// contiguous.
//
// * and # matter because they are dialable: *777 is the echo test, and star
// codes are how most PBXes expose features. + is here only because it is the
// printed legend on the O key and costs one glyph; neither drawbridge nor the
// 3CX anchor consumes it today.
#define NUM_GLYPHS 13
static const uint8_t s_num_font[NUM_GLYPHS][7] = {
    {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}, // 0
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70}, // 1
    {0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8}, // 2
    {0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70}, // 3
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10}, // 4
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70}, // 5
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}, // 6
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40}, // 7
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70}, // 8
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}, // 9
    // '*' -- a six-point asterisk sitting high in the cell, the way a
    // typographic asterisk does. Rows 5-6 blank so it never reads as a plus.
    {0x20, 0xA8, 0x70, 0xF8, 0x70, 0xA8, 0x20}, // *
    // '#' -- two verticals crossed by two horizontals, full cell width.
    {0x50, 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x50}, // #
    // '+' -- centred, deliberately shorter than '#' so the two don't confuse.
    {0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00}, // +
};

static int num_glyph_index(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case '*': return 10;
        case '#': return 11;
        case '+': return 12;
        default:  return -1;
    }
}

bool ui_num_has_glyph(char c)
{
    return num_glyph_index(c) >= 0;
}

static void draw_num_glyph(uint8_t *fb, int x, int y, int idx, int scale)
{
    if (idx < 0 || idx >= NUM_GLYPHS) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = s_num_font[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x80 >> col)) {
                ui_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

void ui_draw_num(uint8_t *fb, int x, int y, const char *s, int scale, int spacing)
{
    int cx = x;
    for (const char *p = s; p && *p; p++) {
        if (cx > UI_W - 5 * scale) break; // clip to panel width
        draw_num_glyph(fb, cx, y, num_glyph_index(*p), scale);
        cx += 5 * scale + spacing;
    }
}

// ── F_UI ─────────────────────────────────────────────────────────────────
static void draw_ui_glyph(uint8_t *fb, int x, int y, unsigned char c, int scale)
{
    if (c < FONT_UI_FIRST || c > FONT_UI_LAST) c = '?';
    const uint8_t *g = font_ui_8x16[c - FONT_UI_FIRST];
    for (int row = 0; row < FONT_UI_H; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_UI_W; col++) {
            if (bits & (0x80 >> col)) {
                ui_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

int ui_text_width(const char *s, int scale)
{
    return s ? (int)strlen(s) * FONT_UI_W * scale : 0;
}

void ui_draw_text(uint8_t *fb, int x, int y, const char *s, int scale)
{
    int cx = x;
    for (const char *p = s; p && *p; p++) {
        if (cx > UI_W - FONT_UI_W * scale) break; // whole glyphs only
        draw_ui_glyph(fb, cx, y, (unsigned char)*p, scale);
        cx += FONT_UI_W * scale;
    }
}

void ui_draw_text_centred(uint8_t *fb, int y, const char *s, int scale)
{
    int w = ui_text_width(s, scale);
    int x = (UI_W - w) / 2;
    if (x < 0) x = 0;
    ui_draw_text(fb, x, y, s, scale);
}
