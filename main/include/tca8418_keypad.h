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
// isn't polled every tick, including mid-call alongside the ES8311.
bool tca8418_key_pending(void);

// Poll for keypress event (returns keycode ASCII or 0 if queue empty)
char tca8418_get_key(void);

// Discard every queued key event, pressed or released, and return how many
// there were. For input that must not be replayed: keys pressed while
// placeCall() blocked the loop (UI_DESIGN 2.3), or during the input grace
// window before a new screen is visible (#40).
int tca8418_flush(void);

// Turn keyboard backlight LED on/off
void tca8418_set_backlight(bool enable);

#ifdef __cplusplus
}
#endif

#endif // TCA8418_KEYPAD_H
