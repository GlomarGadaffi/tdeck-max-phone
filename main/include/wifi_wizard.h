// New for this project: a keypad-driven equivalent of glopanel-weather's
// touch-driven first-boot wizard (WiFi AP list -> password entry), since
// the TCA8418 keypad has no touch/on-screen-keyboard concept. Persists
// SSID/password to NVS on success so only first boot (or an explicit
// "forget WiFi") re-enters this flow -- see project plan Phase 0.
#ifndef WIFI_WIZARD_H
#define WIFI_WIZARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if a previous wizard run persisted WiFi credentials to NVS.
bool wifi_wizard_has_saved_credentials(void);

// Load saved SSID/password from NVS and attempt to connect. Blocks up to
// ~30s (wifi_sta_connect()'s own timeout) -- call from a background task,
// not directly from app_main, if boot must not stall on a down AP (see
// main.c). Returns the same esp_err_t wifi_sta_connect() would.
esp_err_t wifi_wizard_connect_saved(void);

// Run the interactive scan -> select -> password -> connect flow. Takes
// over the display (creates and loads its own LVGL screens against the
// given group) until the user succeeds; on success also persists the
// credentials to NVS. Does not return until connected -- there is nothing
// else for the device to usefully do before it has a network, matching
// glopanel-weather's wizard-gates-first-boot precedent.
void wifi_wizard_run(lv_group_t *group);

#ifdef __cplusplus
}
#endif

#endif // WIFI_WIZARD_H
