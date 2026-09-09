// See view_settings.h. Layout inside the frame's content area (232x259):
//
//   fields list (5 rows, lv_list)           ~150px
//   editor zone (hidden until a row opens)  the rest
//     - textarea editor  (password / IP / TZ / front-light)
//     - or the WiFi scan sub-list (AP rows + cancel)
//   vitals (12pt, read-only)                bottom; hidden while editing
//
// E-paper contract: after view_settings_open() nothing here ever calls
// lv_disp_load_scr() or lvgl_glue_request_full_refresh(); every change is a
// widget update -> LVGL invalidate -> one PARTIAL refresh after the 600ms
// quiet window. Long blocking work (WiFi scan ~2.5s) pushes a "scanning..."
// row first via lvgl_glue_flush_now(); connecting is non-blocking (polled by
// a bounded 1s lv_timer that is deleted on completion) because the main
// task is under the task watchdog and wifi_sta_connect() blocks up to 30s.
#include "view_settings.h"
#include "app_nav.h"
#include "lvgl_glue.h"
#include "scr_mgr.h"
#include "phone_app.h"
#include "tca8418_keypad.h"
#include "net_wifi.h"
#include "epaper_display.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "nvs.h"
#include "lvgl.h"

static const char *TAG = "settings";

// NVS: WiFi creds share the wizard's namespace/keys so both stay in sync.
#define NVS_WIFI_NS   "wifi_wizard"
#define NVS_WIFI_SSID "ssid"
#define NVS_WIFI_PASS "pass"
#define NVS_SET_NS    "settings"
#define NVS_SET_IP    "ip"      // "" or unset = DHCP
#define NVS_SET_MASK  "mask"
#define NVS_SET_GW    "gw"
#define NVS_SET_TZ    "tz"
#define NVS_SET_FL    "fl_pct"  // u8

enum { F_WIFI = 0, F_PASS, F_IP, F_TZ, F_FL, F_POWER, F_COUNT };
#define MAX_APS 12
#define CONNECT_TIMEOUT_S 30

static lv_obj_t *s_screen;
static lv_obj_t *s_content;
static lv_obj_t *s_list;
static lv_obj_t *s_rows[F_COUNT];
static lv_obj_t *s_editor;      // container: field name label + textarea
static lv_obj_t *s_editor_lbl;
static lv_obj_t *s_ta;
static lv_obj_t *s_aplist;      // WiFi scan sub-list
static lv_obj_t *s_vitals;

static int  s_editing = -1;     // field index being edited, -1 = none
static char s_ssid[33], s_pass[65];
static char s_ip[16], s_mask[16], s_gw[16], s_tz[64];
static uint8_t s_fl = 100;
static wifi_ap_record_t s_aps[MAX_APS];
static uint16_t s_ap_count;
static lv_timer_t *s_conn_timer;
static int s_conn_ticks;
static bool s_poweroff_armed;

static const char *LEGEND_NAV  = "select: outer keys, or tap\nedit: ENTER, or hold   back: sym";
static const char *LEGEND_EDIT = "type; alt: case; hold outer+letter: symbols\ncommit: ENTER   cancel: sym";
static const char *LEGEND_PICK = "select: outer keys, or tap\npick: ENTER, or hold   cancel: sym";

// ── NVS ──────────────────────────────────────────────────────────────────

static void nvs_get_str_or(nvs_handle_t h, const char *key, char *out, size_t cap, const char *dflt)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) snprintf(out, cap, "%s", dflt);
}

