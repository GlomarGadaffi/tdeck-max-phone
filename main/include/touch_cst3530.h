// Hynitron CST3530 capacitive touch (the T-Deck-MAX's touch layer, I2C
// 0x1A, INT GPIO12, reset via XL9555 P0_7). Read path only, ported from the
// Hynitron reference driver's cst3xx family (hyn_cst3xx.c, as bundled in the
// working Meshtastic T-Deck-MAX port): 16-bit big-endian register addresses,
// a 7-byte report at 0xD000, a 3-byte "frame consumed" tail write.
#ifndef TOUCH_CST3530_H
#define TOUCH_CST3530_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reset the chip (via the XL9555), read its firmware info block, and put it
// in normal reporting mode. ESP_ERR_NOT_FOUND if no CST3xx answers; the
// caller should treat that as "no touch on this unit", not a fatal error.
// Requires i2c + xl9555 to be initialized first.
esp_err_t touch_cst3530_init(void);

// True once touch_cst3530_init() succeeded.
bool touch_cst3530_present(void);

// Poll one report. Returns true while a finger is down and fills x/y in
// DISPLAY coordinates (0..239, 0..319 after the orientation #defines in
// touch_cst3530.c); false when nothing is touching or on I2C error.
bool touch_cst3530_read(int16_t *x, int16_t *y);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_CST3530_H
