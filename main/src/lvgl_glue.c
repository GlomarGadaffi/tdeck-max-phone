// New for this project. Bridges LVGL 8.3 to:
//   - epaper_display.h's epaper_flush() (full-refresh-only, ~3.3s, 1bpp) --
//     LVGL's "direct mode" with a screen-sized single buffer is exactly the
//     documented pattern for this class of display: LVGL renders straight
//     into a buffer covering the whole screen and flush_cb always receives
//     the complete, current frame regardless of which area changed, so we
//     can ignore `area` entirely and always push the whole panel.
//   - tca8418_keypad.h's tca8418_get_key() (single press-event per call, no
//     x/y) via LV_INDEV_TYPE_KEYPAD + an lv_group, LVGL's native model for
//     a remote-control/keypad-navigated UI.
//
// Concurrency: only one hardware refresh may be in flight at a time (the
// SPI transfers inside epaper_flush() are synchronous and the panel has no
// partial-refresh path to interrupt one safely). Renders and refreshes are
// decoupled by double-buffering: LVGL packs every completed frame into
// s_pending_fb (newest wins), and maybe_start_flush() copies it to
// s_xfer_fb for the flush task only when the panel is idle AND rendering
// has been quiet for FLUSH_QUIET_MS. Input is never blocked: typing through
// a refresh is safe, and the next refresh shows the latest state.
#include "lvgl_glue.h"
#include "epaper_display.h"
#include "tca8418_keypad.h"
#include "touch_cst3530.h"

#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "LVGL_GLUE";

#define LVGL_TICK_PERIOD_MS 2
#define LVGL_FLUSH_TASK_STACK 4096 // epd_refresh() logs + SPI descriptors; 3072 was never measured

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static lv_group_t *s_group;
static lv_indev_t *s_keypad_indev;

// Two packed 1bpp frames, hardware format:
//   s_pending_fb -- written by disp_flush_cb() (LVGL's task) on every render;
//                   always holds the NEWEST rendered frame.
//   s_xfer_fb    -- owned by flush_task for the whole ~3.3s SPI transfer.
// Handing a frame to the panel is a memcpy pending->xfer, so LVGL can keep
// rendering into pending while the panel is busy with xfer, with no shared
// buffer between the two tasks.
static uint8_t s_pending_fb[EPD_BUF_SIZE];
static uint8_t s_xfer_fb[EPD_BUF_SIZE];
static volatile bool s_pending = false;      // pending_fb has a frame the panel hasn't shown yet
static int64_t s_last_render_us = 0;         // when pending_fb was last (re)written
static QueueHandle_t s_flush_q;              // depth 1: "go transfer s_xfer_fb"
static volatile bool s_flushing = false;

// Coalescing window: a frame is only pushed to the panel once no NEW frame
// has been rendered for this long. Every keystroke into the password field
// re-renders the screen; without this each character cost a full ~3.3s
// panel refresh (the e-paper's black/white inversion flashing, which reads
// as "the screen strobing"). With it, a typed word is one refresh once the
// user pauses. Discrete nav presses spaced further apart than this still
// refresh individually.
#define FLUSH_QUIET_MS 600

// Refresh-mode policy. PARTIAL (no inversion flash, ~0.5-1s) for in-screen
// updates -- typing, focus moving down a list. FULL (~3.3s, strobing, clears
// ghosting) when a screen is loaded (lvgl_glue_request_full_refresh(), called
// wherever lv_disp_load_scr() is) and every FULL_EVERY_N_PARTIAL partials
// regardless, so ghosting from a long typing session can't accumulate
// indefinitely.
// 0 = never force a full refresh by count: on real hardware the vendor
// partial waveform showed no visible ghosting at all (user, 2026-09-07), so
// FULL is screen-change-only. Raise this if ghosting ever appears.
#define FULL_EVERY_N_PARTIAL 0
static volatile bool s_force_full = true;    // first frame after boot is always full
static unsigned s_partials_since_full = 0;

