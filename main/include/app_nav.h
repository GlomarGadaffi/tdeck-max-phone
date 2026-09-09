// Navigation shell shared by every screen: the home grid, the framed screen
// layout (top bar / content / legend), the tap-vs-hold touch policy, and the
// status text. Snapshot of tdeck-glopanel's app_nav (2026-09-09) with the
// home menu rebuilt as an icon grid on top of scr_mgr, and BLE status
// replaced by SIP registration state.
#ifndef APP_NAV_H
#define APP_NAV_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Screen ids for scr_mgr. Every screen a module registers goes here so the
// ids can't collide.
enum {
    SCR_HOME = 0,
    SCR_PHONE,          // dialer
    SCR_CALL,           // calling / ringing / in call / result
    SCR_INCOMING,
    SCR_CONTACTS,
    SCR_CONTACT_EDIT,
    SCR_SETTINGS,
};

// Add a tile to the home grid, in call order. `name` and `symbol` (an
// LV_SYMBOL_* string) must have static storage duration. Opening the tile
// pushes `scr_id` (registered with scr_mgr by the owning module).
void app_nav_register(const char *name, const char *symbol, int scr_id);

// Register the home screen with scr_mgr. Call once after every
// app_nav_register() and before the first app_nav_show_home().
void app_nav_init(void);

// Clear the stack and show the home grid.
void app_nav_show_home(void);

// An action fired by "open" (hold on touch / ENTER on the keypad).
typedef void (*app_nav_action_fn)(void *user_data);

// Bind the project's tap/hold policy to a list row or tile: a touch PRESS
// only focuses it (the highlight moves -- one partial refresh); a touch
// LONG_PRESS fires `action`; a keypad ENTER on the focused object fires
// `action` too. A short touch tap deliberately does NOT fire it -- on a
// panel that takes ~1 s to show anything, an accidental brush must not open
// an app. The object must be in the screen's group for keypad ENTER.
void app_nav_bind_tap_hold(lv_obj_t *obj, app_nav_action_fn action, void *user_data);

// For explicit action buttons (Call, Answer, End, Save): a plain tap from
// touch OR keypad ENTER fires `action`. Same inverted focus styling.
void app_nav_bind_tap(lv_obj_t *obj, app_nav_action_fn action, void *user_data);

// For screens with no buttons: a touch LONG_PRESS anywhere fires `action`.
void app_nav_bind_screen_hold(lv_obj_t *screen, app_nav_action_fn action, void *user_data);

// The one-or-two-line key legend at the bottom of a screen (12pt, wrapped).
lv_obj_t *app_nav_add_legend(lv_obj_t *screen, const char *text);

// ── screen frame: reserved safe zones ────────────────────────────────────
//   y   0..21  top bar: title (left) + "WiFi SIP 9:41p" status (right)
//   y  22      1px rule
//   y  23..281 CONTENT (returned; x 4..235, 232x259)
//   y 282      1px rule
//   y 283..319 legend (two lines, 12pt)
#define APP_NAV_BAR_H        22
#define APP_NAV_CONTENT_Y    23
#define APP_NAV_CONTENT_X    4
#define APP_NAV_CONTENT_W    232
#define APP_NAV_CONTENT_H    259
#define APP_NAV_LEGEND_RULE_Y 282

// Build the frame on a screen (legend "" for none); returns the content
// container every widget must live in.
lv_obj_t *app_nav_frame(lv_obj_t *screen, const char *title, const char *legend);
void app_nav_frame_set_title(lv_obj_t *screen, const char *title);
void app_nav_frame_set_legend(lv_obj_t *screen, const char *legend);

// SIP registration state for the status text ("SIP" / "no SIP").
void app_nav_set_sip_registered(bool registered);

// Recompute the status text (WiFi / SIP / clock) at most once a second and
// push it to every frame's bar ONLY when it changed. Call from the pump loop.
void app_nav_status_tick(void);

#ifdef __cplusplus
}
#endif

#endif // APP_NAV_H
