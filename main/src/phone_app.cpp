// See phone_app.h. The SIP/RTP engine is tincan-core's TincanUac; this file
// is the keypad/touch state machine around it (ported from the pre-LVGL
// app_main.cpp, where it was a switch on a UiState enum and an e-paper
// pictogram) plus the three phone screens.
//
// Screen updates follow the e-paper rule every view in this project obeys:
// set a label only when its text actually changed, so a state that doesn't
// move costs no refresh.
#include "phone_app.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include <cmath>
#include <cstring>
#include <string>

#include "poc_config.h"
#include "net_wifi.h"
#include "xl9555.h"
#include "pmu_sy6970.h"
#include "es8311_audio.h"
#include "tca8418_keypad.h"
#include "lvgl_glue.h"
#include "scr_mgr.h"
#include "app_nav.h"
#include "contacts.h"
#include "call_log.h"
#include "tincan_uac.hpp"
#include "playout_buffer.hpp"
#include "tone_gen.h"

static const char *TAG = "PHONE";

// ── state ────────────────────────────────────────────────────────────────

enum class UiState { Idle, Calling, Incoming, InCall };

static TincanUac s_uac_obj;
static TincanUac *s_uac = nullptr;   // non-null once phone_app_start() succeeded
static bool s_registered = false;

static UiState s_state = UiState::Idle;
static std::string s_dial;           // dialer buffer
static std::string s_peer;           // number of the call in progress
static call_dir_t s_dir = CALL_DIR_OUT;
static bool s_ringing_shown = false;
static int64_t s_connect_us = 0;     // when media started (InCall)
static std::string s_result;         // "Busy", "Call ended", ... shown after a call
static int64_t s_result_until_us = 0;
static int s_shown_secs = -1;

#define DIAL_BUFFER_MAX 20
#define RESULT_HOLD_MS  2500

static bool is_dial_char(char c)
{
    return (c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+';
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

static void format_duration(uint32_t s, char *buf, size_t len)
{
    if (s >= 3600) snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)s / 3600, ((unsigned long)s / 60) % 60, (unsigned long)s % 60);
    else           snprintf(buf, len, "%lu:%02lu", (unsigned long)s / 60, (unsigned long)s % 60);
}

// ── audio pump task (unchanged from the pre-LVGL firmware) ───────────────
// Paced solely by the blocking i2s_channel_read() -- one 20 ms frame in, one
// out, 50 frames/sec. Adding any vTaskDelay here directly reduces packet
// rate and grows one-way latency for the whole call.

