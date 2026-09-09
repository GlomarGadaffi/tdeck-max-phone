#include "app_nav.h"
#include "lvgl_glue.h"
#include "scr_mgr.h"
#include "net_wifi.h"
#include "tca8418_keypad.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"

#define APP_NAV_MAX_APPS 8
#define APP_NAV_MAX_FRAMES 12

typedef struct {
    const char *name;
    const char *symbol;
    int scr_id;
} app_nav_entry_t;

static app_nav_entry_t s_entries[APP_NAV_MAX_APPS];
static int s_entry_count = 0;

// ── tap/hold policy ──────────────────────────────────────────────────────

typedef struct {
    app_nav_action_fn action;
    void *user_data;
    bool tap_fires;   // app_nav_bind_tap: a touch CLICKED fires too
} binding_t;

static bool event_from_pointer(void)
{
    lv_indev_t *indev = lv_indev_get_act();
    return indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
}

static void tap_hold_cb(lv_event_t *e)
{
    binding_t *b = (binding_t *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        if (event_from_pointer() && lv_obj_get_group(obj)) lv_group_focus_obj(obj);
        break;
    case LV_EVENT_LONG_PRESSED:
        if (event_from_pointer() && !b->tap_fires && b->action) b->action(b->user_data);
        break;
    case LV_EVENT_CLICKED:
        // Keypad ENTER always; a touch tap only for explicit action buttons.
        if ((b->tap_fires || !event_from_pointer()) && b->action) b->action(b->user_data);
        break;
    case LV_EVENT_DELETE:
        lv_mem_free(b);
        break;
    default:
        break;
    }
}

// Selection indicator: an INVERTED row (black bar, white text) for BOTH
// focus states. The mono theme only styles LV_STATE_FOCUS_KEY with a 1px
// outline; a touch press sets plain LV_STATE_FOCUSED, which it leaves
// unstyled -- so tapping a row highlighted nothing on hardware. Inverting is
// also far more legible on 1-bit e-paper than a hairline outline.
static void style_focus_inverted(lv_obj_t *obj)
{
    static const lv_state_t states[] = { LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        lv_obj_set_style_bg_color(obj, lv_color_black(), states[i]);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, states[i]);
        lv_obj_set_style_text_color(obj, lv_color_white(), states[i]);
        lv_obj_set_style_outline_width(obj, 0, states[i]);
    }
}

static void bind(lv_obj_t *obj, app_nav_action_fn action, void *user_data, bool tap_fires)
{
    binding_t *b = (binding_t *)lv_mem_alloc(sizeof(binding_t));
    if (!b) return;
    b->action = action;
    b->user_data = user_data;
    b->tap_fires = tap_fires;
    lv_obj_add_event_cb(obj, tap_hold_cb, LV_EVENT_ALL, b);
    style_focus_inverted(obj);
}

void app_nav_bind_tap_hold(lv_obj_t *obj, app_nav_action_fn action, void *user_data)
{
    bind(obj, action, user_data, false);
}

void app_nav_bind_tap(lv_obj_t *obj, app_nav_action_fn action, void *user_data)
{
    bind(obj, action, user_data, true);
}

static void screen_hold_cb(lv_event_t *e)
{
    binding_t *b = (binding_t *)lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_LONG_PRESSED:
        if (event_from_pointer() && b->action) b->action(b->user_data);
        break;
    case LV_EVENT_DELETE:
        lv_mem_free(b);
        break;
    default:
        break;
    }
}

void app_nav_bind_screen_hold(lv_obj_t *screen, app_nav_action_fn action, void *user_data)
{
    binding_t *b = (binding_t *)lv_mem_alloc(sizeof(binding_t));
    if (!b) return;
    b->action = action;
    b->user_data = user_data;
    b->tap_fires = false;
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, screen_hold_cb, LV_EVENT_ALL, b);
}

// ── legend ───────────────────────────────────────────────────────────────

lv_obj_t *app_nav_add_legend(lv_obj_t *screen, const char *text)
{
    // 12pt, written as TWO explicit lines so the height is predictable.
    lv_obj_t *l = lv_label_create(screen);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); // never the marquee default
    lv_obj_set_width(l, lv_pct(96));
    lv_obj_set_style_text_line_space(l, 2, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    return l;
}

// ── screen frame ─────────────────────────────────────────────────────────

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *legend;
} frame_t;

static frame_t s_frames[APP_NAV_MAX_FRAMES];
static int s_frame_count = 0;
static char s_status_text[32] = "";
static bool s_sip_registered = false;

static void frame_deleted_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_target(e);
    for (int i = 0; i < s_frame_count; i++) {
        if (s_frames[i].screen != scr) continue;
        s_frames[i] = s_frames[--s_frame_count];
        return;
    }
}

static lv_obj_t *hrule(lv_obj_t *screen, lv_coord_t y)
{
    lv_obj_t *r = lv_obj_create(screen);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 1);
    lv_obj_set_pos(r, 0, y);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

