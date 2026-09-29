// Ported from tdeck-max-phone (main/include/tca8418_keypad.h). API surface
// is unchanged; see tca8418_keypad.cpp for the extended QWERTY keymap this
// project adds on top of the ported driver core.
#ifndef TCA8418_KEYPAD_H
#define TCA8418_KEYPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize TCA8418 keypad matrix controller over I2C
esp_err_t tca8418_init(void);

// True if the controller has queued key events (single GPIO read of the
// active-low INT pin). Gate tca8418_get_key() on this so the shared I2C bus
// isn't polled every tick.
bool tca8418_key_pending(void);

// Poll for keypress event and return its decoded character, or 0 if the
// queue is empty or the key is unmapped/a release edge.
//
// Modifier scheme (matches LilyGO's factory firmware WiFi-password screen,
// see tca8418_keypad.cpp): ALT tap toggles abc/ABC; holding either "UP" key
// (r3c5/r3c9) while pressing another key gives the digit/symbol layer;
// tapping an UP key alone is list navigation, reported as one of these two
// sentinel codes on the key's RELEASE. Which UP key is physically left vs
// right is UNVERIFIED on hardware (tdeck-max-phone's open item U4) -- if
// NAV_A/NAV_B come out backwards, swap the two #define values, nothing else
// needs to change.
#define TCA8418_KEY_NAV_A ((char)0x01) // r3c5 "UP" key -- mapped to list-up
#define TCA8418_KEY_NAV_B ((char)0x02) // r3c9 "UP" key -- mapped to list-down

// SYM (r3c8) is inert/reserved on tdeck-max-phone. This project needs a
// "back/cancel" key more than the factory firmware's show/hide-password use
// of it, so SYM is ESC here.
#define TCA8418_KEY_ESC ((char)0x03)

char tca8418_get_key(void);

// Which layer the unmodified keys emit. QWERTY (default): lowercase letters,
// ALT/UP modifiers as described above. DIALPAD: the digit/symbol legends
// with no modifier -- 1-9 on W E R / S D F / Z X C, 0 on its own key, * on
// A, # on Q, + on O -- and every letter swallowed. A phone dials digits far
// more often than it types names, and a held modifier per digit is exactly
// the mechanism LilyGO's factory firmware got wrong (docs/UI_DESIGN.md 9.1).
// Screens set this in their entry(): dialer/in-call = DIALPAD, text entry
// = QWERTY. DEL, ENT, sym(ESC) and the UP-tap navigation work in both.
typedef enum {
    TCA8418_LAYOUT_QWERTY = 0,
    TCA8418_LAYOUT_DIALPAD,
} tca8418_layout_t;

void tca8418_set_layout(tca8418_layout_t layout);
tca8418_layout_t tca8418_get_layout(void);

// Text-entry mode (default OFF: nothing above changes until a caller opts in;
// only meaningful in the QWERTY layout, the DIALPAD layout ignores it).
// When on, tca8418_get_key() resolves keys through tdeck_kbl.h -- the one-shot
// scheme Meshtastic's T-Deck-MAX keyboard uses: tap an UP key for Shift, SYM
// for the digit/punctuation legends, ALT for the arrow/ESC layer; each applies
// to the NEXT key only and lapses after 1.5 s, so nothing latches and nothing
// depends on a modifier's release edge. UP taps then no longer navigate:
// ALT+E / ALT+X report NAV_A / NAV_B, ALT+S / ALT+F report KEY_LEFT / KEY_RIGHT
// (cursor moves) and ALT+Q reports ESC. lvgl_glue switches this on for its
// keypad indev, so in the QWERTY layout UP taps no longer navigate lists: use
// ALT+E / ALT+X (or touch). The DIALPAD layout keeps UP-tap navigation.
void tca8418_set_text_entry(bool on);
bool tca8418_text_entry(void);
#define TCA8418_KEY_LEFT  ((char)0x04)
#define TCA8418_KEY_RIGHT ((char)0x05)

// Shift state for hints: the ALT case toggle, or in text-entry mode whether a
// Shift tap is armed for the next key.
bool tca8418_caps_enabled(void);

// Turn keyboard backlight LED on/off
void tca8418_set_backlight(bool enable);


#ifdef __cplusplus
}
#endif

#endif // TCA8418_KEYPAD_H
