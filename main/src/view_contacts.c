#include "view_contacts.h"
#include "app_nav.h"
#include "scr_mgr.h"
#include "contacts.h"
#include "phone_app.h"
#include "tca8418_keypad.h"
#include <stdio.h>
#include <string.h>
#include "lvgl.h"

// ── list ─────────────────────────────────────────────────────────────────

static lv_obj_t *s_list_screen;
static lv_obj_t *s_first_row;
static int s_edit_idx = -1;   // -1 = new contact

static const char *LIST_LEGEND = "select: outer keys, or tap\nopen: ENTER, or hold   back: sym";

static void open_edit(void *user_data)
{
    s_edit_idx = (int)(intptr_t)user_data;
    scr_mgr_push(SCR_CONTACT_EDIT);
}

static void list_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) scr_mgr_pop();
}

static lv_obj_t *add_row(lv_obj_t *list, const char *text, int idx)
{
    lv_obj_t *btn = lv_list_add_btn(list, NULL, text);
    // Never the marquee default: a wide row would refresh the panel forever.
    lv_label_set_long_mode(lv_obj_get_child(btn, 0), LV_LABEL_LONG_DOT);
    app_nav_bind_tap_hold(btn, open_edit, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(btn, list_key_cb, LV_EVENT_KEY, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g && lv_obj_get_group(btn) == NULL) lv_group_add_obj(g, btn);
    return btn;
}

static void list_create(lv_obj_t *parent)
{
    s_list_screen = parent;
    lv_obj_t *content = app_nav_frame(parent, "Contacts", LIST_LEGEND);

    lv_obj_t *list = lv_list_create(content);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    s_first_row = add_row(list, "+ New contact", -1);
    for (int i = 0; i < contacts_count(); i++) {
        const contact_t *c = contacts_get(i);
        char row[64];
        snprintf(row, sizeof(row), "%s  %s", c->name, c->number);
        add_row(list, row, i);
    }
    lv_obj_add_event_cb(parent, list_key_cb, LV_EVENT_KEY, NULL);
}

static void list_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
    lv_group_t *g = lv_group_get_default();
    if (g && s_first_row) lv_group_focus_obj(s_first_row);
}

static void list_noop(void) {}

static void list_destroy(void)
{
    s_list_screen = s_first_row = NULL;
}

static const scr_lifecycle_t s_list_life = { list_create, list_entry, list_noop, list_destroy };

// ── editor ───────────────────────────────────────────────────────────────

static lv_obj_t *s_ed_screen, *s_ed_name, *s_ed_number, *s_ed_delete;

static const char *EDIT_LEGEND =
    "fields: outer keys    type to edit\nENT on a button: act    sym: back";

static void ed_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC) scr_mgr_pop();
}

// The number field wants digits with no modifier; the name field wants
// letters. Swap the physical layer with the focus.
static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    tca8418_set_layout(ta == s_ed_number ? TCA8418_LAYOUT_DIALPAD : TCA8418_LAYOUT_QWERTY);
}

static void save_action(void *user_data)
{
    (void)user_data;
    contact_t c = {0};
    strncpy(c.name, lv_textarea_get_text(s_ed_name), CONTACT_NAME_MAX);
    strncpy(c.number, lv_textarea_get_text(s_ed_number), CONTACT_NUMBER_MAX);
    if (!c.number[0]) return;
    contacts_set(s_edit_idx, &c);
    // The list beneath was built before this edit: rebuild it on return.
    scr_mgr_pop();
    scr_mgr_replace_top(SCR_CONTACTS);
}

static void delete_action(void *user_data)
{
    (void)user_data;
    if (s_edit_idx >= 0) contacts_delete(s_edit_idx);
    scr_mgr_pop();
    scr_mgr_replace_top(SCR_CONTACTS);
}

static void call_action(void *user_data)
{
    (void)user_data;
    phone_app_dial(lv_textarea_get_text(s_ed_number));
}

static lv_obj_t *field(lv_obj_t *content, const char *label, lv_coord_t y, const char *accepted, int maxlen)
{
    lv_obj_t *l = lv_label_create(content);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_label_set_text(l, label);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *ta = lv_textarea_create(content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxlen);
    if (accepted) lv_textarea_set_accepted_chars(ta, accepted);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, y + 16);
    lv_obj_add_event_cb(ta, ed_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    return ta;
}

static lv_obj_t *button(lv_obj_t *content, const char *text, lv_coord_t x, app_nav_action_fn action)
{
    lv_obj_t *btn = lv_btn_create(content);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 70, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, 130);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    app_nav_bind_tap(btn, action, NULL);
    lv_obj_add_event_cb(btn, ed_key_cb, LV_EVENT_KEY, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g && lv_obj_get_group(btn) == NULL) lv_group_add_obj(g, btn);
    return btn;
}

static void edit_create(lv_obj_t *parent)
{
    s_ed_screen = parent;
    lv_obj_t *content = app_nav_frame(parent, s_edit_idx >= 0 ? "Contact" : "New contact", EDIT_LEGEND);

    s_ed_name   = field(content, "Name", 0, NULL, CONTACT_NAME_MAX);
    s_ed_number = field(content, "Number", 62, "0123456789*#+", CONTACT_NUMBER_MAX);

    button(content, "Call", 0, call_action);
    button(content, "Save", 80, save_action);
    s_ed_delete = button(content, "Delete", 160, delete_action);
    if (s_edit_idx < 0) lv_obj_add_flag(s_ed_delete, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(parent, ed_key_cb, LV_EVENT_KEY, NULL);
}

static void edit_entry(void)
{
    const contact_t *c = contacts_get(s_edit_idx);
    lv_textarea_set_text(s_ed_name, c ? c->name : "");
    lv_textarea_set_text(s_ed_number, c ? c->number : "");
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_focus_obj(s_ed_name);
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
}

static void edit_exit(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
}

static void edit_destroy(void)
{
    s_ed_screen = s_ed_name = s_ed_number = s_ed_delete = NULL;
}

static const scr_lifecycle_t s_edit_life = { edit_create, edit_entry, edit_exit, edit_destroy };

void view_contacts_register(void)
{
    scr_mgr_register(SCR_CONTACTS, &s_list_life);
    scr_mgr_register(SCR_CONTACT_EDIT, &s_edit_life);
}