static void load_all(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = '\0';
    if (nvs_open(NVS_WIFI_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str_or(h, NVS_WIFI_SSID, s_ssid, sizeof(s_ssid), "");
        nvs_get_str_or(h, NVS_WIFI_PASS, s_pass, sizeof(s_pass), "");
        nvs_close(h);
    }
    s_ip[0] = s_mask[0] = s_gw[0] = '\0';
    snprintf(s_tz, sizeof(s_tz), "%s", CONFIG_TDECK_MAX_TZ);
    s_fl = 100;
    if (nvs_open(NVS_SET_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str_or(h, NVS_SET_IP,   s_ip,   sizeof(s_ip),   "");
        nvs_get_str_or(h, NVS_SET_MASK, s_mask, sizeof(s_mask), "");
        nvs_get_str_or(h, NVS_SET_GW,   s_gw,   sizeof(s_gw),   "");
        nvs_get_str_or(h, NVS_SET_TZ,   s_tz,   sizeof(s_tz),   CONFIG_TDECK_MAX_TZ);
        uint8_t v;
        if (nvs_get_u8(h, NVS_SET_FL, &v) == ESP_OK) s_fl = v;
        nvs_close(h);
    }
}

static void save_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_WIFI_SSID, s_ssid);
    nvs_set_str(h, NVS_WIFI_PASS, s_pass);
    nvs_commit(h);
    nvs_close(h);
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SET_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SET_IP, s_ip);
    nvs_set_str(h, NVS_SET_MASK, s_mask);
    nvs_set_str(h, NVS_SET_GW, s_gw);
    nvs_set_str(h, NVS_SET_TZ, s_tz);
    nvs_set_u8(h, NVS_SET_FL, s_fl);
    nvs_commit(h);
    nvs_close(h);
}

// ── apply ─────────────────────────────────────────────────────────────────

static void apply_tz(void)
{
    setenv("TZ", s_tz, 1);
    tzset();
}

static void apply_ip(void)
{
    wifi_set_static_ip(s_ip, s_mask, s_gw); // "" = DHCP; applies now if the netif exists
}

void settings_apply_at_boot(void)
{
    load_all();
    apply_tz();
    apply_ip();
    epaper_set_frontlight_pct(s_fl);
    ESP_LOGI(TAG, "applied: tz=%s ip=%s fl=%u%%", s_tz, s_ip[0] ? s_ip : "dhcp", (unsigned)s_fl);
}

// ── rows ──────────────────────────────────────────────────────────────────

static void row_set(int f, const char *text)
{
    lv_label_set_text(lv_obj_get_child(s_rows[f], 0), text);
}

static void rows_refresh(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "WiFi: %s", s_ssid[0] ? s_ssid : "(none)");
    row_set(F_WIFI, buf);
    size_t pl = strlen(s_pass);
    char mask[17];
    size_t m = pl > 16 ? 16 : pl;
    memset(mask, '*', m); mask[m] = '\0';
    snprintf(buf, sizeof(buf), "Password: %s", pl ? mask : "(none)");
    row_set(F_PASS, buf);
    if (s_ip[0]) snprintf(buf, sizeof(buf), "IP: static %s", s_ip);
    else         snprintf(buf, sizeof(buf), "IP: DHCP %s", wifi_is_connected() ? wifi_local_ip() : "");
    row_set(F_IP, buf);
    snprintf(buf, sizeof(buf), "Timezone: %s", s_tz);
    row_set(F_TZ, buf);
    snprintf(buf, sizeof(buf), "Front-light: %u%%", (unsigned)s_fl);
    row_set(F_FL, buf);
    row_set(F_POWER, s_poweroff_armed ? "Power off: ENTER again to confirm" : "Power off");
}

// ── vitals (the old Diagnostics block, read-only) ─────────────────────────

static void append(char *buf, size_t bufsize, size_t *n, const char *fmt, ...)
{
    if (*n >= bufsize) return;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + *n, bufsize - *n, fmt, ap);
    va_end(ap);
    if (written < 0) return;
    size_t avail = bufsize - *n;
    *n += ((size_t)written < avail) ? (size_t)written : avail - 1;
}

static void vitals_refresh(void)
{
    char buf[512];
    size_t n = 0;
    append(buf, sizeof(buf), &n, "RSSI %d dBm   IP %s\n", wifi_get_rssi(),
           wifi_is_connected() ? wifi_local_ip() : "-");
    append(buf, sizeof(buf), &n, "Heap %u KB (min %u, largest %u)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    append(buf, sizeof(buf), &n, "PSRAM free %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    append(buf, sizeof(buf), &n, "ESP-IDF %s\n", esp_get_idf_version());
    append(buf, sizeof(buf), &n, "Built %s %s", __DATE__, __TIME__);
    lv_label_set_text(s_vitals, buf);
}

// ── editor ────────────────────────────────────────────────────────────────

static void group_add(lv_obj_t *o)
{
    lv_group_t *g = lv_group_get_default();
    if (g && lv_obj_get_group(o) == NULL) lv_group_add_obj(g, o);
}

static void editor_close(bool refocus_row)
{
    int f = s_editing;
    s_editing = -1;
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_set_editing(g, false);
    lv_group_remove_obj(s_ta);
    lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_vitals, LV_OBJ_FLAG_HIDDEN);
    app_nav_frame_set_legend(s_screen, LEGEND_NAV);
    if (refocus_row && f >= 0 && g) lv_group_focus_obj(s_rows[f]);
}