static void audio_task(void *)
{
    int16_t pcm[POC_FRAME_SAMPLES];
    int16_t micbuf[POC_FRAME_SAMPLES];
    bool pumping = false;

    static PlayoutBuffer jb(POC_JITTER_MAX_MS * POC_SAMPLE_RATE_HZ / 1000,
                            POC_JITTER_TARGET_MS * POC_SAMPLE_RATE_HZ / 1000);

    tone_gen_t ringTone{};
    int ringFrame = 0;

    const int32_t rxGainQ8 = (int32_t)(powf(10.0f, POC_RX_GAIN_DB / 20.0f) * 256.0f);
    const int32_t duckQ8   = (POC_DUCK_DB == 0.0f)
                             ? 256
                             : (int32_t)(powf(10.0f, POC_DUCK_DB / 20.0f) * 256.0f);
    const int hangoverFrames = POC_DUCK_HANGOVER_MS / 20;
    int duckFrames = 0;
    int statFrames = 0;
    int64_t statMicSum = 0, statRxSum = 0;
    int32_t statMicN = 0, statRxN = 0;
    int16_t statMicPeak = 0, statRxPeak = 0;

    for (;;) {
        if (!s_uac || !s_uac->inCall()) {
            pumping = false;
            duckFrames = 0;
            if (s_uac && s_uac->hasIncomingCall()) {
                // Ringer: 2 s on / 4 s off, paced by the blocking speaker write.
                const bool on = (ringFrame % 300) < 100;
                if (on) {
                    tone_gen_fill(&ringTone, 440.0f, 480.0f, POC_RING_AMPL,
                                  POC_SAMPLE_RATE_HZ, pcm, POC_FRAME_SAMPLES);
                } else {
                    memset(pcm, 0, sizeof(pcm));
                }
                audio_hardware_write_spk(pcm, POC_FRAME_SAMPLES);
                ringFrame++;
                continue;
            }
            ringFrame = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!pumping) {
            s_uac->flushRtpBacklog();
            jb.clear();
            pumping = true;
            statFrames = 0;
        }

        if (++statFrames >= 250) {
            statFrames = 0;
            ESP_LOGI(TAG, "rtp census: tx=%lu rx=%lu lost=%lu oo=%lu rej=%lu | jb=%u smp under=%llu"
                          " | mic~%ld peak %d | rx~%ld peak %d%s",
                     (unsigned long)s_uac->rtpTx(), (unsigned long)s_uac->rtpRx(),
                     (unsigned long)s_uac->rtpLost(), (unsigned long)s_uac->rtpOutOfOrder(),
                     (unsigned long)s_uac->rtpRejected(),
                     (unsigned)jb.getLength(), (unsigned long long)jb.getUnderruns(),
                     (long)(statMicN ? statMicSum / statMicN : 0), (int)statMicPeak,
                     (long)(statRxN ? statRxSum / statRxN : 0), (int)statRxPeak,
                     s_uac->lastTxErrno() ? " TX FAILING" : "");
            if (s_uac->lastTxErrno()) {
                ESP_LOGE(TAG, "  sendto() errno %d -- RTP is not leaving the board",
                         s_uac->lastTxErrno());
            }
            statMicSum = statRxSum = 0;
            statMicN = statRxN = 0;
            statMicPeak = statRxPeak = 0;
        }

        size_t got = audio_hardware_read_mic(micbuf, POC_FRAME_SAMPLES);
        if (got > 0) {
            if (duckFrames > 0) {
                for (size_t i = 0; i < got; i++) {
                    micbuf[i] = (int16_t)((micbuf[i] * duckQ8) >> 8);
                }
                duckFrames--;
            }
            for (size_t i = 0; i < got; i++) {
                int16_t a = micbuf[i] < 0 ? (int16_t)-micbuf[i] : micbuf[i];
                statMicSum += a;
                if (a > statMicPeak) statMicPeak = a;
            }
            statMicN += (int32_t)got;
            s_uac->sendAudioFrame(micbuf, got);
        }

        s_uac->pumpRx(jb);
        const size_t rx = POC_FRAME_SAMPLES;
        const bool real = jb.read(pcm, rx);
        if (real) {
            int32_t sumAbs = 0;
            int16_t framePeak = 0;
            for (size_t i = 0; i < rx; i++) {
                int16_t a = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
                sumAbs += a;
                if (a > framePeak) framePeak = a;
            }
            statRxSum += sumAbs;
            statRxN += (int32_t)rx;
            if (framePeak > statRxPeak) statRxPeak = framePeak;

            if ((sumAbs / (int32_t)rx) > POC_DUCK_THRESHOLD && duckQ8 != 256) {
                duckFrames = hangoverFrames;
            }

            if (rxGainQ8 != 256) {
                for (size_t i = 0; i < rx; i++) {
                    int32_t v = ((int32_t)pcm[i] * rxGainQ8) >> 8;
                    if (v >  32767) v =  32767;
                    if (v < -32768) v = -32768;
                    pcm[i] = (int16_t)v;
                }
            }
        }
        audio_hardware_write_spk(pcm, rx);
    }
}

// ── dialer screen ────────────────────────────────────────────────────────

static lv_obj_t *s_dl_screen, *s_dl_number, *s_dl_status, *s_dl_btnm;
static lv_obj_t *s_dl_recent[2];

static const char *DIALER_LEGEND =
    "digits: keys or tap    DEL: erase\nENT: call (empty = redial)    sym: back";

