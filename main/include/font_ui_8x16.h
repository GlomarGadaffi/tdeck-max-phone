#ifndef FONT_UI_8X16_H
#define FONT_UI_8X16_H

// F_UI (UI_DESIGN 7, #37): 8x16 bitmap font, printable ASCII 0x20-0x7E.
// One byte per row, MSB = leftmost pixel, 1 = ink. Drawn at 1x (30 cols x
// 20 rows on the 240x320 panel) and 2x (15 x 10). 95 x 16 = 1520 bytes.
//
// Glyphs are Spleen 8x16 (BSD 2-Clause, Frederic Cambus); the licence text
// is in font_ui_8x16.c, which tools/gen_font_ui.py generates.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_UI_W       8
#define FONT_UI_H       16
#define FONT_UI_FIRST   0x20
#define FONT_UI_LAST    0x7E
#define FONT_UI_GLYPHS  (FONT_UI_LAST - FONT_UI_FIRST + 1)   // 95

extern const uint8_t font_ui_8x16[FONT_UI_GLYPHS][FONT_UI_H];

#ifdef __cplusplus
}
#endif

#endif // FONT_UI_8X16_H