static void connect_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_screen) { lv_timer_del(t); s_conn_timer = NULL; return; }
    s_conn_ticks++;
    if (wifi_is_connected()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi: %s  (connected)", s_ssid);
        row_set(F_WIFI, buf);
        rows_refresh(); // IP row picks up the lease
        row_set(F_WIFI, buf);
        vitals_refresh();
    } else if (s_conn_ticks < CONNECT_TIMEOUT_S) {
        return; // no text change -> no refresh
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi: %s  (no connection)", s_ssid);
        row_set(F_WIFI, buf);
    }
    lv_timer_del(s_conn_timer);
    s_conn_timer = NULL;
}

static void start_connect(void)
{
    save_wifi();
    char buf[64];
    snprintf(buf, sizeof(buf), "WiFi: %s  (connecting...)", s_ssid);
    row_set(F_WIFI, buf);
    wifi_sta_connect_start(s_ssid, s_pass); // non-blocking; see net_wifi.h
    s_conn_ticks = 0;
    if (s_conn_timer) lv_timer_del(s_conn_timer);
    s_conn_timer = lv_timer_create(connect_timer_cb, 1000, NULL); // bounded: deleted on result
}

static void commit_edit(const char *text)
{
    int f = s_editing;
    switch (f) {
    case F_PASS:
        snprintf(s_pass, sizeof(s_pass), "%s", text);
        editor_close(true);
        rows_refresh();
        if (s_ssid[0]) start_connect();
        return;
    case F_IP: {
        // "" or "dhcp" -> DHCP; else "ip mask gw" (mask/gw optional).
        char a[16] = "", b[16] = "", c[16] = "";
        if (text[0] && strcasecmp(text, "dhcp") != 0) {
            sscanf(text, "%15s %15s %15s", a, b, c);
        }
        snprintf(s_ip, sizeof(s_ip), "%s", a);
        snprintf(s_mask, sizeof(s_mask), "%s", b[0] ? b : (a[0] ? "255.255.255.0" : ""));
        snprintf(s_gw, sizeof(s_gw), "%s", c);
        save_settings();
        apply_ip();
        break;
    }
    case F_TZ:
        if (text[0]) snprintf(s_tz, sizeof(s_tz), "%s", text);
        save_settings();
        apply_tz();
        break;
    case F_FL: {
        int v = atoi(text);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_fl = (uint8_t)v;
        save_settings();
        epaper_set_frontlight_pct(s_fl);
        break;
    }
    default:
        break;
    }
    editor_close(true);
    rows_refresh();
}

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {           // ENTER in a one-line textarea
        commit_edit(lv_textarea_get_text(s_ta));
    } else if (code == LV_EVENT_KEY) {
        // First sym leaves LVGL edit mode; this fires on the SECOND sym.
        if (lv_event_get_key(e) == LV_KEY_ESC) editor_close(true);
    }
}

static void editor_open(int f, const char *label, const char *initial, const char *accepted, int maxlen)
{
    s_editing = f;
    lv_label_set_text(s_editor_lbl, label);
    lv_textarea_set_accepted_chars(s_ta, accepted);
    lv_textarea_set_max_length(s_ta, maxlen);
    lv_textarea_set_text(s_ta, initial);
    lv_obj_add_flag(s_vitals, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_aplist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
    app_nav_frame_set_legend(s_screen, LEGEND_EDIT);
    group_add(s_ta);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_focus_obj(s_ta);
        lv_group_set_editing(g, true);
    }
}

// ── WiFi scan sub-list ────────────────────────────────────────────────────