static esp_timer_handle_t s_tick_timer;

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// Pack LVGL's 1-byte-per-pixel LV_COLOR_DEPTH_1 buffer into the real
// 1-bit-per-pixel hardware format epaper_flush() expects (MSB-first per
// row, 1=white 0=black). lv_color_t.full is LVGL's own on/off bit for this
// color depth (0=black, 1=white, matching lv_color_black()/lv_color_white())
// -- so the polarity should already line up with no inversion needed, but
// this is unverified on real hardware (nothing in this project has touched
// a real panel yet); if colors come out inverted, flip the condition below.
static void pack_1bpp(uint8_t *dst, const lv_color_t *src)
{
    memset(dst, 0xFF, EPD_BUF_SIZE); // default: all white
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const lv_color_t *row = &src[y * EPD_WIDTH];
        uint8_t *dst_row = &dst[y * EPD_BYTES_PER_ROW];
        for (int x = 0; x < EPD_WIDTH; x++) {
            if (!row[x].full) { // black pixel -> clear the bit
                dst_row[x / 8] &= (uint8_t)~(0x80 >> (x % 8));
            }
        }
    }
}

static void flush_task(void *arg)
{
    (void)arg;
    uint8_t mode;
    for (;;) {
        // 5s with nothing to show -> put the panel to deep sleep. Between
        // refreshes it stays powered so consecutive updates skip re-init
        // (~200ms each); the idle sleep bounds the booster's on-time. All
        // panel access stays on this task.
        if (xQueueReceive(s_flush_q, &mode, pdMS_TO_TICKS(5000)) != pdTRUE) {
            epaper_sleep();
            continue;
        }
        s_flushing = true;
        epaper_flush_mode(s_xfer_fb, (epd_refresh_mode_t)mode);
        s_flushing = false;
    }
}

// Push the newest rendered frame to the panel if one is pending, the panel
// is idle, and rendering has been quiet for FLUSH_QUIET_MS. Called from the
// same task that runs lv_timer_handler(), so it never races disp_flush_cb().
static void maybe_start_flush(void)
{
    if (!s_pending || s_flushing) return;
    if (esp_timer_get_time() - s_last_render_us < (int64_t)FLUSH_QUIET_MS * 1000) return;
    memcpy(s_xfer_fb, s_pending_fb, EPD_BUF_SIZE);
    s_pending = false;
    // Mark busy HERE, on the producer side, not only in flush_task: between
    // the enqueue below and flush_task waking up to set s_flushing itself,
    // this function could otherwise run again (next pump, 20ms later) and
    // memcpy a newer frame over s_xfer_fb while the SPI transfer is starting
    // to read it -- a torn frame. flush_task still clears it when done.
    s_flushing = true;

    // Screen changes use the vendor FAST waveform (~1.0s, mild flash) rather
    // than FULL (3.1s, heavy strobe): opening an app then pressing sym used
    // to cost ~6s of dead panel, which read as "the key doesn't work". A
    // true FULL still runs at boot and every FULL_EVERY_N_SCREEN screen
    // changes as ghost insurance; in-screen updates stay PARTIAL.
    #define FULL_EVERY_N_SCREEN 10
    static unsigned s_screens_since_full = 0;
    static bool s_booted = false;
    uint8_t mode;
    if (s_force_full) {
        s_force_full = false;
        s_partials_since_full = 0;
        if (!s_booted || ++s_screens_since_full >= FULL_EVERY_N_SCREEN) {
            mode = EPD_REFRESH_FULL;
            s_booted = true;
            s_screens_since_full = 0;
        } else {
            mode = EPD_REFRESH_FAST;
        }
    } else if (FULL_EVERY_N_PARTIAL > 0 && (int)s_partials_since_full >= FULL_EVERY_N_PARTIAL) {
        mode = EPD_REFRESH_FULL;
        s_partials_since_full = 0;
    } else {
        mode = EPD_REFRESH_PARTIAL;
        s_partials_since_full++;
    }
    xQueueOverwrite(s_flush_q, &mode);
}

void lvgl_glue_request_full_refresh(void)
{
    s_force_full = true;
}

