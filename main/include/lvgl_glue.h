// New for this project: bridges LVGL 8.3 to the ported epaper_display.c
// (full-refresh-only, 1bpp) and tca8418_keypad.c (single-press-event) APIs.
#ifndef LVGL_GLUE_H
#define LVGL_GLUE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the LVGL tick source, display driver (backed by epaper_flush(),
// off-task via a depth-1 overwrite queue so a 3.3s hardware refresh never
// blocks the caller), and a keypad input device (backed by
// tca8418_get_key()) bound to a new input group. Call once, after
// epaper_display_init() and tca8418_init() have both succeeded.
//
// Returns the input group every navigable screen/widget should be added to
// via lv_group_add_obj() -- there is only one group in this project (no
// touch, no multi-focus concept), so screens don't create their own.
//
// Deliberately does NOT spawn its own "pump" task: LVGL is not thread-safe,
// and this project would otherwise need a mutex around every lv_* call to
// let a second task touch LVGL safely. Simpler and just as correct: exactly
// ONE task ever calls into LVGL. Call lvgl_glue_pump() from that one task's
// own loop (see main.c and wifi_wizard.c's local wait-for-event loops).
lv_group_t *lvgl_glue_init(void);

// The keypad input device, for scr_mgr to swap groups onto per screen.
lv_indev_t *lvgl_glue_keypad_indev(void);

// Run one LVGL time/render/input tick, then push the newest rendered frame
// to the panel if rendering has been quiet for a short window (see
// FLUSH_QUIET_MS in lvgl_glue.c -- a burst of keystrokes becomes one ~3.3s
// refresh, not one per key). Call in a loop (~20ms cadence) from whichever
// single task owns LVGL -- never from more than one task.
void lvgl_glue_pump(void);

// Make the next refresh a FULL one (~3.3s, inverting, clears ghosting)
// instead of the default PARTIAL. Call wherever lv_disp_load_scr() is: a
// whole new screen is exactly when ghosting from the old one should be
// wiped. Ordinary in-screen updates (typing, focus moves) stay partial.
void lvgl_glue_request_full_refresh(void);

// Render now and push to the panel immediately, skipping the quiet window.
// For the moment right before a long blocking call (WiFi scan/connect) when
// the pump loop won't run for a while: whatever is on screen goes out first.
void lvgl_glue_flush_now(void);

#ifdef __cplusplus
}
#endif

#endif // LVGL_GLUE_H
