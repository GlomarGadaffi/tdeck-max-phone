#ifndef EPAPER_DISPLAY_H
#define EPAPER_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ui_render.h"   // ui_model_t, ui_band_t, ui_screen_t

#ifdef __cplusplus
extern "C" {
#endif

// Initialize GDEQ031T10 E-Paper display & backlight on GPIO41
esp_err_t epaper_display_init(void);

// Set front-light brightness (true = ON, false = OFF)
void epaper_set_backlight(bool enable);

// Draw the UI model to the panel (#38). Composes the whole frame, then
// refreshes. `band` names the one band this update is expected to change;
// B_ALL means a screen transition and always takes a full refresh.
//
// Blocks for the whole refresh (~1.25 s full), so it is called only from the
// e-paper task, never from the control loop.
void epaper_render(const ui_model_t *m, ui_band_t band);

// Monotonic count of renders that have finished on the glass (UI_DESIGN 9.8).
// Bumped after the panel refresh completes, not when a render is requested,
// so a reader can tell "the user can see this now" from "it is queued".
uint32_t epaper_render_generation(void);

// CONFIG_TDECK_MAX_EPD_BENCH only: time full and partial refreshes at boot
// and log min/avg/max (#43). Call before the render task starts -- it drives
// the SPI bus directly.
void epaper_bench_run(void);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_DISPLAY_H
