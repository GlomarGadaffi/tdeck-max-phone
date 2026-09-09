// SPI/command layer ported from tdeck-max-phone (main/include+src/epaper_display.*);
// the dialpad font and phone-specific render function were NOT ported (this
// project renders through LVGL instead -- see lvgl_glue.c). The raw
// full-refresh path (epaper_flush) is exported here as the new LVGL flush
// target.
#ifndef EPAPER_DISPLAY_H
#define EPAPER_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WIDTH         240
#define EPD_HEIGHT        320
#define EPD_BYTES_PER_ROW (EPD_WIDTH / 8)                   // 30
#define EPD_BUF_SIZE      (EPD_BYTES_PER_ROW * EPD_HEIGHT)  // 9600

// Initialize GDEQ031T10 E-Paper display & backlight on GPIO41
esp_err_t epaper_display_init(void);

// Front-light (LEDC PWM on GPIO41). set_backlight toggles; set_frontlight_pct
// sets the ON brightness (0-100) and applies it.
void epaper_set_backlight(bool enable);
void epaper_set_frontlight_pct(uint8_t pct);

// Power the panel off + deep sleep (bistable: the image stays, zero static
// drain). Refreshes leave the panel powered so back-to-back updates skip
// re-init; the flush task calls this after an idle timeout. Must be called
// from the same task that calls epaper_flush*() (the SPI device handle is
// not shared across tasks).
void epaper_sleep(void);

// Refresh waveforms, from the vendor reference (Display_EPD_W21.cpp):
//   FULL    -- EPD_Init: default OTP LUT. ~3.3s measured, several black/white
//              inversion passes (reads as the whole panel "strobing"). Clears
//              ghosting; use on screen changes and periodically.
//   FAST    -- EPD_Init_Fast: 0xE0=0x02, 0xE5=0x5A. Vendor says ~1.0s.
//   PARTIAL -- EPD_Init_Part: 0xE0=0x02, 0xE5=0x79, 0x50=0xD7, then the same
//              whole-frame OLD/NEW transfer as FULL (vendor's EPD_Dis_PartAll:
//              full-screen partial refresh, no window command, so none of the
//              window-coordinate uncertainty tdeck-max-phone documented). No
//              inversion flash; accumulates ghosting, hence periodic FULL.
// All three send the panel's previous frame as OLD data, so the panel never
// depends on RAM retained across deep sleep.
typedef enum {
    EPD_REFRESH_FULL = 0,
    EPD_REFRESH_FAST = 1,
    EPD_REFRESH_PARTIAL = 2,
} epd_refresh_mode_t;

// Push a full 1bpp frame (EPD_BUF_SIZE bytes, MSB-first per row, 1=white
// 0=black) to the panel. Blocks for the duration of the refresh; callers
// that can't afford to block should post through a task/queue (see
// lvgl_glue.c). epaper_flush() is EPD_REFRESH_FULL.
void epaper_flush(const uint8_t *fb);
void epaper_flush_mode(const uint8_t *fb, epd_refresh_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_DISPLAY_H