static void aplist_close(bool refocus_row)
{
    uint32_t n = lv_obj_get_child_cnt(s_aplist);
    for (uint32_t i = 0; i < n; i++) lv_group_remove_obj(lv_obj_get_child(s_aplist, i));
    lv_obj_add_flag(s_aplist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_vitals, LV_OBJ_FLAG_HIDDEN);
    app_nav_frame_set_legend(s_screen, LEGEND_NAV);
    lv_group_t *g = lv_group_get_default();
    if (refocus_row && g) lv_group_focus_obj(s_rows[F_WIFI]);
}

static void ap_pick(void *user_data)
{
    int idx = (int)(intptr_t)user_data;
    if (idx < 0) { aplist_close(true); rows_refresh(); return; }
    snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)s_aps[idx].ssid);
    bool open_ap = (s_aps[idx].authmode == WIFI_AUTH_OPEN);
    aplist_close(false);
    rows_refresh();
    if (open_ap) {
        s_pass[0] = '\0';
        start_connect();
        lv_group_t *g = lv_group_get_default();
        if (g) lv_group_focus_obj(s_rows[F_WIFI]);
    } else {
        // Straight into the password editor for the chosen network.
        editor_open(F_PASS, "Password", "", NULL, 64);
    }
}

static void aplist_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    if (lv_event_get_key(e) == LV_KEY_ESC) { aplist_close(true); rows_refresh(); }
}

static void wifi_row_open(void)
{
    row_set(F_WIFI, "WiFi: scanning...");
    lvgl_glue_flush_now(); // the scan below blocks ~2.5s; show why
    int found = wifi_scan_start();
    s_ap_count = 0;
    if (found > 0) s_ap_count = wifi_scan_get_results(s_aps, MAX_APS);

    lv_obj_clean(s_aplist);
    lv_obj_t *first = NULL;
    for (uint16_t i = 0; i < s_ap_count; i++) {
        char row[64];
        snprintf(row, sizeof(row), "%s  (%d dBm)%s", s_aps[i].ssid, s_aps[i].rssi,
                 s_aps[i].authmode == WIFI_AUTH_OPEN ? "" : " *");
        lv_obj_t *btn = lv_list_add_btn(s_aplist, NULL, row);
        lv_label_set_long_mode(lv_obj_get_child(btn, 0), LV_LABEL_LONG_DOT); // never the marquee
        app_nav_bind_tap_hold(btn, ap_pick, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, aplist_key_cb, LV_EVENT_KEY, NULL);
        group_add(btn);
        if (!first) first = btn;
    }
    lv_obj_t *cancel = lv_list_add_btn(s_aplist, NULL, s_ap_count ? "[ cancel ]" : "[ nothing found -- cancel ]");
    lv_label_set_long_mode(lv_obj_get_child(cancel, 0), LV_LABEL_LONG_DOT);
    app_nav_bind_tap_hold(cancel, ap_pick, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(cancel, aplist_key_cb, LV_EVENT_KEY, NULL);
    group_add(cancel);
    if (!first) first = cancel;

    rows_refresh();
    lv_obj_add_flag(s_vitals, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_aplist, LV_OBJ_FLAG_HIDDEN);
    app_nav_frame_set_legend(s_screen, LEGEND_PICK);
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(first);
}

// ── row actions / keys ────────────────────────────────────────────────────

static void row_action(void *user_data)
{
    int f = (int)(intptr_t)user_data;
    char cur[32];
    switch (f) {
    case F_WIFI: wifi_row_open(); break;
    case F_PASS: editor_open(F_PASS, "Password", s_pass, NULL, 64); break;
    case F_IP: {
        char init[64] = "";
        if (s_ip[0]) snprintf(init, sizeof(init), "%s %s %s", s_ip, s_mask, s_gw);
        editor_open(F_IP, "IP  (empty = DHCP, or: ip mask gw)", init, "0123456789. dhcp", 60);
        break;
    }
    case F_TZ: editor_open(F_TZ, "Timezone (POSIX)", s_tz, NULL, 60); break;
    case F_FL:
        snprintf(cur, sizeof(cur), "%u", (unsigned)s_fl);
        editor_open(F_FL, "Front-light %  (0-100)", cur, "0123456789", 3);
        break;
    case F_POWER:
        if (!s_poweroff_armed) {
            s_poweroff_armed = true;
            rows_refresh();
            break;
        }
        row_set(F_POWER, "Powering off...");
        lvgl_glue_flush_now();
        phone_app_power_off();   // does not return
        break;
    default: break;
    }
}

static void leave(void)
{
    if (s_poweroff_armed) {   // sym while armed = cancel, not leave
        s_poweroff_armed = false;
        rows_refresh();
        return;
    }
    if (s_editing >= 0) editor_close(false);
    if (!lv_obj_has_flag(s_aplist, LV_OBJ_FLAG_HIDDEN)) aplist_close(false);
    scr_mgr_pop();
}

static void row_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    if (lv_event_get_key(e) == LV_KEY_ESC) leave();
}