static void dialer_refresh(void)
{
    if (!s_dl_screen) return;
    // Show the tail of a long buffer: that is the part being typed.
    const size_t max_shown = 14;
    const char *shown = s_dial.size() > max_shown ? s_dial.c_str() + (s_dial.size() - max_shown) : s_dial.c_str();
    set_text(s_dl_number, shown);

    char status[48];
    if (!s_uac)              snprintf(status, sizeof(status), "No network");
    else if (!s_registered)  snprintf(status, sizeof(status), "Ext %s  not registered", POC_SIP_EXT_SELF);
    else                     snprintf(status, sizeof(status), "Ext %s  ready", POC_SIP_EXT_SELF);
    set_text(s_dl_status, status);

    for (int i = 0; i < 2; i++) {
        const call_log_entry_t *e = call_log_get(i);
        char row[64];
        if (!e) {
            snprintf(row, sizeof(row), "%s", i == 0 ? "no recent calls" : "");
        } else {
            char dur[16];
            format_duration(e->duration_s, dur, sizeof(dur));
            snprintf(row, sizeof(row), "%s  %s  %s",
                     e->dir == CALL_DIR_OUT ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                     contacts_display_name(e->number),
                     e->answered ? dur : "missed");
        }
        set_text(lv_obj_get_child(s_dl_recent[i], 0), row);
    }
}

static void dial_from_buffer(void)
{
    if (!s_dial.empty()) {
        std::string target = s_dial;
        s_dial.clear();
        phone_app_dial(target.c_str());
        return;
    }
    // ENT on an empty buffer redials the last number that connected. Inert
    // when there is nothing stored.
    const char *last = call_log_last_dialled();
    if (last[0]) phone_app_dial(last);
    else ESP_LOGI(TAG, "ENT from empty dialer: nothing to redial yet");
}

static void dialer_add(char c)
{
    if (s_dial.size() >= DIAL_BUFFER_MAX) return;
    s_dial += c;
    dialer_refresh();
}

static void dialer_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        if (!s_dial.empty()) { s_dial.clear(); dialer_refresh(); }
        else scr_mgr_pop();
    } else if (key == LV_KEY_BACKSPACE) {
        if (!s_dial.empty()) { s_dial.pop_back(); dialer_refresh(); }
    } else if (key == LV_KEY_ENTER) {
        // Only when the SCREEN is focused: a focused recent row's ENTER is
        // its own tap/hold action.
        if (lv_event_get_target(e) == s_dl_screen) dial_from_buffer();
    } else if (key < 0x80 && is_dial_char((char)key)) {
        dialer_add((char)key);
    }
}

static void dialer_btnm_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(m);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    const char *txt = lv_btnmatrix_get_btn_text(m, id);
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (!s_dial.empty()) { s_dial.pop_back(); dialer_refresh(); }
    } else if (strcmp(txt, LV_SYMBOL_CALL) == 0) {
        dial_from_buffer();
    } else if (txt[0] && !txt[1]) {
        dialer_add(txt[0]);
    }
}

static void recent_action(void *user_data)
{
    const call_log_entry_t *e = call_log_get((int)(intptr_t)user_data);
    if (e) phone_app_dial(e->number);
}

static void dialer_create(lv_obj_t *parent)
{
    s_dl_screen = parent;
    lv_obj_t *content = app_nav_frame(parent, "Phone", DIALER_LEGEND);

    s_dl_number = lv_label_create(content);
    lv_obj_set_style_text_font(s_dl_number, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_dl_number, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_dl_number, lv_pct(100));
    lv_obj_set_style_text_align(s_dl_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dl_number, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(s_dl_number, "");

    s_dl_status = lv_label_create(content);
    lv_obj_set_style_text_font(s_dl_status, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_dl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_dl_status, lv_pct(100));
    lv_obj_set_style_text_align(s_dl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dl_status, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_dl_status, "");

    // Touch keypad. Direct tap = digit here (no hold policy: a dial pad is
    // exactly the widget a tap should act on). Deliberately NOT in the
    // keypad group and not click-focusable: the physical keys already do
    // this, and PREV/NEXT should only step between the recent rows.
    static const char *btnm_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "*", "0", "#", "\n",
        LV_SYMBOL_BACKSPACE, LV_SYMBOL_CALL, "",
    };
    s_dl_btnm = lv_btnmatrix_create(content);
    lv_btnmatrix_set_map(s_dl_btnm, btnm_map);
    lv_obj_set_size(s_dl_btnm, lv_pct(100), 158);
    lv_obj_align(s_dl_btnm, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_pad_all(s_dl_btnm, 2, 0);
    lv_obj_set_style_pad_gap(s_dl_btnm, 4, 0);
    lv_obj_set_style_border_width(s_dl_btnm, 0, 0);
    lv_obj_set_style_text_font(s_dl_btnm, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_dl_btnm, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_dl_btnm, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_dl_btnm, 4, LV_PART_ITEMS);
    lv_obj_clear_flag(s_dl_btnm, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(s_dl_btnm);
    lv_obj_add_event_cb(s_dl_btnm, dialer_btnm_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_group_t *g = lv_group_get_default();
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_btn_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 20);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 216 + i * 22);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_text(l, "");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        app_nav_bind_tap_hold(row, recent_action, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, dialer_key_cb, LV_EVENT_KEY, NULL);
        if (g && lv_obj_get_group(row) == NULL) lv_group_add_obj(g, row);
        s_dl_recent[i] = row;
    }

    // The screen itself is the default key target: digits typed with
    // nothing else focused land here.
    lv_obj_add_event_cb(parent, dialer_key_cb, LV_EVENT_KEY, NULL);
    if (g) lv_group_add_obj(g, parent);
}

