// Screen stack manager: register screens once, then push/pop/switch them.
//
// Ported from LilyGO's T-Deck-MAX factory firmware (examples/factory/
// ui_scr_mrg.[ch], MIT License, Copyright (c) Xinyuan-LilyGO / GX.Duan /
// ShallowGreen123) and reshaped for this board's e-paper + keypad model:
//   - no load animations (a 0.7-3 s panel can't animate; every load is one
//     refresh, FULL/FAST per lvgl_glue's policy);
//   - one lv_group per live screen, swapped onto the keypad indev on every
//     activate, so PREV/NEXT can never walk into a screen underneath;
//   - fixed-size registry and stack instead of lv_mem_alloc'd linked lists;
//   - the outgoing screen is deleted only AFTER the new one is loaded
//     (deleting first is a use-after-free inside lv_disp_load_scr).
//
// Lifecycle per stack entry: create(parent) builds widgets into `parent`
// (a fresh, white, non-scrollable screen object; the default group is
// already this screen's group, so lv_group_get_default() is right);
// entry() runs on every activate (first show AND on return from a pop);
// exit() when covered by a push or about to be removed; destroy() right
// before the screen object is deleted -- null your static widget pointers
// there. Every screen is created fresh on push and destroyed on pop; nothing
// is cached across pushes.
#ifndef SCR_MGR_H
#define SCR_MGR_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*create)(lv_obj_t *parent);
    void (*entry)(void);
    void (*exit)(void);
    void (*destroy)(void);
} scr_lifecycle_t;

#define SCR_MGR_MAX_SCREENS 16
#define SCR_MGR_MAX_DEPTH   8

void scr_mgr_init(void);
bool scr_mgr_register(int id, const scr_lifecycle_t *life);

// Clear the whole stack and make `id` the root.
bool scr_mgr_switch(int id);
// Cover the current top with `id`. Refused if `id` is already on top.
bool scr_mgr_push(int id);
// Remove the top and return to the one beneath. Refused at the root.
bool scr_mgr_pop(void);
bool scr_mgr_replace_top(int id);

int scr_mgr_top_id(void);          // -1 when empty
int scr_mgr_depth(void);
lv_obj_t *scr_mgr_top_obj(void);

#ifdef __cplusplus
}
#endif

#endif // SCR_MGR_H