static void screen_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) leave();
}

// ── scr_mgr lifecycle ─────────────────────────────────────────────────────

static void settings_create(lv_obj_t *parent)
{
    load_all();
    s_poweroff_armed = false;
    s_editing = -1;
    s_screen = parent;
    s_content = app_nav_frame(s_screen, "Settings", LEGEND_NAV);

    static const char *const names[F_COUNT] = { "WiFi", "Password", "IP", "Timezone", "Front-light", "Power off" };
    s_list = lv_list_create(s_content);
    lv_obj_set_size(s_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < F_COUNT; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, names[i]);
        lv_label_set_long_mode(lv_obj_get_child(btn, 0), LV_LABEL_LONG_DOT);
        app_nav_bind_tap_hold(btn, row_action, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, row_key_cb, LV_EVENT_KEY, NULL);
        s_rows[i] = btn;
    }

    // Editor zone: field name + one-line plaintext textarea.
    s_editor = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_editor);
    lv_obj_set_size(s_editor, lv_pct(100), 70);
    lv_obj_align_to(s_editor, s_list, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_clear_flag(s_editor, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_editor_lbl = lv_label_create(s_editor);
    lv_obj_set_style_text_font(s_editor_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_editor_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_editor_lbl, lv_pct(100));
    lv_obj_align(s_editor_lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    s_ta = lv_textarea_create(s_editor);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, false); // see wifi_wizard.c: feedback IS the point
    lv_obj_set_width(s_ta, lv_pct(100));
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, 0, 18);
    lv_obj_add_event_cb(s_ta, ta_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ta, ta_event_cb, LV_EVENT_KEY, NULL);
    lv_group_remove_obj(s_ta);   // joins the group only while editing
    lv_obj_add_flag(s_editor, LV_OBJ_FLAG_HIDDEN);

    // WiFi scan sub-list, same zone, taller (scrolls by focus).
    s_aplist = lv_list_create(s_content);
    lv_obj_set_size(s_aplist, lv_pct(100), 100);
    lv_obj_align_to(s_aplist, s_list, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
    lv_obj_set_style_border_width(s_aplist, 1, 0);
    lv_obj_set_style_pad_all(s_aplist, 0, 0);
    lv_obj_set_scrollbar_mode(s_aplist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_aplist, LV_OBJ_FLAG_HIDDEN);

    // Vitals, read-only, bottom of the content area.
    s_vitals = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_vitals, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_vitals, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_vitals, lv_pct(100));
    lv_obj_align(s_vitals, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_event_cb(s_screen, screen_key_cb, LV_EVENT_KEY, NULL);
    group_add(s_screen);
}

static void settings_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
    rows_refresh();
    vitals_refresh();
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(s_rows[0]);
}

static void settings_exit(void)
{
    if (s_conn_timer) { lv_timer_del(s_conn_timer); s_conn_timer = NULL; }
}

static void settings_destroy(void)
{
    s_screen = s_content = s_list = s_editor = s_editor_lbl = s_ta = s_aplist = s_vitals = NULL;
    for (int i = 0; i < F_COUNT; i++) s_rows[i] = NULL;
    s_editing = -1;
}

static const scr_lifecycle_t s_life = { settings_create, settings_entry, settings_exit, settings_destroy };

void view_settings_register(void)
{
    scr_mgr_register(SCR_SETTINGS, &s_life);
}
