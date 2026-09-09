// New for this project. Keypad-driven WiFi provisioning, modeled on
// glopanel-weather's touch wizard (AP list -> password -> connect,
// NVS-persisted so only first boot re-enters it) but with every touch
// control replaced by a keypad+lv_group equivalent -- see the project plan
// Phase 0 for why (no lv_keyboard/lv_btnmatrix touch entry exists on this
// hardware's input model).
//
// Threading: this entire file runs on whichever ONE task calls
// wifi_wizard_run()/wifi_wizard_connect_saved() (see lvgl_glue.h -- LVGL is
// not thread-safe, so exactly one task ever touches it). Interactive steps
// block that task in a local "pump LVGL until an event sets a done flag"
// loop; network calls (wifi_scan_start(), wifi_sta_connect()) block it
// directly, which is fine since there is nothing else for the device to
// usefully do before it has a network.
#include "wifi_wizard.h"
#include "net_wifi.h"
#include "lvgl_glue.h"
#include "app_nav.h" // tap/hold binding + legend helpers

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_wizard";
static const char *NVS_NS        = "wifi_wizard";
static const char *NVS_KEY_SSID  = "ssid";
static const char *NVS_KEY_PASS  = "pass";

#define MAX_APS 16
#define PUMP_PERIOD_MS 20

static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;
static char s_ssid[33];
static char s_pass[65];
static bool s_ap_is_open;

static lv_group_t *s_group;
static lv_obj_t *s_screen; // one reusable screen object across every wizard step

static void pump_until(volatile bool *done)
{
    while (!*done) {
        lvgl_glue_pump();
        vTaskDelay(pdMS_TO_TICKS(PUMP_PERIOD_MS));
    }
}

// Build a new framed screen (top bar title + legend zone, see app_nav.h) and
// return its CONTENT container -- every widget a step creates goes in there.
static lv_obj_t *fresh_screen(const char *title, const char *legend)
{
    // Deleting the OLD screen before loading the new one is a use-after-free:
    // lv_disp_load_scr()/lv_scr_load_anim() reads the display's still-set
    // "active screen" pointer internally (to animate away from it), so if
    // that object is already freed by the time this runs, LVGL dereferences
    // freed memory -- crashed as a LoadProhibited panic in
    // lv_obj_get_local_style_prop on real hardware. Load the new screen
    // first (which atomically becomes the active screen), THEN delete the
    // old one now that nothing references it any more.
    lv_obj_t *old = s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_t *content = app_nav_frame(s_screen, title, legend);
    lvgl_glue_request_full_refresh(); // new screen: wipe the old one's ghosting
    lv_disp_load_scr(s_screen);
    if (old) lv_obj_del(old);
    return content;
}

// ── Step: scan + AP list ────────────────────────────────────────────────

static volatile bool s_list_done;
static volatile bool s_list_rescan;

// Fired by app_nav's tap/hold binding: keypad ENTER or a touch hold.
static void ap_btn_cb(void *user_data)
{
    int idx = (int)(intptr_t)user_data;
    if (idx < 0) { // "Rescan" row
        s_list_rescan = true;
        s_list_done = true;
        return;
    }
    strncpy(s_ssid, (const char *)s_aps[idx].ssid, sizeof(s_ssid) - 1);
    s_ap_is_open = (s_aps[idx].authmode == WIFI_AUTH_OPEN);
    s_list_rescan = false;
    s_list_done = true;
}

// Runs a scan and shows the results; returns once the user picked an AP
// (fills s_ssid/s_ap_is_open) or requested a rescan (loops internally).
static void run_scan_and_select(void)
{
    for (;;) {
        fresh_screen("Scanning for WiFi...", "");
        lvgl_glue_flush_now(); // push "Scanning..." before the blocking scan, or it'd never show

        int found = wifi_scan_start();
        s_ap_count = 0;
        if (found > 0) {
            s_ap_count = wifi_scan_get_results(s_aps, MAX_APS);
        }
        ESP_LOGI(TAG, "scan: %d found, %d shown", found, (int)s_ap_count);

        lv_obj_t *scr = fresh_screen("Select a network", "select: outer keys, or tap\nconnect: ENTER, or hold");
        lv_obj_t *list = lv_list_create(scr);
        lv_obj_set_size(list, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(list, 0, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_pad_all(list, 0, 0);
        // No scrollbar: purely cosmetic on a keypad-navigated, no-touch
        // device -- ruled out as NOT the cause of the continuous-refresh bug
        // below (kept anyway, it has no purpose here).
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *first = NULL;
        for (uint16_t i = 0; i < s_ap_count; i++) {
            char row[64];
            snprintf(row, sizeof(row), "%s  (%d dBm)%s", s_aps[i].ssid, s_aps[i].rssi,
                     s_aps[i].authmode == WIFI_AUTH_OPEN ? "" : " *");
            lv_obj_t *btn = lv_list_add_btn(list, NULL, row);
            // lv_list_add_btn()'s label defaults to LV_LABEL_LONG_SCROLL_CIRCULAR
            // -- an infinite marquee animation. Any SSID row wider than the
            // panel invalidates every animation tick, FOREVER, and this
            // display can only do a ~3.3s full-panel refresh -- confirmed on
            // real hardware as a continuous, never-settling refresh loop
            // (150+ seconds observed) plus a task-watchdog trip from the
            // resulting render backlog. Truncating with an ellipsis instead
            // is also just the right behavior for a static-view panel.
            lv_label_set_long_mode(lv_obj_get_child(btn, 0), LV_LABEL_LONG_DOT);
            app_nav_bind_tap_hold(btn, ap_btn_cb, (void *)(intptr_t)i);
            lv_group_add_obj(s_group, btn);
            if (!first) first = btn;
        }
        lv_obj_t *rescan_btn = lv_list_add_btn(list, NULL, "[ Rescan ]");
        app_nav_bind_tap_hold(rescan_btn, ap_btn_cb, (void *)(intptr_t)-1);
        lv_group_add_obj(s_group, rescan_btn);
        if (!first) first = rescan_btn;

        lv_group_focus_obj(first);

        s_list_done = false;
        pump_until(&s_list_done);
        if (!s_list_rescan) return; // s_ssid/s_ap_is_open are set; caller proceeds
        // else loop back and rescan
    }
}

// ── Step: password entry ────────────────────────────────────────────────

static volatile bool s_pw_done;
static lv_obj_t *s_pw_ta;

static void pw_ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) { // one-line textarea: ENTER while editing fires this, not a newline
        strncpy(s_pass, lv_textarea_get_text(s_pw_ta), sizeof(s_pass) - 1);
        s_pw_done = true;
    } else if (code == LV_EVENT_KEY) {
        // First ESC (while editing) is consumed by the group to leave edit
        // mode -- this fires only on a SECOND ESC, once navigation-mode is
        // back, meaning "cancel". Empty password = "go back and rescan".
        if (lv_event_get_key(e) == LV_KEY_ESC) {
            s_pass[0] = '\0';
            s_pw_done = true;
        }
    }
}