// Defensive only: with flush_ready always signaled from disp_flush_cb below,
// LVGL never actually has to wait on the buffer. Kept so that if that ever
// changes, the wait is a yield and not the hard spin that tripped the task
// watchdog on IDLE0 during bring-up.
static void disp_wait_cb(lv_disp_drv_t *drv)
{
    (void)drv;
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)area;
    // In direct_mode, LVGL calls this once per unjoined invalidated area,
    // every time with the full-screen buffer (color_p always holds the
    // complete frame). Only the LAST area of a render pass is a complete
    // frame worth packing; earlier calls are just "buffer free" acks.
    if (lv_disp_flush_is_last(drv)) {
        pack_1bpp(s_pending_fb, color_p);
        s_pending = true;
        s_last_render_us = esp_timer_get_time();
    }
    // The pack is a full copy out of LVGL's buffer and s_pending_fb is never
    // read by the SPI transfer (that reads s_xfer_fb), so LVGL's buffer is
    // genuinely free again right here. Bring-up history for the record: an
    // earlier version handed LVGL's ONE packed buffer straight to the SPI
    // task and acked here anyway -- LVGL then re-packed into it mid-transfer
    // (garbled, endlessly refreshing screen); fixing that by acking from the
    // SPI task instead made LVGL spin-wait ~3.3s per frame (task watchdog).
    // Double-buffering + coalescing in maybe_start_flush() is the design
    // that has neither problem.
    lv_disp_flush_ready(drv);
}

// LVGL calls this on its own read-period timer (LV_INDEV_DEF_READ_PERIOD),
// from the pump task's lv_timer_handler() -- no separate keypad task needed.
// Reports a PRESSED+RELEASED pulse per physical keypress, since
// tca8418_get_key() only ever reports discrete press events (see its
// header) rather than continuous physical key state.
static void keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static bool s_pulse_down = false;
    static uint32_t s_last_lv_key = 0;

    if (s_pulse_down) {
        data->key = s_last_lv_key;
        data->state = LV_INDEV_STATE_RELEASED;
        s_pulse_down = false;
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
    // Input is NOT gated on s_flushing: rendering goes to s_pending_fb, which
    // the in-flight SPI transfer never reads, so typing through a refresh is
    // safe and the next refresh simply shows the latest state.
    if (!tca8418_key_pending()) return;

    char c = tca8418_get_key();
    if (!c) return;

    uint32_t lv_key;
    switch (c) {
        case '\r':                lv_key = LV_KEY_ENTER;     break;
        case '\b':                lv_key = LV_KEY_BACKSPACE; break;
        case TCA8418_KEY_ESC:     lv_key = LV_KEY_ESC;       break;
        // LV_KEY_PREV/NEXT, not UP/DOWN: LVGL 8's keypad indev only moves
        // GROUP FOCUS automatically for PREV/NEXT (lv_indev_keypad_proc's
        // dedicated branch). UP/DOWN are just forwarded as LV_EVENT_KEY to
        // whatever's currently focused, and a plain lv_btn (every list row
        // in this project, home menu included) does nothing with them --
        // list navigation would silently freeze on the first entry.
        case TCA8418_KEY_NAV_A:   lv_key = LV_KEY_PREV;      break; // list-up
        case TCA8418_KEY_NAV_B:   lv_key = LV_KEY_NEXT;      break; // list-down
        case TCA8418_KEY_LEFT:    lv_key = LV_KEY_LEFT;      break; // text-entry mode: cursor
        case TCA8418_KEY_RIGHT:   lv_key = LV_KEY_RIGHT;     break;
        default:                  lv_key = (uint32_t)(unsigned char)c; break; // literal char -> forwarded to focused widget (e.g. textarea)
    }

    s_last_lv_key = lv_key;
    s_pulse_down = true;
    data->key = lv_key;
    data->state = LV_INDEV_STATE_PRESSED;
}

