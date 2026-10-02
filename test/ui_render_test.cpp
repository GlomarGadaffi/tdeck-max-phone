// Host-side tests for the e-paper renderer (main/src/ui_render.cpp).
//
// The panel cannot be seen from a log, and a full refresh is a second of
// flashing per look, so the drawing code is checked here instead: glyph
// tables, text placement, clipping. Set UI_DUMP_DIR to also write each test
// frame as a PBM image for eyeballing on the host.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "font_ui_8x16.h"
#include "ui_render.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        std::cout << "  FAIL: " << what << "\n";
    } else {
        std::cout << "  ok:   " << what << "\n";
    }
}

static bool black(const uint8_t *fb, int x, int y)
{
    return (fb[y * UI_STRIDE + x / 8] & (0x80 >> (x % 8))) == 0;
}

static int ink_in_rows(const uint8_t *fb, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < UI_W; x++)
            n += black(fb, x, y);
    return n;
}

// P4 (binary PBM): 1 = black, which is the inverse of the panel's bit sense.
static void dump(const uint8_t *fb, const char *name)
{
    const char *dir = std::getenv("UI_DUMP_DIR");
    if (!dir) return;
    std::string path = std::string(dir) + "/" + name + ".pbm";
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P4\n%d %d\n", UI_W, UI_H);
    for (int i = 0; i < UI_FB_SIZE; i++) std::fputc((uint8_t)~fb[i], f);
    std::fclose(f);
}

static void test_font_table()
{
    std::cout << "F_UI table\n";
    check(FONT_UI_GLYPHS == 95, "95 glyphs, printable ASCII 0x20-0x7E");
    check(sizeof(font_ui_8x16) == 1520, "1520 bytes, as UI_DESIGN 7 budgets");

    bool space_blank = true;
    for (int r = 0; r < FONT_UI_H; r++) space_blank &= font_ui_8x16[0][r] == 0;
    check(space_blank, "space is blank");

    int empty = 0;
    for (int g = 1; g < FONT_UI_GLYPHS; g++) {
        int ink = 0;
        for (int r = 0; r < FONT_UI_H; r++) ink |= font_ui_8x16[g][r];
        if (!ink) empty++;
    }
    check(empty == 0, "every non-space glyph has ink");

    // Spot check against the Spleen 8x16 source so a regenerated table that
    // shifted by one glyph cannot pass.
    const uint8_t *A = font_ui_8x16['A' - FONT_UI_FIRST];
    check(A[2] == 0x7C && A[3] == 0xC6 && A[6] == 0xFE, "'A' matches Spleen rows 2/3/6");
    const uint8_t *g = font_ui_8x16['g' - FONT_UI_FIRST];
    check(g[5] == 0x7E && g[14] == 0xFC, "'g' descender matches Spleen");
}

static void test_text()
{
    std::cout << "ui_draw_text\n";
    std::vector<uint8_t> fb(UI_FB_SIZE);

    ui_clear(fb.data());
    ui_draw_text(fb.data(), 0, 0, "A", 1);
    // Row 2 of 'A' is 0x7C: pixels 1..5 ink, 0 and 6..7 paper.
    check(!black(fb.data(), 0, 2) && black(fb.data(), 1, 2) && black(fb.data(), 5, 2) &&
              !black(fb.data(), 6, 2),
          "1x 'A' row 2 lands on x=1..5");
    check(ink_in_rows(fb.data(), 16, UI_H - 1) == 0, "1x glyph stays inside its 16 rows");

    ui_clear(fb.data());
    ui_draw_text(fb.data(), 0, 0, "A", 2);
    check(black(fb.data(), 2, 4) && black(fb.data(), 3, 5) && !black(fb.data(), 1, 4),
          "2x 'A' doubles each pixel");
    check(ink_in_rows(fb.data(), 32, UI_H - 1) == 0, "2x glyph stays inside its 32 rows");

    check(ui_text_width("READY", 1) == 40 && ui_text_width("READY", 2) == 80,
          "text width is 8 px per char per scale");

    // An out-of-range byte must be visible, not silently dropped.
    std::vector<uint8_t> a(UI_FB_SIZE), b(UI_FB_SIZE);
    ui_clear(a.data());
    ui_clear(b.data());
    ui_draw_text(a.data(), 0, 0, "\x01", 1);
    ui_draw_text(b.data(), 0, 0, "?", 1);
    check(a == b, "non-printable byte draws as '?'");

    // 40 characters on a 30-column panel: whole glyphs only, no wrap into
    // the next row, nothing written past the right edge.
    ui_clear(fb.data());
    ui_draw_text(fb.data(), 0, 100, "0123456789012345678901234567890123456789", 1);
    check(ink_in_rows(fb.data(), 0, 99) == 0 && ink_in_rows(fb.data(), 116, UI_H - 1) == 0,
          "over-long line is clipped, never wrapped");

    ui_clear(fb.data());
    ui_draw_text_centred(fb.data(), 0, "AB", 1);
    int minx = UI_W, maxx = -1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < UI_W; x++)
            if (black(fb.data(), x, y)) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
    check(minx >= 112 && maxx < 128, "centred text sits in x=112..127 for two glyphs");

    // A sample sheet for the eye: every glyph at 1x, a label at 2x.
    ui_clear(fb.data());
    char line[31];
    for (int row = 0; row < 4; row++) {
        int n = 0;
        for (int c = FONT_UI_FIRST + row * 30; c <= FONT_UI_LAST && n < 30; c++) line[n++] = (char)c;
        line[n] = 0;
        ui_draw_text(fb.data(), 0, row * 16, line, 1);
    }
    ui_draw_text_centred(fb.data(), 96, "INCOMING", 2);
    ui_draw_num(fb.data(), 30, 150, "*777", 5, 5);
    dump(fb.data(), "font_sheet");
}