// Prompts for the password of s_ssid. On return, s_pass holds what the user
// typed, or "" if they cancelled (caller should treat that as "go back").
static void run_password_entry(void)
{
    char title[64]; // "Password for " + up to 32-char SSID + NUL, comfortably
    snprintf(title, sizeof(title), "Password for %s", s_ssid);
    lv_obj_t *scr = fresh_screen(title, "case: alt    symbols: hold outer key + letter\nsubmit: ENTER    cancel: sym twice");

    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_text(hint, s_ssid);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 4);

    s_pw_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_pw_ta, true);
    // Plaintext, not dots: on a panel that refreshes only after you pause
    // typing, seeing what you actually typed (and which case) is the only
    // feedback there is. Personal device, no shoulder-surfing concern.
    lv_textarea_set_password_mode(s_pw_ta, false);
    lv_obj_set_width(s_pw_ta, lv_pct(100));
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_add_event_cb(s_pw_ta, pw_ta_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_pw_ta, pw_ta_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(s_group, s_pw_ta);
    lv_group_focus_obj(s_pw_ta);

    s_pass[0] = '\0';
    s_pw_done = false;
    pump_until(&s_pw_done);
}

// ── Step: connect ────────────────────────────────────────────────────────

// Returns true on success. Shows a "connecting" screen (best-effort -- the
// flush is async and this call blocks for the WiFi connect attempt right
// behind it) and, on failure, an error screen the caller pumps past with
// ENTER (see wifi_wizard_run()).
static bool run_connect(const char *ssid, const char *pass)
{
    lv_obj_t *scr = fresh_screen("Connecting...", "");
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text_fmt(l, "Connecting to\n%s ...", ssid);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 8);
    lvgl_glue_flush_now(); // push "Connecting..." before the blocking connect (up to 30s)

    esp_err_t err = wifi_sta_connect(ssid, pass);
    return err == ESP_OK;
}

static volatile bool s_err_done;
static void err_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    if (lv_event_get_key(e) == LV_KEY_ENTER) s_err_done = true;
}
static void err_continue_action(void *user_data)
{
    (void)user_data;
    s_err_done = true;
}

static void show_connect_error(void)
{
    lv_obj_t *content = fresh_screen("Could not connect", "continue: ENTER, or hold");
    lv_obj_t *l = lv_label_create(content);
    lv_label_set_text(l, "Check the password and\ntry again.");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);

    // Key/hold handling stays on the SCREEN object (s_screen), not the content.
    lv_group_add_obj(s_group, s_screen);
    lv_group_focus_obj(s_screen);
    lv_obj_add_event_cb(s_screen, err_key_cb, LV_EVENT_KEY, NULL);
    app_nav_bind_screen_hold(s_screen, err_continue_action, NULL);

    s_err_done = false;
    pump_until(&s_err_done);
}

// ── NVS persistence ──────────────────────────────────────────────────────

bool wifi_wizard_has_saved_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY_SSID, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t wifi_wizard_connect_saved(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    char ssid[33] = "", pass[65] = "";
    size_t ssid_len = sizeof(ssid), pass_len = sizeof(pass);
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len); // pass may legitimately be empty (open AP)
    nvs_close(h);
    if (err != ESP_OK) return err;

    return wifi_sta_connect(ssid, pass);
}

void wifi_wizard_run(lv_group_t *group)
{
    s_group = group;

    for (;;) {
        run_scan_and_select(); // fills s_ssid, s_ap_is_open

        if (s_ap_is_open) {
            s_pass[0] = '\0';
        } else {
            run_password_entry(); // fills s_pass, or "" if the user cancelled
            if (s_pass[0] == '\0' && s_ap_is_open == false) {
                // Cancelled (ESC x2) -- only treat as "go back" for a
                // secured network; an open network's blank password is
                // legitimate and handled by the branch above.
                continue;
            }
        }

        if (run_connect(s_ssid, s_pass)) {
            save_credentials(s_ssid, s_pass);
            ESP_LOGI(TAG, "connected and saved credentials for \"%s\"", s_ssid);
            return;
        }
        show_connect_error();
        // loop back to the AP list
    }
}