static void dialer_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_DIALPAD);
    dialer_refresh();
    lv_group_t *g = lv_group_get_default();
    if (g && s_dl_screen) lv_group_focus_obj(s_dl_screen);
}

static void dialer_exit(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
}

static void dialer_destroy(void)
{
    s_dl_screen = s_dl_number = s_dl_status = s_dl_btnm = NULL;
    s_dl_recent[0] = s_dl_recent[1] = NULL;
}

static const scr_lifecycle_t s_dialer_life = { dialer_create, dialer_entry, dialer_exit, dialer_destroy };

// ── call screen (calling / ringing / in call / result) ───────────────────

static lv_obj_t *s_cl_screen, *s_cl_name, *s_cl_number, *s_cl_status, *s_cl_timer, *s_cl_end;

static const char *CALL_LEGEND = "hang up: DEL or ENT, or tap End\ndigits: send tones";

static void call_refresh(void)
{
    if (!s_cl_screen) return;
    set_text(s_cl_name, contacts_display_name(s_peer.c_str()));
    set_text(s_cl_number, contacts_find_by_number(s_peer.c_str()) >= 0 ? s_peer.c_str() : "");

    const char *status = "";
    char timer[16] = "";
    switch (s_state) {
    case UiState::Calling:
        status = s_ringing_shown ? "Ringing" : "Calling...";
        break;
    case UiState::InCall: {
        status = "In call";
        int secs = (int)((esp_timer_get_time() - s_connect_us) / 1000000);
        format_duration((uint32_t)secs, timer, sizeof(timer));
        s_shown_secs = secs;
        break;
    }
    default:
        status = s_result.c_str();
        break;
    }
    set_text(s_cl_status, status);
    set_text(s_cl_timer, timer);
    if (s_cl_end) {
        bool ended = (s_state != UiState::Calling && s_state != UiState::InCall);
        if (ended) lv_obj_add_flag(s_cl_end, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(s_cl_end, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hangup_action(void *)
{
    if (!s_uac) return;
    if (s_state == UiState::Calling) {
        s_uac->hangup();   // CANCEL; the 487 lands in the tick's ended branch
        set_text(s_cl_status, "Cancelling");
    } else if (s_state == UiState::InCall) {
        s_uac->hangup();
    }
}

static void call_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_BACKSPACE || key == LV_KEY_ENTER) {
        if (lv_event_get_target(e) == s_cl_screen) hangup_action(NULL);
    } else if (key < 0x80 && is_dial_char((char)key)) {
        if (s_uac && s_state == UiState::InCall) s_uac->sendDtmf((char)key);
    }
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, lv_coord_t y, bool filled,
                               app_nav_action_fn action)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(btn, filled ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(btn, filled ? lv_color_white() : lv_color_black(), 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    app_nav_bind_tap(btn, action, NULL);
    // Touch-only affordance: keep it OUT of the keypad group so DEL/ENT
    // stay the keypad's hang-up/answer keys with the screen focused.
    lv_group_remove_obj(btn);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return btn;
}

static void call_create(lv_obj_t *parent)
{
    s_cl_screen = parent;
    lv_obj_t *content = app_nav_frame(parent, "Call", CALL_LEGEND);

    s_cl_name = lv_label_create(content);
    lv_obj_set_style_text_font(s_cl_name, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_cl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cl_name, lv_pct(100));
    lv_obj_set_style_text_align(s_cl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cl_name, LV_ALIGN_TOP_MID, 0, 24);
    lv_label_set_text(s_cl_name, "");

    s_cl_number = lv_label_create(content);
    lv_obj_set_style_text_font(s_cl_number, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_cl_number, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cl_number, lv_pct(100));
    lv_obj_set_style_text_align(s_cl_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cl_number, LV_ALIGN_TOP_MID, 0, 58);
    lv_label_set_text(s_cl_number, "");

    s_cl_status = lv_label_create(content);
    lv_obj_set_style_text_font(s_cl_status, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_cl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_cl_status, lv_pct(100));
    lv_obj_set_style_text_align(s_cl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cl_status, LV_ALIGN_TOP_MID, 0, 92);
    lv_label_set_text(s_cl_status, "");

    s_cl_timer = lv_label_create(content);
    lv_obj_set_style_text_font(s_cl_timer, &lv_font_montserrat_32, 0);
    lv_obj_set_width(s_cl_timer, lv_pct(100));
    lv_obj_set_style_text_align(s_cl_timer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cl_timer, LV_ALIGN_TOP_MID, 0, 122);
    lv_label_set_text(s_cl_timer, "");

    s_cl_end = action_button(content, "End", 190, true, hangup_action);

    lv_obj_add_event_cb(parent, call_key_cb, LV_EVENT_KEY, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, parent);
}

static void call_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_DIALPAD);
    call_refresh();
    lv_group_t *g = lv_group_get_default();
    if (g && s_cl_screen) lv_group_focus_obj(s_cl_screen);
}

static void call_exit(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
}

static void call_destroy(void)
{
    s_cl_screen = s_cl_name = s_cl_number = s_cl_status = s_cl_timer = s_cl_end = NULL;
}

static const scr_lifecycle_t s_call_life = { call_create, call_entry, call_exit, call_destroy };

// ── incoming screen ──────────────────────────────────────────────────────

static lv_obj_t *s_in_screen, *s_in_name, *s_in_number;

static const char *INCOMING_LEGEND = "answer: ENT, or tap Answer\ndecline: DEL, or tap Decline";

static void incoming_refresh(void)
{
    if (!s_in_screen) return;
    set_text(s_in_name, contacts_display_name(s_peer.c_str()));
    set_text(s_in_number, contacts_find_by_number(s_peer.c_str()) >= 0 ? s_peer.c_str() : "");
}

static void end_call_bookkeeping(bool answered)
{
    uint16_t dur = 0;
    if (answered && s_connect_us) dur = (uint16_t)((esp_timer_get_time() - s_connect_us) / 1000000);
    call_log_add(s_peer.c_str(), s_dir, answered, dur);
    audio_hardware_set_amp(false);
    s_state = UiState::Idle;
    s_connect_us = 0;
    s_shown_secs = -1;
}

static void answer_action(void *)
{
    if (!s_uac || s_state != UiState::Incoming) return;
    s_uac->answer();
    s_state = UiState::InCall;
    s_connect_us = esp_timer_get_time();
    audio_hardware_set_amp(true);
    scr_mgr_replace_top(SCR_CALL);
}

static void decline_action(void *)
{
    if (!s_uac || s_state != UiState::Incoming) return;
    s_uac->reject();
    end_call_bookkeeping(false);
    if (scr_mgr_top_id() == SCR_INCOMING) scr_mgr_pop();
}

static void incoming_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) answer_action(NULL);
    else if (key == LV_KEY_BACKSPACE || key == LV_KEY_ESC) decline_action(NULL);
}