lv_obj_t *app_nav_frame(lv_obj_t *screen, const char *title, const char *legend)
{
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *t = lv_label_create(screen);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, 116);
    lv_label_set_text(t, title ? title : "");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, APP_NAV_CONTENT_X, 3);

    lv_obj_t *s = lv_label_create(screen);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, 112);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s, s_status_text);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -APP_NAV_CONTENT_X, 5);

    hrule(screen, APP_NAV_BAR_H);

    lv_obj_t *c = lv_obj_create(screen);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, APP_NAV_CONTENT_X, APP_NAV_CONTENT_Y);
    lv_obj_set_size(c, APP_NAV_CONTENT_W, APP_NAV_CONTENT_H);
    lv_obj_set_style_bg_color(c, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    // Not clickable: a touch on empty content must reach the SCREEN so a
    // screen-hold binding fires.
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);

    hrule(screen, APP_NAV_LEGEND_RULE_Y);
    lv_obj_t *lg = app_nav_add_legend(screen, legend ? legend : "");

    if (s_frame_count < APP_NAV_MAX_FRAMES) {
        s_frames[s_frame_count].screen = screen;
        s_frames[s_frame_count].title = t;
        s_frames[s_frame_count].status = s;
        s_frames[s_frame_count].legend = lg;
        s_frame_count++;
        lv_obj_add_event_cb(screen, frame_deleted_cb, LV_EVENT_DELETE, NULL);
    }
    return c;
}

void app_nav_frame_set_title(lv_obj_t *screen, const char *title)
{
    for (int i = 0; i < s_frame_count; i++) {
        if (s_frames[i].screen == screen) {
            lv_label_set_text(s_frames[i].title, title);
            return;
        }
    }
}

void app_nav_frame_set_legend(lv_obj_t *screen, const char *legend)
{
    for (int i = 0; i < s_frame_count; i++) {
        if (s_frames[i].screen == screen && s_frames[i].legend) {
            lv_label_set_text(s_frames[i].legend, legend ? legend : "");
            return;
        }
    }
}

void app_nav_set_sip_registered(bool registered)
{
    s_sip_registered = registered;
}

void app_nav_status_tick(void)
{
    static int64_t s_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < 1000000) return; // at most once a second
    s_last_us = now;

    char clock[8] = "--:--";
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 >= 2020) {
        int h12 = tm.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(clock, sizeof(clock), "%d:%02d%c", h12, tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
    }

    char text[sizeof(s_status_text)];
    snprintf(text, sizeof(text), "%s %s %s",
             wifi_is_connected() ? "WiFi" : "noWiFi",
             s_sip_registered ? "SIP" : "noSIP",
             clock);
    if (strcmp(text, s_status_text) == 0) return; // unchanged: no invalidation, no refresh

    memcpy(s_status_text, text, sizeof(s_status_text));
    for (int i = 0; i < s_frame_count; i++) lv_label_set_text(s_frames[i].status, s_status_text);
}

// ── home grid ────────────────────────────────────────────────────────────
// Tiles in two columns, in registration order: a symbol above a name, the
// focused tile inverted. LilyGO's factory launcher is the same idea with
// bitmap icons and swipe paging; with three to five apps one page is plenty.

#define TILE_W   108
#define TILE_H    76
#define TILE_GAP   8

static lv_obj_t *s_first_tile;

static void open_action(void *user_data)
{
    scr_mgr_push((int)(intptr_t)user_data);
}

void app_nav_register(const char *name, const char *symbol, int scr_id)
{
    if (s_entry_count >= APP_NAV_MAX_APPS) return;
    s_entries[s_entry_count].name = name;
    s_entries[s_entry_count].symbol = symbol;
    s_entries[s_entry_count].scr_id = scr_id;
    s_entry_count++;
}

static void home_create(lv_obj_t *parent)
{
    lv_obj_t *content = app_nav_frame(parent, "T-Deck Phone",
                                      "select: outer keys, or tap\nopen: ENTER, or hold");
    s_first_tile = NULL;
    lv_group_t *g = lv_group_get_default();
    for (int i = 0; i < s_entry_count; i++) {
        lv_obj_t *tile = lv_btn_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, (i % 2) * (TILE_W + TILE_GAP) + 4, (i / 2) * (TILE_H + TILE_GAP) + 4);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_black(), 0);
        lv_obj_set_style_radius(tile, 6, 0);
        lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sym = lv_label_create(tile);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_24, 0);
        lv_label_set_text(sym, s_entries[i].symbol);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *name = lv_label_create(tile);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, TILE_W - 8);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, s_entries[i].name);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);

        app_nav_bind_tap_hold(tile, open_action, (void *)(intptr_t)s_entries[i].scr_id);
        if (g && lv_obj_get_group(tile) == NULL) lv_group_add_obj(g, tile);
        if (!s_first_tile) s_first_tile = tile;
    }
}

static void home_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
    // Refocus on EVERY show: coming back from an app otherwise leaves the
    // group's focus wherever it was.
    lv_group_t *g = lv_group_get_default();
    if (g && s_first_tile) lv_group_focus_obj(s_first_tile);
}

static void home_noop(void) {}

static void home_destroy(void)
{
    s_first_tile = NULL;
}

static const scr_lifecycle_t s_home_life = { home_create, home_entry, home_noop, home_destroy };

void app_nav_init(void)
{
    scr_mgr_register(SCR_HOME, &s_home_life);
}

void app_nav_show_home(void)
{
    scr_mgr_switch(SCR_HOME);
}
