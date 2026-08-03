#ifndef EPAPER_DISPLAY_H
#define EPAPER_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize GDEQ031T10 E-Paper display & backlight on GPIO41
esp_err_t epaper_display_init(void);

// Set front-light brightness (true = ON, false = OFF)
void epaper_set_backlight(bool enable);

// Render basic call status screen (Caller ID, status text, PTT state)
void epaper_render_call_status(const char *caller_id, const char *status, bool ptt_active);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_DISPLAY_H