static void incoming_create(lv_obj_t *parent)
{
    s_in_screen = parent;
    lv_obj_t *content = app_nav_frame(parent, "Incoming call", INCOMING_LEGEND);

    s_in_name = lv_label_create(content);
    lv_obj_set_style_text_font(s_in_name, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_in_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_in_name, lv_pct(100));
    lv_obj_set_style_text_align(s_in_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_in_name, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(s_in_name, "");

    s_in_number = lv_label_create(content);
    lv_obj_set_style_text_font(s_in_number, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_in_number, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_in_number, lv_pct(100));
    lv_obj_set_style_text_align(s_in_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_in_number, LV_ALIGN_TOP_MID, 0, 64);
    lv_label_set_text(s_in_number, "");

    action_button(content, "Answer", 130, true, answer_action);
    action_button(content, "Decline", 184, false, decline_action);

    lv_obj_add_event_cb(parent, incoming_key_cb, LV_EVENT_KEY, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, parent);
}

static void incoming_entry(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_DIALPAD);
    incoming_refresh();
    lv_group_t *g = lv_group_get_default();
    if (g && s_in_screen) lv_group_focus_obj(s_in_screen);
}

static void incoming_exit(void)
{
    tca8418_set_layout(TCA8418_LAYOUT_QWERTY);
}

