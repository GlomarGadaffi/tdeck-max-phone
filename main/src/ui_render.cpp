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

// ── Band grid and screen composition (#38, UI_DESIGN 4) ──────────────────
static const struct { int y0, y1; const char *name; } s_bands[] = {
    {  0,  29, "STATUS" },   // identity + link state + volume; rule at y=28-29
    { 40,  79, "LABEL"  },   // state name, F_UI 2x
    { 88, 159, "NUMBER" },   // the number, F_NUM
    {168, 199, "SUB"    },   // secondary line
    {200, 271, "BODY"   },   // pictogram / cheat-sheet
    {272, 319, "HINT"   },   // rule at y=272-273, then two hint lines
};

void ui_band_rows(ui_band_t band, int *y0, int *y1)
{
    if (band >= B_STATUS && band < B_ALL) {
        *y0 = s_bands[band].y0;
        *y1 = s_bands[band].y1;
    } else {
        *y0 = 0;
        *y1 = UI_H - 1;
    }
}

const char *ui_band_name(ui_band_t band)
{
    return (band >= B_STATUS && band < B_ALL) ? s_bands[band].name : "ALL";
}

const char *ui_screen_name(ui_screen_t screen)
{
    switch (screen) {
        case UI_IDLE:     return "IDLE";
        case UI_DIALLING: return "DIALLING";
        case UI_CALLING:  return "CALLING";
        case UI_INCOMING: return "INCOMING";
        case UI_INCALL:   return "IN CALL";
        case UI_ENDED:    return "ENDED";
        case UI_NOTICE:   return "NOTICE";
    }
    return "?";
}

static bool all_num_glyphs(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++)
        if (!ui_num_has_glyph(*p)) return false;
    return true;
}

// B_STATUS: fixed pixel columns (UI_DESIGN 4.6), F_UI 1x, 2 px rule.
static void draw_status(uint8_t *fb, const ui_model_t *m)
{
    const int y = 6;
    ui_draw_text(fb, 4, y, m->self_ext, 1);
    ui_draw_text(fb, 48, y, m->registered ? "REG" : "NOREG", 1);
    ui_draw_text(fb, 104, y, m->wifi_up ? "WIFI" : "----", 1);
    char vol[8] = "VOL ";
    unsigned v = m->volume > 100 ? 100 : m->volume;
    int n = 4;
    if (v >= 100) vol[n++] = '1';
    if (v >= 10) vol[n++] = (char)('0' + (v / 10) % 10);
    vol[n++] = (char)('0' + v % 10);
    vol[n] = 0;
    ui_draw_text(fb, 160, y, vol, 1);
    ui_fill_rect(fb, 0, 28, UI_W, 2, true);
}

static const char *screen_label(const ui_model_t *m)
{
    switch (m->screen) {
        case UI_IDLE:     return m->registered ? "READY" : "NO SERVICE";
        case UI_DIALLING: return "DIAL";
        case UI_CALLING:  return "CALLING";
        case UI_INCOMING: return "INCOMING";
        case UI_INCALL:   return "IN CALL";
        case UI_ENDED:    return m->last_call_failed ? "CALL FAILED" : "CALL ENDED";
        case UI_NOTICE:   return m->notice_label;
    }
    return "";
}

// B_NUMBER: F_NUM when every character has a glyph, F_UI 2x otherwise (an
// alphanumeric caller ID). Centred in the 72-row band.
static void draw_number(uint8_t *fb, const char *num)
{
    if (!num || !*num) return;
    if (all_num_glyphs(num)) {
        const int scale = 4, spacing = 4, pitch = 5 * scale + spacing;
        int w = (int)strlen(num) * pitch - spacing;
        int x = (UI_W - w) / 2;
        if (x < 0) x = 0;
        ui_draw_num(fb, x, 88 + (72 - 7 * scale) / 2, num, scale, spacing);
    } else {
        ui_draw_text_centred(fb, 88 + (72 - 32) / 2, num, 2);
    }
}

// B_BODY: filled block = a call is ringing or up, hollow ring = otherwise.
static void draw_body(uint8_t *fb, const ui_model_t *m)
{
    if (m->screen == UI_NOTICE) return;
    const int x = UI_W / 2 - 30, y = 206, w = 60, h = 60;
    if (m->screen == UI_INCOMING || m->screen == UI_INCALL) {
        ui_fill_rect(fb, x, y, w, h, true);
    } else {
        ui_fill_rect(fb, x, y, w, 4, true);
        ui_fill_rect(fb, x, y + h - 4, w, 4, true);
        ui_fill_rect(fb, x, y, 4, h, true);
        ui_fill_rect(fb, x + w - 4, y, 4, h, true);
    }
}

void ui_compose(uint8_t *fb, const ui_model_t *m)
{
    ui_clear(fb);
    draw_status(fb, m);
    ui_draw_text_centred(fb, 44, screen_label(m), 2);
    draw_number(fb, m->number);
    if (m->screen == UI_NOTICE) ui_draw_text_centred(fb, 176, m->notice_sub, 1);
    draw_body(fb, m);
    ui_fill_rect(fb, 0, 272, UI_W, 2, true);
    if (m->screen == UI_NOTICE) {
        ui_draw_text(fb, 0, 280, m->notice_hint[0], 1);
        ui_draw_text(fb, 0, 298, m->notice_hint[1], 1);
    }
}
