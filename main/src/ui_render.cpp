// Pure 1-bit drawing for the e-paper UI -- see ui_render.h. No ESP-IDF
// headers on purpose: test/ compiles this file on the host.
#include "ui_render.h"
#include "font_ui_8x16.h"
#include <stdio.h>
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

// ── B_STATUS: fixed pixel columns (UI_DESIGN 4.6), F_UI 1x, 2 px rule ────
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

// ── B_LABEL ──────────────────────────────────────────────────────────────
static const char *screen_label(const ui_model_t *m)
{
    switch (m->screen) {
        // NO SERVICE is not in the 4.1 mockup, which only shows READY. An
        // unregistered phone cannot take calls, and READY over a NOREG
        // status bar would be the one place the screen contradicts itself.
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

// ── B_NUMBER (UI_DESIGN 4.2) ─────────────────────────────────────────────
// F_NUM, scaled by length. The 4.2 table gives pitches of 30/24/18 px; those
// leave no room for the 4 px caret on a full line (8 x 30 = 240), so the
// spacing is 1 px tighter here and every row of the table still fits with
// the caret: 8 x 28, 10 x 23, 13 x 17.
#define NUM_SHOWN_MAX 13

static void num_layout(size_t len, int *scale, int *spacing)
{
    if (len <= 8)       { *scale = 5; *spacing = 3; }
    else if (len <= 10) { *scale = 4; *spacing = 3; }
    else                { *scale = 3; *spacing = 2; }
}

static void draw_number(uint8_t *fb, const ui_model_t *m)
{
    const char *num = m->number;
    if (!*num) return;
    const int band_y = 88, band_h = 72;

    if (!all_num_glyphs(num)) {
        // An alphanumeric caller ID (a SIP display name): F_UI 2x, centred.
        ui_draw_text_centred(fb, band_y + (band_h - 32) / 2, num, 2);
        return;
    }

    size_t len = strlen(num);
    const char *shown = num;
    if (len > NUM_SHOWN_MAX) {           // rightmost 13; B_SUB says so
        shown = num + (len - NUM_SHOWN_MAX);
        len = NUM_SHOWN_MAX;
    }
    int scale, spacing;
    num_layout(len, &scale, &spacing);
    const int pitch = 5 * scale + spacing;
    const int w = (int)len * pitch - spacing;
    const int h = 7 * scale;
    const int y = band_y + (band_h - h) / 2;

    if (m->screen == UI_DIALLING) {
        // Right-aligned like a calculator, so the caret stays put and the
        // digit just typed appears in the same place every time.
        const int caret_x = UI_W - 4 - 4;          // 4 px wide, 4 px margin
        const int x = caret_x - 4 - w;             // 4 px gap before the caret
        ui_draw_num(fb, x, y, shown, scale, spacing);
        ui_fill_rect(fb, caret_x, y, 4, h, true);
    } else {
        ui_draw_num(fb, (UI_W - w) / 2, y, shown, scale, spacing);
    }
}

// ── B_SUB ────────────────────────────────────────────────────────────────
static void format_duration(char *out, size_t n, uint32_t secs)
{
    uint32_t mm = secs / 60, ss = secs % 60;
    if (mm > 99) mm = 99;
    snprintf(out, n, "%02u:%02u", (unsigned)mm, (unsigned)ss);
}

static void draw_sub(uint8_t *fb, const ui_model_t *m)
{
    char line[UI_TEXT_COLS + 1] = "";
    char dur[8];
    switch (m->screen) {
        case UI_IDLE:
            if (m->last_call_secs) {
                format_duration(dur, sizeof(dur), m->last_call_secs);
                snprintf(line, sizeof(line), "LAST CALL   %s", dur);
            }
            break;
        case UI_ENDED:
            if (m->last_call_failed) {
                snprintf(line, sizeof(line), "COULD NOT CONNECT");
            } else {
                format_duration(dur, sizeof(dur), m->last_call_secs);
                snprintf(line, sizeof(line), "%s", dur);
            }
            break;
        case UI_DIALLING: {
            // The only thing B_SUB ever says while dialling: that B_NUMBER
            // is no longer showing the whole buffer (UI_DESIGN 4.2).
            size_t len = strlen(m->number);   // < sizeof(m->number) = 24
            if (len > NUM_SHOWN_MAX)
                snprintf(line, sizeof(line), "%u DIGITS - SHOWING LAST %d",
                         (unsigned)(len % 100), NUM_SHOWN_MAX);
            break;
        }
        case UI_INCALL:
            if (m->muted) snprintf(line, sizeof(line), "MUTED");
            break;
        case UI_NOTICE:
            snprintf(line, sizeof(line), "%s", m->notice_sub);
            break;
        default:
            break;
    }
    if (line[0]) ui_draw_text_centred(fb, 176, line, 1);
}

// ── B_BODY ───────────────────────────────────────────────────────────────
// Rings and blocks drawn with fill_rect only (UI_DESIGN 7): solid black at
// maximum contrast, strokes 8 px, no glyph data.
static void ring(uint8_t *fb, int x, int y, int w, int h, int t)
{
    ui_fill_rect(fb, x, y, w, t, true);
    ui_fill_rect(fb, x, y + h - t, w, t, true);
    ui_fill_rect(fb, x, y, t, h, true);
    ui_fill_rect(fb, x + w - t, y, t, h, true);
}

static void draw_body(uint8_t *fb, const ui_model_t *m)
{
    switch (m->screen) {
        case UI_IDLE:
        case UI_ENDED:
        case UI_DIALLING: {
            // The keypad cheat-sheet: free to keep on e-paper, and the only
            // thing teaching the digit layer if the alt legends are not
            // printed on the keycaps (UI_DESIGN U3, still open).
            static const char *const sheet[4] = {
                "1=W   2=E   3=R",
                "4=S   5=D   6=F",
                "7=Z   8=X   9=C",
                "*=A   0=0   #=Q",
            };
            for (int i = 0; i < 4; i++) ui_draw_text(fb, 24, 204 + 16 * i, sheet[i], 1);
            break;
        }
        case UI_CALLING:   // hollow ring: waiting
            ring(fb, (UI_W - 96) / 2, 208, 96, 56, 8);
            break;
        case UI_INCOMING:  // solid block: maximum black, readable across a desk
            ui_fill_rect(fb, (UI_W - 128) / 2, 208, 128, 56, true);
            break;
        case UI_INCALL:    // the same ring with a filled centre bar: connected
            ring(fb, (UI_W - 96) / 2, 208, 96, 56, 8);
            ui_fill_rect(fb, (UI_W - 96) / 2 + 16, 208 + 24, 96 - 32, 8, true);
            break;
        case UI_NOTICE:
            break;
    }
}

// ── B_HINT: what ENT and DEL do right now (UI_DESIGN 1.5) ────────────────
// Only bindings that exist are advertised. Mute, volume and hold-DEL are in
// the design but not built (#45, #46), so no hint mentions them, and IDLE's
// DEL says what it really does today -- the power-off prompt.
static void draw_hint(uint8_t *fb, const ui_model_t *m)
{
    const char *l1 = "", *l2 = "";
    switch (m->screen) {
        case UI_IDLE:
        case UI_ENDED:
            l1 = m->number[0] ? "ENT redial    DEL power off" : "DEL power off";
            l2 = "type a number to dial";
            break;
        case UI_DIALLING:
            l1 = "ENT call      DEL erase";
            l2 = "9 + number = outside line";
            break;
        case UI_CALLING:
            l1 = "connecting - please wait";
            l2 = "keys are ignored until answer";
            break;
        case UI_INCOMING:
            l1 = "ENT answer    DEL reject";
            break;
        case UI_INCALL:
            l1 = "DEL end call";
            break;
        case UI_NOTICE:
            l1 = m->notice_hint[0];
            l2 = m->notice_hint[1];
            break;
    }
    ui_fill_rect(fb, 0, 272, UI_W, 2, true);
    ui_draw_text(fb, 4, 280, l1, 1);
    ui_draw_text(fb, 4, 298, l2, 1);
}

void ui_compose(uint8_t *fb, const ui_model_t *m)
{
    ui_clear(fb);
    draw_status(fb, m);
    ui_draw_text_centred(fb, 44, screen_label(m), 2);
    draw_number(fb, m);
    draw_sub(fb, m);
    draw_body(fb, m);
    draw_hint(fb, m);
}