static void incoming_destroy(void)
{
    s_in_screen = s_in_name = s_in_number = NULL;
}

static const scr_lifecycle_t s_incoming_life = { incoming_create, incoming_entry, incoming_exit, incoming_destroy };

// ── public API ───────────────────────────────────────────────────────────

void phone_app_register_screens(void)
{
    scr_mgr_register(SCR_PHONE, &s_dialer_life);
    scr_mgr_register(SCR_CALL, &s_call_life);
    scr_mgr_register(SCR_INCOMING, &s_incoming_life);
}

esp_err_t phone_app_start(void)
{
    if (s_uac) return ESP_OK;
    if (!wifi_is_connected()) return ESP_ERR_INVALID_STATE;

    if (!s_uac_obj.init(wifi_local_ip(), POC_SIP_LOCAL_PORT, POC_RTP_LOCAL_PORT,
                        POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT, POC_SIP_EXT_SELF,
                        POC_SIP_REG_EXPIRES)) {
        ESP_LOGE(TAG, "UAC init failed (socket bind?)");
        return ESP_FAIL;
    }
    s_uac_obj.setCredentials("", POC_SIP_SECRET);   // only used if the registrar challenges
    s_uac_obj.setDtmfMode(POC_DTMF_RFC2833 ? TincanUac::DtmfMode::Rfc2833
                                           : TincanUac::DtmfMode::Inband);
    s_registered = s_uac_obj.registerExt();
    app_nav_set_sip_registered(s_registered);
    s_uac = &s_uac_obj;

    // Audio pump. Priority above the main loop: starving it is audible,
    // starving the UI is not. Pinned to core 1, away from Wi-Fi/lwIP.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, nullptr, 1);

    ESP_LOGI(TAG, "SIP up (registered=%d) as %s", (int)s_registered, POC_SIP_EXT_SELF);
    dialer_refresh();
    return ESP_OK;
}

bool phone_app_started(void)
{
    return s_uac != nullptr;
}

bool phone_app_registered(void)
{
    return s_registered;
}

void phone_app_dial(const char *number)
{
    if (!number || !number[0]) return;
    if (!s_uac) {
        ESP_LOGW(TAG, "dial %s: no SIP yet", number);
        return;
    }
    if (s_state != UiState::Idle) return;

    ESP_LOGI(TAG, "dialing %s", number);
    s_peer = number;
    s_dir = CALL_DIR_OUT;
    s_ringing_shown = false;
    s_result.clear();
    if (s_uac->placeCallBegin(s_peer)) {
        s_state = UiState::Calling;
        if (scr_mgr_top_id() != SCR_CALL) scr_mgr_push(SCR_CALL);
        else call_refresh();
    } else {
        s_result = "Call failed";
        s_result_until_us = esp_timer_get_time() + (int64_t)RESULT_HOLD_MS * 1000;
        if (scr_mgr_top_id() != SCR_CALL) scr_mgr_push(SCR_CALL);
        else call_refresh();
    }
}

static void show_result_and_schedule_pop(const char *result)
{
    s_result = result;
    s_result_until_us = esp_timer_get_time() + (int64_t)RESULT_HOLD_MS * 1000;
    call_refresh();
}

