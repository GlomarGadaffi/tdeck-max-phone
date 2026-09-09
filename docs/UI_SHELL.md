# UI shell — LVGL on the e-paper, and where it came from

The phone's UI is LVGL 8.3 (ESP-IDF managed component), drawn on the
GDEQ031T10 e-paper through `lvgl_glue.c`, driven by the TCA8418 keypad and
the CST3530 touch layer. Nothing here is Arduino; the whole firmware is one
ESP-IDF project.

## Provenance

| Piece | Source | Notes |
| :-- | :-- | :-- |
| `lvgl_glue.c/h`, `app_nav.c/h`, `epaper_display.cpp/h` (FULL/FAST/PARTIAL refresh, stay-awake, front-light), `touch_cst3530.c/h`, `tca8418_keypad.cpp/h` (QWERTY layers), `xl9555.c/h` (ANT_SWITCH mask), `net_wifi.c/h` (scan, non-blocking connect, static IP), `wifi_wizard.c/h`, `view_settings.c/h` | **tdeck-glopanel**, snapshot taken 2026-09-09 (that repo had no commits yet; its author flagged `lvgl_glue.c` and `app_nav.c` as still being tuned) | DIAG log lines from that day's hang hunt were stripped. `CONFIG_TDECK_GLOPANEL_*` → `CONFIG_TDECK_MAX_*`. `app_nav` was rebuilt on top of `scr_mgr` and its BLE status replaced with SIP state. `view_settings` was moved onto the `scr_mgr` lifecycle and gained a confirmed Power off row. |
| `scr_mgr.c/h` | **LilyGO T-Deck-MAX factory firmware**, `examples/factory/ui_scr_mrg.[ch]` (MIT) | Rewritten without load animations, with one `lv_group` per live screen swapped onto the keypad indev on activate, fixed-size registry/stack, and delete-after-load. |
| Phone screens (dialer / call / incoming) | Design from LilyGO's `ui_deckpro.cpp` screen 8; state machine from this repo's pre-LVGL `app_main.cpp` | The dialer's touch pad is an `lv_btnmatrix`, not the demo's hand-rolled hit test. |
| `tca8418` **DIALPAD layout** | New here | The digit legends become the unmodified layer on the dialer/call/incoming screens (UI_DESIGN §1: a held modifier per digit is what LilyGO's factory firmware got wrong). Text-entry screens switch back to QWERTY. |

Hardware-verified facts the shell relies on (all from tdeck-glopanel's
bring-up, not re-measured here): the vendor partial waveform (`0xE0=0x02,
0xE5=0x79, 0x50=0xD7`, whole-frame, no window) refreshes in ~0.66 s
back-to-back with the panel kept powered; FULL is ~3.1 s and only used on
screen changes; LVGL's flush must double-buffer, ack immediately, and
coalesce (600 ms quiet window); `lv_list_add_btn` labels must be
`LV_LABEL_LONG_DOT` (the default marquee refreshes forever); keypad
navigation is `LV_KEY_PREV/NEXT`; the CST3530 touch axes are **not** yet
verified against the display orientation.

## Screens

```
HOME (grid: Phone / Contacts / Settings)
 ├─ PHONE   dialer: number, status, touch pad, two recent-call rows
 │    └─ CALL      calling / ringing / in call (timer, DTMF) / result, then auto-pop
 ├─ CONTACTS list → CONTACT_EDIT (name, number, Call / Save / Delete)
 └─ SETTINGS WiFi / Password / IP / Timezone / Front-light / Power off
INCOMING is pushed over whatever is showing; Answer replaces it with CALL.
```

Every screen is an `scr_mgr` lifecycle: `create(parent)` builds widgets into
a fresh screen whose group is already the default, `entry()` runs on every
show (set the keypad layout and focus here), `exit()` when covered,
`destroy()` right before deletion (null your static pointers). Screens are
rebuilt on every push; nothing is cached across pushes.

Input conventions (from tdeck-glopanel): on lists and tiles a touch **tap
focuses, a hold opens**, keypad ENTER opens; explicit action buttons
(Call, Answer, End, Save) act on a plain tap. `sym` is back everywhere.
Labels are only set when their text changed, so an idle screen costs no
refresh.

## Threading

The main task is the only LVGL owner and also runs `TincanUac::poll()`
(phone_app_tick → lvgl_glue_pump → status tick, 20 ms cadence, under the
task watchdog). Dialing from a widget callback is therefore on the right
task. The audio task uses only TincanUac's media calls, as before.

## Re-syncing with tdeck-glopanel

The files in the first table are a snapshot, not a submodule. When that
repo lands a fix (refresh policy, keypad edge cases, touch orientation),
diff against it — `CONFIG_TDECK_GLOPANEL_` ↔ `CONFIG_TDECK_MAX_` is the only
mechanical rename; `app_nav.c` and `view_settings.c` have diverged on
purpose and need a manual merge.