static void test_num()
{
    std::cout << "F_NUM\n";
    const char *dialable = "0123456789*#+";
    bool all = true;
    for (const char *p = dialable; *p; p++) all &= ui_num_has_glyph(*p);
    check(all, "every dialable character has an F_NUM glyph");
    check(!ui_num_has_glyph('A') && !ui_num_has_glyph(' '), "letters and space do not");
}

static ui_model_t model(ui_screen_t screen, const char *number)
{
    ui_model_t m{};
    m.screen = screen;
    std::snprintf(m.number, sizeof(m.number), "%s", number);
    std::snprintf(m.self_ext, sizeof(m.self_ext), "1002");
    m.registered = true;
    m.wifi_up = true;
    m.volume = 100;
    return m;
}

// Every screen, drawn whole, must leave the margins between bands blank:
// that is what makes each band independently refreshable (UI_DESIGN 4.0).
static void test_band_grid()
{
    std::cout << "band grid\n";
    int y0, y1;
    ui_band_rows(B_STATUS, &y0, &y1);
    check(y0 == 0 && y1 == 29, "B_STATUS is rows 0-29");
    ui_band_rows(B_NUMBER, &y0, &y1);
    check(y0 == 88 && y1 == 159, "B_NUMBER is rows 88-159");
    ui_band_rows(B_HINT, &y0, &y1);
    check(y0 == 272 && y1 == 319, "B_HINT is rows 272-319");
    ui_band_rows(B_ALL, &y0, &y1);
    check(y0 == 0 && y1 == UI_H - 1, "B_ALL is the whole panel");

    struct { ui_screen_t s; const char *num; const char *name; } screens[] = {
        {UI_IDLE, "*777", "idle"},
        {UI_DIALLING, "9*777", "dialling"},
        {UI_CALLING, "777", "calling"},
        {UI_INCOMING, "1001", "incoming"},
        {UI_INCALL, "1001", "incall"},
        {UI_ENDED, "777", "ended"},
        {UI_INCOMING, "Alice", "incoming_alpha"},
    };
    std::vector<uint8_t> fb(UI_FB_SIZE);
    for (auto &sc : screens) {
        ui_model_t m = model(sc.s, sc.num);
        ui_compose(fb.data(), &m);
        bool margins_blank = ink_in_rows(fb.data(), 30, 39) == 0 &&
                             ink_in_rows(fb.data(), 80, 87) == 0 &&
                             ink_in_rows(fb.data(), 160, 167) == 0;
        check(margins_blank, std::string(sc.name) + ": margins between bands stay blank");
        check(ink_in_rows(fb.data(), 88, 159) > 0, std::string(sc.name) + ": number band has ink");
        dump(fb.data(), sc.name);
    }

    ui_model_t m = model(UI_NOTICE, "");
    std::snprintf(m.notice_label, sizeof(m.notice_label), "POWER OFF?");
    std::snprintf(m.notice_hint[0], sizeof(m.notice_hint[0]), "ENT  power off");
    std::snprintf(m.notice_hint[1], sizeof(m.notice_hint[1]), "any other key cancels");
    ui_compose(fb.data(), &m);
    check(ink_in_rows(fb.data(), 40, 79) > 0 && ink_in_rows(fb.data(), 280, 319) > 0,
          "notice: label and hint lines are drawn");
    dump(fb.data(), "notice_poweroff");

    // The status bar is model data, not decoration: NOREG must look different.
    ui_model_t a = model(UI_IDLE, ""), b = a;
    b.registered = false;
    std::vector<uint8_t> fa(UI_FB_SIZE), fbb(UI_FB_SIZE);
    ui_compose(fa.data(), &a);
    ui_compose(fbb.data(), &b);
    check(std::memcmp(fa.data(), fbb.data(), 30 * UI_STRIDE) != 0, "REG vs NOREG changes B_STATUS");
}

int main()
{
    test_font_table();
    test_text();
    test_num();
    test_band_grid();
    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures ? 1 : 0;
}