void phone_app_tick(void)
{
    if (!s_uac) {
        // Wi-Fi came up after boot (or the wizard just finished): start now.
        static int64_t s_last_try_us = 0;
        int64_t now = esp_timer_get_time();
        if (wifi_is_connected() && now - s_last_try_us > 5000000) {
            s_last_try_us = now;
            esp_task_wdt_reset();
            phone_app_start();   // registerExt() blocks up to ~6 s, inside the 10 s WDT
            esp_task_wdt_reset();
        }
        return;
    }

    s_uac->poll();

    // Keep the registration alive; without this the binding lapses at
    // POC_SIP_REG_EXPIRES and the phone silently stops taking calls.
    // Reports failures only; a later successful refresh is not observable
    // through this API, so the status stays "noSIP" until the next boot.
    if (s_uac->maintainRegistration()) {
        if (s_registered) ESP_LOGW(TAG, "re-REGISTER failed");
        s_registered = false;
        app_nav_set_sip_registered(false);
        dialer_refresh();
    }

    const int64_t now = esp_timer_get_time();

    if (s_uac->hasIncomingCall() && s_state != UiState::InCall && s_state != UiState::Incoming) {
        s_state = UiState::Incoming;
        s_peer = s_uac->incomingCallerId();
        s_dir = CALL_DIR_IN;
        s_connect_us = 0;
        ESP_LOGI(TAG, "incoming call from %s", s_peer.c_str());
        audio_hardware_set_amp(true);   // audio_task rings while hasIncomingCall()
        if (scr_mgr_top_id() != SCR_INCOMING) scr_mgr_push(SCR_INCOMING);
        else incoming_refresh();
    }

    switch (s_state) {
    case UiState::Idle:
        if (s_result_until_us && now >= s_result_until_us) {
            s_result_until_us = 0;
            s_result.clear();
            if (scr_mgr_top_id() == SCR_CALL) scr_mgr_pop();
        }
        break;

    case UiState::Calling:
        if (s_uac->inCall()) {
            s_state = UiState::InCall;
            s_connect_us = now;
            audio_hardware_set_amp(true);
            call_refresh();
        } else if (s_uac->callEnded() || !s_uac->dialing()) {
            const int code = s_uac->lastDialCode();
            ESP_LOGW(TAG, "call to %s ended before connect (%d)", s_peer.c_str(), code);
            end_call_bookkeeping(false);
            show_result_and_schedule_pop(code == 487 ? "Cancelled" : code == 486 ? "Busy" :
                                         code == -1  ? "No answer" : "Call failed");
        } else if (s_uac->dialRinging() && !s_ringing_shown) {
            s_ringing_shown = true;
            call_refresh();
        }
        break;

    case UiState::Incoming:
        if (!s_uac->hasIncomingCall()) {
            // Caller gave up (CANCEL) before we answered/rejected.
            end_call_bookkeeping(false);
            if (scr_mgr_top_id() == SCR_INCOMING) scr_mgr_pop();
        }
        break;

    case UiState::InCall:
        if (s_uac->callEnded() || !s_uac->inCall()) {
            end_call_bookkeeping(true);
            show_result_and_schedule_pop("Call ended");
        } else {
            int secs = (int)((now - s_connect_us) / 1000000);
            if (secs != s_shown_secs) call_refresh();   // once a second
        }
        break;
    }
}

void phone_app_power_off(void)
{
    ESP_LOGW(TAG, "powering off");

    if (s_uac && (s_uac->inCall() || s_uac->dialing())) {
        s_uac->hangup();
        vTaskDelay(pdMS_TO_TICKS(150));   // let the BYE/CANCEL actually get out
    }
    audio_hardware_set_amp(false);

    bool onUsb = false;
    if (pmu_sy6970_vbus_present(&onUsb) != ESP_OK) {
        ESP_LOGW(TAG, "could not read VBUS state; assuming battery");
        onUsb = false;
    }

    // The caller has already put a "powering off" screen up and asked for
    // an immediate flush; give the panel time to finish showing it.
    vTaskDelay(pdMS_TO_TICKS(3500));

    // Touch controller into reset before the rails move (vendor sequence).
    xl9555_reset_touch();
    vTaskDelay(pdMS_TO_TICKS(20));

    // Drop every rail xl9555_init() brought up.
    xl9555_write_port0(0x00);
    xl9555_write_port1(0x00);

    if (!onUsb) {
        pmu_sy6970_shutdown();
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGW(TAG, "still running after BATFET_DIS -- sleeping instead");
    } else {
        ESP_LOGW(TAG, "on USB: BATFET_DIS cannot cut VBUS, deep sleeping instead. "
                      "Unplug USB and use the PWR button for a true power off.");
    }

    // BOOT (GPIO0) wakes it from sleep -- active-low physical switch.
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}
