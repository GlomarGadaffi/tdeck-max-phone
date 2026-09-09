// The phone: SIP registration, the call state machine, the audio pump, and
// the dialer / call / incoming screens. C-callable so the C views (Contacts,
// Settings) can dial and power off; the implementation is C++ because it
// owns the TincanUac.
//
// Threading: everything here except the audio task runs on the main task,
// which is also the only LVGL owner -- so screen updates and SIP control
// share one loop and never need a lock (TincanUac's contract: only the media
// calls are safe off-task, and only the audio task uses those).
#ifndef PHONE_APP_H
#define PHONE_APP_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the phone's screens with scr_mgr. Before app_nav_show_home().
void phone_app_register_screens(void);

// Bind sockets, REGISTER (blocking, bounded ~6 s), start the audio task.
// Needs Wi-Fi up. Safe to call again after a failure. Returns ESP_OK once
// the UAC is initialised (registration may still have failed -- see
// phone_app_registered()).
esp_err_t phone_app_start(void);
bool phone_app_started(void);
bool phone_app_registered(void);

// Every main-loop tick: SIP poll, registration refresh, state machine,
// on-screen updates. Pushes the Incoming screen itself when a call arrives.
void phone_app_tick(void);

// Place a call (from the dialer, a recent row, or a contact). Pushes the
// Call screen on success. Ignored while a call is already in progress.
void phone_app_dial(const char *number);

// Real power off (SY6970 BATFET), or deep sleep on USB. Does not return.
// Hangs up first if a call is active.
void phone_app_power_off(void);

#ifdef __cplusplus
}
#endif

#endif // PHONE_APP_H
