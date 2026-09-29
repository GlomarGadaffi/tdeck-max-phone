// tdeck_kbl -- one-shot modifier + layer resolver for the T-Deck-MAX 4x10 keyboard.
//
// Why this exists: every modifier scheme built on this matrix so far (LilyGO's
// factory UI, and the drivers ported from it) either LATCHES a layer (ALT case
// toggle) or needs a CHORD held on a bottom-row key while the other thumb hits
// a letter (hold UP for the digit layer), and both depend on seeing the
// modifier's RELEASE edge. A dropped release edge, or a burst of events read one
// per poll tick, leaves the layer stuck on. Meshtastic's T-Deck-MAX keyboard
// driver (src/input/TDeckProKeyboard.cpp) is the only implementation here that
// has behaved, and its mechanism is:
//
//   * a modifier key TOGGLES a bit on its PRESS edge (release is not needed);
//   * the next ordinary key is resolved through that layer and then CLEARS
//     every armed modifier (one-shot);
//   * an armed modifier nobody used is dropped after 1.5 s.
//
// This module is that mechanism, with Meshtastic's key roles: the two bottom-row
// keys the vendor code calls "UP" (r3c5, r3c9) are SHIFT, r3c8 is SYM (the
// printed digit/punctuation legends) and r2c0 is ALT (arrows, ESC, TAB). It
// consumes PRESS edges only, so there is no state a lost release could wedge.
//
// Differences from Meshtastic, all deliberate: characters are resolved on the
// press edge (Meshtastic emits on release and drops a character when two keys
// overlap); layers have a fixed precedence ALT > SYM > SHIFT instead of
// `flags % 5`; '0' is on the base layer; and no Meshtastic-app actions (ping,
// GPS toggle, mute, backlight) are bound.
//
// Pure C, no ESP-IDF dependencies: host-testable. The canonical copy and its
// host test live in tdeck-max-idf (components/tdeck_keypad/, test_host/test_kbl.c);
// tdeck-max-phone and tdeck-glopanel carry verbatim copies of tdeck_kbl.{h,c}
// (their keypad drivers were themselves copies of tdeck-max-idf's) -- change
// all three together.
#ifndef TDECK_KBL_H
#define TDECK_KBL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Matrix geometry: the (row, col) tca8418_decode_event() returns.
#define TDECK_KBL_ROWS 4
#define TDECK_KBL_COLS 10

// Armed-modifier bits, as reported by tdeck_kbl_mods().
#define TDECK_KBL_SHIFT 0x01 // r3c5 and r3c9, sharing one bit (pressing both cancels)
#define TDECK_KBL_SYM   0x02 // r3c8
#define TDECK_KBL_ALT   0x04 // r2c0

// An armed modifier that no key consumed is dropped after this long.
#define TDECK_KBL_MOD_TIMEOUT_MS 1500

typedef enum {
    TDECK_KBL_NONE = 0, // nothing to deliver: a modifier was toggled, or the key has no meaning on this layer
    TDECK_KBL_CHAR,     // .ch holds a printable ASCII character
    TDECK_KBL_DEL,
    TDECK_KBL_ENT,
    TDECK_KBL_ESC,      // ALT + Q
    TDECK_KBL_TAB,      // ALT + T
    TDECK_KBL_UP,       // ALT + E
    TDECK_KBL_DOWN,     // ALT + X
    TDECK_KBL_LEFT,     // ALT + S
    TDECK_KBL_RIGHT,    // ALT + F
} tdeck_kbl_kind_t;

typedef struct {
    tdeck_kbl_kind_t kind;
    char ch;
} tdeck_kbl_event_t;

typedef struct {
    uint8_t mods;      // armed TDECK_KBL_* bits
    uint32_t mods_ms;  // time of the last modifier press
} tdeck_kbl_t;

void tdeck_kbl_init(tdeck_kbl_t *k);

// Feed one key PRESS (never a release). Modifier keys return TDECK_KBL_NONE
// after toggling their bit; any other key returns its resolved event and clears
// every armed modifier. Positions that are not physical keys (r3c0..r3c4,
// out of range) are ignored and leave the state untouched.
tdeck_kbl_event_t tdeck_kbl_press(tdeck_kbl_t *k, int row, int col, uint32_t now_ms);

// Modifiers still armed at now_ms (an expired one is dropped first). For hints.
uint8_t tdeck_kbl_mods(tdeck_kbl_t *k, uint32_t now_ms);

// Drop every armed modifier (screen change, or a key the caller handled itself).
void tdeck_kbl_clear(tdeck_kbl_t *k);

// True if (row, col) is one of the modifier keys.
bool tdeck_kbl_is_modifier(int row, int col);

#ifdef __cplusplus
}
#endif

#endif // TDECK_KBL_H
