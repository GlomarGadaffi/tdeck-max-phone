// See scr_mgr.h for provenance (LilyGO factory ui_scr_mrg.c, MIT) and the
// e-paper/keypad deltas.
#include "scr_mgr.h"
#include "lvgl_glue.h"
#include "esp_log.h"

static const char *TAG = "SCR_MGR";

typedef struct {
    int id;
    const scr_lifecycle_t *life;
} reg_entry_t;

typedef struct {
    int id;
    const scr_lifecycle_t *life;
    lv_obj_t *obj;
    lv_group_t *group;
} stack_entry_t;

static reg_entry_t s_reg[SCR_MGR_MAX_SCREENS];
static int s_reg_count = 0;

static stack_entry_t s_stack[SCR_MGR_MAX_DEPTH];
static int s_depth = 0;

static const scr_lifecycle_t *find_life(int id)
{
    for (int i = 0; i < s_reg_count; i++) {
        if (s_reg[i].id == id) return s_reg[i].life;
    }
    return NULL;
}

// Build a screen: its own group becomes the default for the duration of
// create() so the screen's widgets land in it without every view having to
// know about groups.
static bool build(stack_entry_t *e, int id, const scr_lifecycle_t *life)
{
    e->id = id;
    e->life = life;
    e->group = lv_group_create();
    if (!e->group) return false;
    lv_group_set_default(e->group);

    e->obj = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(e->obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(e->obj, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(e->obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(e->obj, LV_OBJ_FLAG_SCROLLABLE);
    if (life->create) life->create(e->obj);
    return true;
}

// Make `e` the live screen: its group on the keypad, entry(), then load.
static void activate(stack_entry_t *e)
{
    lv_group_set_default(e->group);
    lv_indev_t *kp = lvgl_glue_keypad_indev();
    if (kp) lv_indev_set_group(kp, e->group);
    if (e->life->entry) e->life->entry();
    lvgl_glue_request_full_refresh();
    lv_disp_load_scr(e->obj);
}

// Tear an entry down. Only call once something ELSE is the loaded screen.
static void teardown(stack_entry_t *e, bool was_active)
{
    if (was_active && e->life->exit) e->life->exit();
    if (e->life->destroy) e->life->destroy();
    if (e->obj) lv_obj_del(e->obj);
    if (e->group) lv_group_del(e->group);
    e->obj = NULL;
    e->group = NULL;
    e->life = NULL;
    e->id = -1;
}

void scr_mgr_init(void)
{
    s_reg_count = 0;
    s_depth = 0;
}

bool scr_mgr_register(int id, const scr_lifecycle_t *life)
{
    if (!life || s_reg_count >= SCR_MGR_MAX_SCREENS) return false;
    if (find_life(id)) {
        ESP_LOGW(TAG, "screen %d already registered", id);
        return false;
    }
    s_reg[s_reg_count].id = id;
    s_reg[s_reg_count].life = life;
    s_reg_count++;
    return true;
}

bool scr_mgr_switch(int id)
{
    const scr_lifecycle_t *life = find_life(id);
    if (!life) return false;

    // Keep the old stack alive until the new root is on screen.
    stack_entry_t old[SCR_MGR_MAX_DEPTH];
    int old_depth = s_depth;
    for (int i = 0; i < old_depth; i++) old[i] = s_stack[i];
    s_depth = 0;

    if (!build(&s_stack[0], id, life)) return false;
    s_depth = 1;
    activate(&s_stack[0]);

    for (int i = old_depth - 1; i >= 0; i--) teardown(&old[i], i == old_depth - 1);
    return true;
}

bool scr_mgr_push(int id)
{
    const scr_lifecycle_t *life = find_life(id);
    if (!life) return false;
    if (s_depth > 0 && s_stack[s_depth - 1].id == id) return false;
    if (s_depth >= SCR_MGR_MAX_DEPTH) {
        ESP_LOGE(TAG, "stack full (%d)", s_depth);
        return false;
    }

    if (s_depth > 0) {
        stack_entry_t *top = &s_stack[s_depth - 1];
        if (top->life->exit) top->life->exit();
    }
    if (!build(&s_stack[s_depth], id, life)) return false;
    s_depth++;
    activate(&s_stack[s_depth - 1]);
    return true;
}

bool scr_mgr_pop(void)
{
    if (s_depth <= 1) return false;
    stack_entry_t gone = s_stack[s_depth - 1];
    s_depth--;
    activate(&s_stack[s_depth - 1]);
    teardown(&gone, true);
    return true;
}

bool scr_mgr_replace_top(int id)
{
    const scr_lifecycle_t *life = find_life(id);
    if (!life || s_depth == 0) return false;
    stack_entry_t gone = s_stack[s_depth - 1];
    if (!build(&s_stack[s_depth - 1], id, life)) {
        s_stack[s_depth - 1] = gone;
        return false;
    }
    activate(&s_stack[s_depth - 1]);
    teardown(&gone, true);
    return true;
}

int scr_mgr_top_id(void)
{
    return s_depth ? s_stack[s_depth - 1].id : -1;
}

int scr_mgr_depth(void)
{
    return s_depth;
}

lv_obj_t *scr_mgr_top_obj(void)
{
    return s_depth ? s_stack[s_depth - 1].obj : NULL;
}