// Pointer input: the CST3530 touch layer, polled on LVGL's indev period from
// the same single task. LVGL itself turns a press on any object that belongs
// to a group into a focus change (tap = select); app_nav's tap/hold binding
// decides what a LONG_PRESSED does (hold = open). Absent on units where the
// probe failed -- the indev is simply never registered.
static lv_indev_drv_t s_touch_drv;
#define TOUCH_LONG_PRESS_MS 600

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static int16_t s_last_x = 0, s_last_y = 0;
    int16_t x, y;
    if (touch_cst3530_read(&x, &y)) {
        s_last_x = x;
        s_last_y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
}

lv_indev_t *lvgl_glue_keypad_indev(void)
{
    return s_keypad_indev;
}

void lvgl_glue_pump(void)
{
    lv_timer_handler();
    maybe_start_flush();
}

void lvgl_glue_flush_now(void)
{
    lv_refr_now(NULL);
    // Skip the quiet window: the caller is about to block this task for a
    // long time and wants what's on screen pushed before that happens. If a
    // refresh is already in flight, wait it out (bounded) so this frame
    // isn't left sitting behind a blocking network call.
    s_last_render_us = 0;
    for (int i = 0; i < 400 && s_flushing; i++) vTaskDelay(pdMS_TO_TICKS(10));
    maybe_start_flush();
}

lv_group_t *lvgl_glue_init(void)
{
    lv_init();

    lv_color_t *buf = (lv_color_t *)heap_caps_malloc((size_t)EPD_WIDTH * EPD_HEIGHT * sizeof(lv_color_t),
                                                       MALLOC_CAP_SPIRAM);
    // 76.8 KB at 1 byte/pixel: fits internal RAM too, which is all the QEMU
    // boot test has (sdkconfig.ci.qemu disables PSRAM).
    if (!buf) buf = (lv_color_t *)heap_caps_malloc((size_t)EPD_WIDTH * EPD_HEIGHT * sizeof(lv_color_t),
                                                    MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate %d-byte LVGL draw buffer in PSRAM",
                 (int)(EPD_WIDTH * EPD_HEIGHT * sizeof(lv_color_t)));
        return NULL;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf, NULL, EPD_WIDTH * EPD_HEIGHT);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = EPD_WIDTH;
    s_disp_drv.ver_res = EPD_HEIGHT;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = disp_flush_cb;
    s_disp_drv.wait_cb = disp_wait_cb; // see disp_wait_cb's comment -- avoids a hard CPU spin while a flush is outstanding
    s_disp_drv.direct_mode = 1; // buffer is screen-sized: flush_cb always gets the full, current frame
    lv_disp_drv_register(&s_disp_drv);

    s_group = lv_group_create();
    lv_group_set_default(s_group);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    s_indev_drv.read_cb = keypad_read_cb;
    s_keypad_indev = lv_indev_drv_register(&s_indev_drv);
    lv_indev_set_group(s_keypad_indev, s_group);
    // One-shot Shift/Sym/Alt everywhere in the QWERTY layout (Meshtastic's scheme, tdeck_kbl.h):
    // UP taps arm Shift instead of navigating; lists are navigated with ALT+E / ALT+X. The
    // DIALPAD layout ignores this and stays digits-first with UP-tap navigation.
    tca8418_set_text_entry(true);

    if (touch_cst3530_present()) {
        lv_indev_drv_init(&s_touch_drv);
        s_touch_drv.type = LV_INDEV_TYPE_POINTER;
        s_touch_drv.read_cb = touch_read_cb;
        s_touch_drv.long_press_time = TOUCH_LONG_PRESS_MS;
        lv_indev_drv_register(&s_touch_drv);
        ESP_LOGI(TAG, "touch pointer indev registered (hold = %d ms)", TOUCH_LONG_PRESS_MS);
    } else {
        ESP_LOGW(TAG, "no touch controller -- keypad only");
    }

    s_flush_q = xQueueCreate(1, sizeof(uint8_t));

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(flush_task, "epd_flush", LVGL_FLUSH_TASK_STACK, NULL, 3, NULL);

    ESP_LOGI(TAG, "LVGL glue up: %dx%d, direct mode, 1bpp packed on flush", EPD_WIDTH, EPD_HEIGHT);
    return s_group;
}
