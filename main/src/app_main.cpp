// ─────────────────────────────────────────────────────────────────────────────
//  tdeck-max-phone  —  app_main.cpp
//  SIP Phone Firmware for LilyGO T-Deck MAX, registered as a LAN extension
//  to a drawbridge PBX instance (which owns all 3CX integration).
//
//  Task layout (three tasks, deliberately):
//    main       - SIP control, keypad, UI state machine. Never does slow I/O.
//    audio pump - RTP<->I2S, paced ONLY by the blocking i2s_channel_read().
//    e-paper    - the ~1 s panel refresh, off the control path entirely.
//
//  Audio and display each used to run inline on the main loop, which made a
//  call sound choppy (~20 pkt/s instead of 50, with latency that grew for the
//  whole call) and blacked audio out for seconds on every screen change.
// ─────────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/i2c.h"
#include <atomic>
#include <cctype>
#include <cstring>
#include <cmath>
#include <string>

#include "board_tdeck_max.h"
#include "poc_config.h"
#include "net_wifi.h"
#include "xl9555.h"
#include "pmu_sy6970.h"
#include "es8311_audio.h"
#include "tca8418_keypad.h"
#include "epaper_display.h"
#include "tincan_uac.hpp"

static const char *TAG = "APP_MAIN";

// Keypad-driven call state, layered on top of TincanUac's own SIP state
// (uac.hasIncomingCall()/inCall()/callEnded()). Idle/Dialing/Ringing-out
// are UI-only distinctions -- TincanUac tracks its own Calling/Ringing/
// InCall internally and this just decides what the keypad and e-paper do
// in response.
enum class UiState { Idle, Dialing, Incoming, InCall, ConfirmOff };

// ── boot-time hardware bring-up helpers ─────────────────────────────────────

// Log-and-continue instead of ESP_ERROR_CHECK. On a board with six devices
// sharing one I2C bus, aborting on the first NAK gives a panic-reboot loop
// with no UI and no indication of WHICH device failed -- the least
// debuggable outcome possible. Continuing lets the boot log report every
// failure at once and still reach the I2C scan below. Set
// CONFIG_TDECK_MAX_HALT_ON_INIT_FAIL=y to get fail-fast back.
static bool s_init_failed = false;
static void try_init(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "init OK   : %s", what);
        return;
    }
    s_init_failed = true;
    ESP_LOGE(TAG, "init FAIL : %s -> %s (0x%x)", what, esp_err_to_name(err), err);
#if CONFIG_TDECK_MAX_HALT_ON_INIT_FAIL
    ESP_ERROR_CHECK(err);
#endif
}

static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = BOARD_I2C_SDA;
    conf.scl_io_num = BOARD_I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    esp_err_t err = i2c_param_config(BOARD_I2C_PORT, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(BOARD_I2C_PORT, conf.mode, 0, 0, 0);
}

// Probe every 7-bit address and name the ones this board is supposed to
// have. This is the single most useful line of boot output during bring-up:
// it answers "is the bus even alive, and which chip is missing" before any
// driver-level debugging starts.
static void i2c_bus_scan(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGI(TAG, "[sim] I2C bus scan skipped");
#else
    struct { uint8_t addr; const char *name; } expected[] = {
        {0x18, "ES8311 audio codec"},
        {0x1A, "CST328 touch"},
        {0x20, "XL9555 I/O expander"},
        {0x28, "BHI260AP IMU"},
        {0x34, "TCA8418 keypad"},
        {0x55, "BQ27220 fuel gauge"},
        {0x5A, "DRV2605 haptics"},
        {0x6A, "SY6970 charger"},
        {0x6B, "BQ25896 charger (older rev)"},
    };

    ESP_LOGI(TAG, "--- I2C scan (SDA=%d SCL=%d) ---", BOARD_I2C_SDA, BOARD_I2C_SCL);
    bool seen[128] = {false};
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(30));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) { seen[addr] = true; found++; }
    }

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        ESP_LOGI(TAG, "  0x%02x %-28s %s", expected[i].addr, expected[i].name,
                 seen[expected[i].addr] ? "PRESENT" : "-- absent --");
    }
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (!seen[addr]) continue;
        bool known = false;
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
            if (expected[i].addr == addr) known = true;
        if (!known) ESP_LOGW(TAG, "  0x%02x (unexpected device)", addr);
    }
    ESP_LOGI(TAG, "--- I2C scan: %d device(s) responding ---", found);
#endif
}

// ── e-paper render task ─────────────────────────────────────────────────────
// A refresh is ~1.25 s of panel time (UI_DESIGN 5.7). Doing that inline meant
// every dialled digit and every call-state change stalled SIP and audio for
// the whole refresh. The main loop now just posts the UI model (#38).
//
// Depth-1 mailbox: only the newest model matters, and posting must never
// block the control path even while the panel is mid-refresh. It replaces
// the old xQueueOverwrite queue because coalescing has to MERGE the band as
// well as replace the model: if a transition (B_ALL) is still waiting when a
// one-band edit arrives, the edit must not downgrade it to a partial of a
// screen that was never drawn. Different bands merge to B_ALL.

struct RenderJob {
    ui_model_t model;
    ui_band_t  band;
    uint32_t   seq;     // ui_post() sequence number of this model
};

static portMUX_TYPE      s_render_mux = portMUX_INITIALIZER_UNLOCKED;
static RenderJob         s_render_job;
static bool              s_render_pending = false;
static uint32_t          s_render_absorbed = 0; // posts merged into the pending job
static uint32_t          s_render_posted = 0;   // last seq handed out
static volatile uint32_t s_render_drawn = 0;    // last seq on the glass
static TaskHandle_t      s_epaper_task = nullptr;

static ui_band_t merge_band(ui_band_t a, ui_band_t b)
{
    return (a == b) ? a : B_ALL;
}

// Returns the sequence number of this model, for ui_drawn().
static uint32_t ui_post(const ui_model_t &m, ui_band_t band)
{
    taskENTER_CRITICAL(&s_render_mux);
    if (s_render_pending) {
        band = merge_band(s_render_job.band, band);
        s_render_absorbed++;
    }
    s_render_job.model = m;
    s_render_job.band = band;
    s_render_job.seq = ++s_render_posted;
    s_render_pending = true;
    uint32_t seq = s_render_posted;
    taskEXIT_CRITICAL(&s_render_mux);
    if (s_epaper_task) xTaskNotifyGive(s_epaper_task);
    return seq;
}

// True once the model posted as `seq` (or a newer one) is on the glass.
static bool ui_drawn(uint32_t seq)
{
    return (int32_t)(s_render_drawn - seq) >= 0;
}

// ── UI model ────────────────────────────────────────────────────────────────

static TincanUac *s_uac = nullptr;

static ui_model_t ui_model(ui_screen_t screen, const std::string &number)
{
    ui_model_t m{};
    m.screen = screen;
    std::strncpy(m.number, number.c_str(), sizeof(m.number) - 1);
    std::strncpy(m.self_ext, POC_SIP_EXT_SELF, sizeof(m.self_ext) - 1);
    m.registered = s_uac && s_uac->registered();
    m.wifi_up = wifi_is_connected();
    m.muted = false;                 // no mute yet (#45)
    m.volume = POC_SPK_VOLUME;       // fixed at init; no runtime volume yet (#45)
    return m;
}

// Boot, failure and power-off screens: the band grid with supplied text.
static ui_model_t ui_notice(const char *label, const char *sub,
                            const char *hint1 = "", const char *hint2 = "")
{
    ui_model_t m = ui_model(UI_NOTICE, "");
    std::strncpy(m.notice_label, label, sizeof(m.notice_label) - 1);
    std::strncpy(m.notice_sub, sub, sizeof(m.notice_sub) - 1);
    std::strncpy(m.notice_hint[0], hint1, sizeof(m.notice_hint[0]) - 1);
    std::strncpy(m.notice_hint[1], hint2, sizeof(m.notice_hint[1]) - 1);
    return m;
}

// What audio_task should be doing when no call is up (#36). Set by the main
// loop, read by audio_task every frame; see "ringer and ringback" below.
enum AudioMode : uint8_t { AUDIO_IDLE, AUDIO_RING, AUDIO_RINGBACK, AUDIO_CALL };
static std::atomic<uint8_t> s_audio_mode{AUDIO_IDLE};

static void audio_set_mode(AudioMode m)
{
    s_audio_mode.store(m);
}

// Real power off, the way the board is built to do it.
//
// This mirrors LilyGO's factory ui_shutdown_on(): put the touch controller in
// reset, then force the SY6970's battery FET open (REG09 bit 5, BATFET_DIS) --
// what XPowersLib exposes as PPM.shutdown(). That genuinely disconnects the
// battery rather than idling the CPU, and the PWR button on the top right
// brings it back, because that button is wired to the PMU's power-on pin and
// not to any ESP32 GPIO. Which is why it works as an on/off switch at all.
//
// Deep sleep is the fallback, not the plan: BATFET_DIS gates only the battery
// path, so on USB the board keeps running from VBUS. LilyGO's own UI refuses
// to shut down in that case ("cannot be shut down when connected to USB").
// Rather than refuse, we sleep instead and say so -- on the bench the board is
// nearly always on USB, and "nothing happened" would be the worst answer.
//
// The e-paper holds its image with no power at all, so the final screen stays
// readable while the board is off. The panel keeps telling the truth.
// ── Dial buffer + last-number redial ────────────────────────────────────────

// Characters the dial buffer accepts. Must stay in sync with s_keymap
// (tca8418_keypad.cpp) on the input side and ui_num_has_glyph() (ui_render.cpp)
// on the output side -- a character the keypad can produce but the display
// can't draw looks like a dropped keypress.
static bool is_dial_char(char c)
{
    return (c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+';
}

// A dial buffer longer than this is a mistake, not a number. Further presses
// are ignored rather than beeped (there is no beep path) -- the buffer simply
// stops growing, which is visible on the panel.
#define DIAL_BUFFER_MAX 20

// Last dialled number, persisted so ENT-from-idle survives a reboot. This is
// what replaced the POC_TEST_DIAL bench hack: it keeps the muscle memory that
// ENT from the idle screen places a call, but the target is now something the
// user actually dialled rather than a compile-time constant.
static const char *NVS_NS_PHONE   = "phone";
static const char *NVS_KEY_LASTNO = "lastno";

static std::string load_last_dialled(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PHONE, NVS_READONLY, &h) != ESP_OK) return {};

    char   buf[DIAL_BUFFER_MAX + 1] = {0};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, NVS_KEY_LASTNO, buf, &len);
    nvs_close(h);

    if (err != ESP_OK) return {};
    return std::string(buf);
}

static void save_last_dialled(const std::string &number)
{
    if (number.empty() || number.size() > DIAL_BUFFER_MAX) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PHONE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, NVS_KEY_LASTNO, number.c_str()) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void power_off(TincanUac &uac)
{
    ESP_LOGW(TAG, "powering off");

    // Don't leave the far end talking to a corpse.
    if (uac.inCall()) {
        uac.hangup();
        vTaskDelay(pdMS_TO_TICKS(150));   // let the BYE actually get out
    }
    audio_set_mode(AUDIO_IDLE);
    audio_hardware_set_amp(false);

    bool onUsb = false;
    if (pmu_sy6970_vbus_present(&onUsb) != ESP_OK) {
        ESP_LOGW(TAG, "could not read VBUS state; assuming battery");
        onUsb = false;
    }

    // Posted, not rendered inline: epaper_task owns the SPI bus and driving
    // it from here would race a refresh already in flight. Wait for THIS
    // screen to be on the glass rather than guessing a delay -- it may have to
    // queue behind a refresh that is already running. Bounded, so a dead panel
    // cannot stop the phone from switching off.
    uint32_t seq = ui_post(onUsb ? ui_notice("SLEEPING", "on USB: asleep, not off",
                                             "BOOT button wakes it")
                                 : ui_notice("POWERED OFF", "",
                                             "PWR button turns it on"),
                           B_ALL);
    for (int i = 0; i < 100 && !ui_drawn(seq); i++) vTaskDelay(pdMS_TO_TICKS(50));

    // Touch controller into reset before the rails move. The vendor does this
    // with the comment that it can otherwise come up in an undefined state.
    xl9555_reset_touch();
    vTaskDelay(pdMS_TO_TICKS(20));

    // Drop every rail xl9555_init() brought up -- 4G modem, LoRa, GPS,
    // haptics, speaker amp, 1V8. On the USB path especially, these would
    // otherwise stay powered for the entire "off" period.
    xl9555_write_port0(0x00);
    xl9555_write_port1(0x00);

    if (!onUsb) {
        pmu_sy6970_shutdown();
        vTaskDelay(pdMS_TO_TICKS(500));   // the FET should open well inside this
        // Still here: the write landed but something is still feeding the
        // system rail. Fall through to sleep rather than sit in a dead loop.
        ESP_LOGW(TAG, "still running after BATFET_DIS -- sleeping instead");
    } else {
        ESP_LOGW(TAG, "on USB: BATFET_DIS cannot cut VBUS, deep sleeping instead. "
                      "Unplug USB and use the PWR button for a true power off.");
    }

    // BOOT (GPIO0) wakes it from sleep -- a real physical switch, documented
    // by the vendor for download mode, and active-low. Not the keypad: waking
    // on the TCA8418's INT would be nicer but needs the controller left
    // powered and configured to assert INT while asleep, which is unverified.
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
    // Not reached: deep sleep exits through a reset, so the board comes back
    // through app_main() and re-registers from scratch.
}

// Render coalescing (#41, UI_DESIGN 5.6): after the first post, keep
// absorbing posts until POC_UI_SETTLE_MS passes with none, newest model wins.
// A burst of typing becomes one render instead of one per key -- before this,
// three '7' presses inside 550 ms cost two full refreshes. Capped at
// POC_UI_SETTLE_MAX_MS so a user who never pauses still sees the screen move.
static void epaper_task(void *)
{
    RenderJob job;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const int64_t t_first = esp_timer_get_time();
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POC_UI_SETTLE_MS)) > 0) {
            if ((esp_timer_get_time() - t_first) / 1000 >= POC_UI_SETTLE_MAX_MS) break;
        }

        taskENTER_CRITICAL(&s_render_mux);
        bool have = s_render_pending;
        uint32_t absorbed = s_render_absorbed;
        if (have) {
            job = s_render_job;
            s_render_pending = false;
            s_render_absorbed = 0;
        }
        taskEXIT_CRITICAL(&s_render_mux);
        if (!have) continue;

        if (absorbed) {
            ESP_LOGI(TAG, "render coalesced %lu post(s) into 1 (settled %lld ms)",
                     (unsigned long)absorbed + 1,
                     (long long)((esp_timer_get_time() - t_first) / 1000));
        }
        epaper_render(&job.model, job.band);
        s_render_drawn = job.seq;

        // Stack is sized by measurement, not guesswork: report the
        // high-water mark once the deepest path (compose + refresh) has run.
        static bool s_stack_logged = false;
        if (!s_stack_logged) {
            s_stack_logged = true;
            ESP_LOGI(TAG, "epaper task stack: %u bytes never used of 4096",
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
}

// ── audio pump task ─────────────────────────────────────────────────────────
// Paced solely by the blocking i2s_channel_read() -- one 20 ms frame in, one
// out, 50 frames/sec. Adding any vTaskDelay here (as the old inline version
// effectively did) directly reduces packet rate and grows one-way latency for
// the whole call.

// ── ringer and ringback (#36, UI_DESIGN 6) ──────────────────────────────────
// The phone used to ring silently: audio_task only pumped while inCall(), so
// no code path could make a sound while a call was merely ringing, and the
// main task must not do it (audio_hardware_write_spk() blocks on I2S and
// would stall SIP). The main loop now sets s_audio_mode; audio_task
// synthesises the tone. The main loop also owns the amplifier -- it is an
// XL9555 pin with a shadow register, so exactly one task writes it.
// One 20 ms frame of ring or ringback at the codec's ACTUAL rate. t0 is the
// sample count since the tone started, which drives the cadence; the phase
// accumulators keep the waveform continuous across frames.
//   RING:     1000/1250 Hz warble at 20 Hz -- the classic electronic ringer --
//             in a double-ring cadence, 0.4 on, 0.2 off, 0.4 on, 2.0 off.
//   RINGBACK: North American 440 + 480 Hz, 2 s on, 4 s off.
static void synth_tone_frame(int16_t *out, uint8_t mode, uint32_t t0, uint32_t sr,
                             float *ph1, float *ph2)
{
    const float twopi = 6.28318531f;
    for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
        const uint32_t ms = (uint32_t)(((uint64_t)(t0 + i) * 1000u) / sr);
        float v = 0.0f;
        if (mode == AUDIO_RING) {
            const uint32_t c = ms % 3000u;
            const bool on = c < 400u || (c >= 600u && c < 1000u);
            const float f = ((ms / 25u) & 1u) ? 1250.0f : 1000.0f;
            *ph1 += twopi * f / (float)sr;
            if (*ph1 > twopi) *ph1 -= twopi;
            if (on) v = POC_RING_AMPL * sinf(*ph1);
        } else {
            const bool on = (ms % 6000u) < 2000u;
            *ph1 += twopi * 440.0f / (float)sr;
            *ph2 += twopi * 480.0f / (float)sr;
            if (*ph1 > twopi) *ph1 -= twopi;
            if (*ph2 > twopi) *ph2 -= twopi;
            if (on) v = POC_RINGBACK_AMPL * 0.5f * (sinf(*ph1) + sinf(*ph2));
        }
        out[i] = (int16_t)v;
    }
}

static void audio_task(void *)
{
    int16_t pcm[POC_FRAME_SAMPLES];
    int16_t micbuf[POC_FRAME_SAMPLES];
    static const int16_t silence[POC_FRAME_SAMPLES] = {0};
    bool pumping = false;

    // Q8 fixed point so the per-sample work is a multiply and a shift.
    const int32_t rxGainQ8 = (int32_t)(powf(10.0f, POC_RX_GAIN_DB / 20.0f) * 256.0f);
    const int32_t duckQ8   = (POC_DUCK_DB == 0.0f)
                             ? 256
                             : (int32_t)(powf(10.0f, POC_DUCK_DB / 20.0f) * 256.0f);
    const int hangoverFrames = POC_DUCK_HANGOVER_MS / 20;   // frames are 20 ms
    int duckFrames = 0;
    int statFrames = 0;
    // Mean |sample| accumulators for the census. Packet counts alone can't
    // tell "RTP is flowing" from "RTP is flowing and every frame is silence",
    // which is the difference between a codec fault here and a media fault
    // upstream. 64-bit because 250 frames * 160 samples * 32767 overflows 32.
    int64_t statMicSum = 0, statRxSum = 0;
    int32_t statMicN = 0, statRxN = 0;
    int16_t statMicPeak = 0, statRxPeak = 0;

    // Synthesise at the rate the codec was really opened at, not
    // POC_SAMPLE_RATE_HZ: a tone built for one rate and played at another
    // skews pitch and duration (the trap #36 warns about).
    const uint32_t toneRate = audio_hardware_sample_rate();
    uint8_t toneMode = AUDIO_IDLE;
    uint32_t toneT = 0;
    float tonePh1 = 0.0f, tonePh2 = 0.0f;

    for (;;) {
        if (!s_uac || !s_uac->inCall()) {
            pumping = false;
            duckFrames = 0;

            const uint8_t mode = s_audio_mode.load();
            if ((mode == AUDIO_RING || mode == AUDIO_RINGBACK) && toneRate > 0) {
                if (mode != toneMode) {
                    toneMode = mode;
                    toneT = 0;
                    tonePh1 = tonePh2 = 0.0f;
                    ESP_LOGI(TAG, "%s tone on @ %lu Hz",
                             mode == AUDIO_RING ? "ring" : "ringback", (unsigned long)toneRate);
                }
                synth_tone_frame(pcm, mode, toneT, toneRate, &tonePh1, &tonePh2);
                toneT += POC_FRAME_SAMPLES;
                // The blocking write paces this loop, exactly as the call path
                // is paced by the blocking read. If it fails (or there is no
                // codec, as under QEMU) fall back to a delay instead of spinning.
                size_t wrote = audio_hardware_write_spk(pcm, POC_FRAME_SAMPLES);
#if CONFIG_TDECK_MAX_SIM_MODE
                wrote = 0;
#endif
                if (wrote == 0) vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (toneMode != AUDIO_IDLE) {
                ESP_LOGI(TAG, "tone off after %lu ms",
                         (unsigned long)((uint64_t)toneT * 1000u / toneRate));
                toneMode = AUDIO_IDLE;
            }
            // Not in a call: nothing to pace against, so idle politely.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        toneMode = AUDIO_IDLE;   // answered mid-ring: the call owns the speaker now

        if (!pumping) {
            // Entering a call -- drop anything the far end queued before we
            // started pumping, so we don't inherit that as permanent latency.
            s_uac->flushRtpBacklog();
            pumping = true;
            statFrames = 0;
        }

        // Periodic RTP census. "No audio" has three causes that are
        // indistinguishable from the outside -- we never sent, they never
        // sent, or both flowed and the codec path is at fault -- and this is
        // the cheapest way to tell them apart without a packet capture.
        // 250 frames * 20 ms = 5 s.
        if (++statFrames >= 250) {
            statFrames = 0;
            // mic~/rx~ are mean |sample| out of 32767. Anything under ~30 is
            // effectively digital silence; speech sits in the hundreds.
            ESP_LOGI(TAG, "rtp census: tx=%lu rx=%lu | mic~%ld peak %d | rx~%ld peak %d%s",
                     (unsigned long)s_uac->rtpTx(), (unsigned long)s_uac->rtpRx(),
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

        // Blocking read IS the clock for this loop.
        size_t got = audio_hardware_read_mic(micbuf, POC_FRAME_SAMPLES);
        if (got > 0) {
            // Duck the mic if the far end was talking on the previous frame.
            // Reading the mic before writing the speaker is what makes this
            // the right frame to attenuate: it was captured while their audio
            // was coming out of the speaker.
            if (duckFrames > 0) {
                for (size_t i = 0; i < got; i++) {
                    micbuf[i] = (int16_t)((micbuf[i] * duckQ8) >> 8);
                }
                duckFrames--;
            }
            // Census: measure what we are ACTUALLY transmitting, i.e. after
            // ducking, since that is what the far end receives.
            for (size_t i = 0; i < got; i++) {
                int16_t a = micbuf[i] < 0 ? (int16_t)-micbuf[i] : micbuf[i];
                statMicSum += a;
                if (a > statMicPeak) statMicPeak = a;
            }
            statMicN += (int32_t)got;
            s_uac->sendAudioFrame(micbuf, got);
        }

        size_t rx = s_uac->recvAudioFrame(pcm, POC_FRAME_SAMPLES);
        if (rx > 0) {
            // Decide "is the far end talking" on the RAW decoded frame, before
            // the gain below. Measuring post-gain would mean the threshold
            // silently retunes itself every time POC_RX_GAIN_DB changes.
            int32_t sumAbs = 0;
            int16_t framePeak = 0;
            for (size_t i = 0; i < rx; i++) {
                int16_t a = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
                sumAbs += a;
                if (a > framePeak) framePeak = a;
            }
            // Census on the RAW decoded frame, before rx gain -- this is what
            // actually arrived from the bridge.
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
            audio_hardware_write_spk(pcm, rx);
        } else {
            // Feed silence so the I2S DMA doesn't replay its last buffer.
            // NOTE: no jitter buffer, no sequence-number reordering and no
            // packet-loss concealment -- one datagram in, one frame out. On
            // a clean LAN this is fine; it will audibly suffer on a lossy
            // or bursty link. Out of scope for the PoC.
            audio_hardware_write_spk(silence, POC_FRAME_SAMPLES);
        }
    }
}

#if CONFIG_TDECK_MAX_EPD_BENCH
// UI half of the e-paper bench: drives the real render path (mailbox, settle
// window, band diff, ghost budget) with scripted posts, because nobody can
// press keys on a board that is only reachable over serial. Every decision is
// in the log as "render #N ... -> FULL/PARTIAL/SKIP (why)".
static void ui_wait(uint32_t seq)
{
    for (int i = 0; i < 200 && !ui_drawn(seq); i++) vTaskDelay(pdMS_TO_TICKS(25));
}

static void ui_bench(void)
{
    ESP_LOGW(TAG, "=== UI RENDER BENCH ===");
    ui_wait(ui_post(ui_model(UI_IDLE, "777"), B_ALL));

    // A transition, then a fast burst: four edits 120 ms apart must coalesce
    // into ONE partial of B_NUMBER (#41 + #42).
    ui_wait(ui_post(ui_model(UI_DIALLING, "9"), B_ALL));
    ESP_LOGW(TAG, "ui bench: burst of 4 edits, 120 ms apart -> expect 1 PARTIAL");
    uint32_t seq = 0;
    for (const char *n : {"9*", "9*7", "9*77", "9*777"}) {
        seq = ui_post(ui_model(UI_DIALLING, n), B_NUMBER);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ui_wait(seq);

    // Slow typing up to 13 digits: every edit is its own partial until the
    // ghost cap promotes one to a full refresh (#43). That is also 8
    // consecutive partials on the glass -- what the ghosting threshold has
    // to be judged on by eye.
    ESP_LOGW(TAG, "ui bench: 8 slow edits -> expect partials until the ghost cap, then FULL");
    std::string num = "9*777";
    for (int i = 1; i <= 8; i++) {
        num += (char)('0' + i % 10);
        ui_wait(ui_post(ui_model(UI_DIALLING, num), B_NUMBER));
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    // 13 -> 14 digits changes B_SUB too ("14 DIGITS - SHOWING LAST 13").
    ESP_LOGW(TAG, "ui bench: 14th digit -> B_NUMBER request, expect FULL (spans bands)");
    num += '9';
    ui_wait(ui_post(ui_model(UI_DIALLING, num), B_NUMBER));

    // A band request whose frame changes two bands must be promoted to FULL.
    ESP_LOGW(TAG, "ui bench: B_NUMBER request that also changes the label -> expect FULL");
    ui_wait(ui_post(ui_model(UI_CALLING, num), B_NUMBER));

    // Nothing changed: must not touch the panel at all.
    ESP_LOGW(TAG, "ui bench: identical frame -> expect SKIP");
    ui_wait(ui_post(ui_model(UI_CALLING, num), B_ALL));

    // Gallery of the UI_DESIGN 4 screens, 3 s each, for whoever is watching.
    ESP_LOGW(TAG, "ui bench: screen gallery");
    ui_model_t g = ui_model(UI_CALLING, "777");
    ui_wait(ui_post(g, B_ALL));
    vTaskDelay(pdMS_TO_TICKS(3000));
    g = ui_model(UI_INCOMING, "1001");
    ui_wait(ui_post(g, B_ALL));
    vTaskDelay(pdMS_TO_TICKS(3000));
    g = ui_model(UI_INCALL, "1001");
    ui_wait(ui_post(g, B_ALL));
    vTaskDelay(pdMS_TO_TICKS(3000));
    g = ui_model(UI_ENDED, "777");
    g.last_call_secs = 151;
    ui_wait(ui_post(g, B_ALL));
    vTaskDelay(pdMS_TO_TICKS(3000));
    g.last_call_failed = true;
    ui_wait(ui_post(g, B_ALL));
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGW(TAG, "=== UI RENDER BENCH COMPLETE ===");
}
#endif // CONFIG_TDECK_MAX_EPD_BENCH

#if CONFIG_TDECK_MAX_AUDIO_SELFTEST
// Speaker and microphone failures are indistinguishable during a call --
// both yield silence. This separates them: an audible tone proves the
// DAC/I2S/amp path, and a non-zero mic level proves the ADC path without
// anyone having to hear anything.
// One pass of "play a tone, then listen". Returns the mic peak; logs the
// speaker result. `label` identifies which pin orientation is under test.
static int32_t audio_pass(const char *label)
{
    // Derive everything from the rate the codec was ACTUALLY opened at.
    // Generating a tone for 8 kHz and playing it at 16 kHz halves its
    // duration and doubles its pitch, which is what made a "2 second" tone
    // finish in 428 ms and sent us chasing the wrong fault.
    const uint32_t sr = audio_hardware_sample_rate();
    const int frames = (int)((sr * 2) / POC_FRAME_SAMPLES);

    audio_hardware_set_amp(true);
    vTaskDelay(pdMS_TO_TICKS(50));   // let the amp settle before driving it
    ESP_LOGW(TAG, "[%s] playing 1 kHz tone for 2 s @ %lu Hz -- LISTEN NOW", label, sr);

    int16_t tone[POC_FRAME_SAMPLES];
    const float step = 2.0f * 3.14159265f * 1000.0f / (float)sr;
    float phase = 0.0f;
    size_t written_total = 0;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
            // Amplitude is POC_SELFTEST_TONE_AMPL -- see poc_config.h for why
            // it is a knob and not a literal. It has been turned down twice.
            tone[i] = (int16_t)(POC_SELFTEST_TONE_AMPL * sinf(phase));
            phase += step;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        written_total += audio_hardware_write_spk(tone, POC_FRAME_SAMPLES);
    }
    int elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, "[%s] tone done -- %u samples accepted (expected %d), took %d ms "
                  "(expected ~2000)", label,
             (unsigned)written_total, frames * POC_FRAME_SAMPLES, elapsed_ms);
    if (written_total == 0) {
        ESP_LOGE(TAG, "[%s] I2S accepted ZERO samples -- TX path is not running", label);
    } else if (elapsed_ms < 1500 || elapsed_ms > 2600) {
        ESP_LOGE(TAG, "[%s] TX ran at the wrong rate -- sample-rate/channel mismatch", label);
    }

    // Amp off before capturing. Speaker and mic are centimetres apart with no
    // isolation, so leaving the amp powered lets speaker output couple back
    // into the mic and inflate the level we are trying to measure -- which is
    // exactly what it looked like the first time the DAC ceiling was raised.
    // The capture measures the mic, so the speaker has no business being live.
    audio_hardware_set_amp(false);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGW(TAG, "[%s] recording 2 s -- SPEAK NOW / make noise", label);
    int16_t mic[POC_FRAME_SAMPLES];
    int32_t peak = 0;
    int64_t sumsq = 0;
    size_t total = 0, zeroFrames = 0;
    for (int f = 0; f < frames; f++) {
        size_t got = audio_hardware_read_mic(mic, POC_FRAME_SAMPLES);
        if (got == 0) { zeroFrames++; continue; }
        bool allZero = true;
        for (size_t i = 0; i < got; i++) {
            int32_t v = mic[i];
            if (v != 0) allZero = false;
            int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sumsq += (int64_t)v * v;
        }
        if (allZero) zeroFrames++;
        total += got;
    }
    int rms = (total > 0) ? (int)sqrt((double)sumsq / (double)total) : 0;
    ESP_LOGW(TAG, "[%s] mic: %u samples, peak=%d rms=%d, silent/empty frames=%u/%d",
             label, (unsigned)total, (int)peak, rms, (unsigned)zeroFrames, frames);

    if (total == 0) {
        ESP_LOGE(TAG, "[%s] mic returned NO DATA -- I2S RX not running", label);
    } else if (peak == 0) {
        ESP_LOGE(TAG, "[%s] mic is bit-exact ZERO", label);
    } else if (peak < 50) {
        ESP_LOGW(TAG, "[%s] mic level very low (peak=%d) -- check PGA gain", label, (int)peak);
    } else {
        ESP_LOGW(TAG, "[%s] mic is LIVE (peak=%d) -- ADC path good", label, (int)peak);
    }

    audio_hardware_set_amp(false);
    return peak;
}

// Ringer check (#36): run the real tone generator for a fixed span and time
// it. The I2S write is what paces the ring loop in audio_task, so if the
// synthesis rate and the codec rate disagree this is where it shows -- the
// same trap that once made a "2 second" self-test tone finish in 428 ms.
static void ring_pass(uint8_t mode, int ms)
{
    const uint32_t sr = audio_hardware_sample_rate();
    const int frames = (int)((sr * (uint32_t)ms / 1000u) / POC_FRAME_SAMPLES);
    int16_t pcm[POC_FRAME_SAMPLES];
    float ph1 = 0.0f, ph2 = 0.0f;
    int16_t peak = 0;
    size_t written = 0;

    audio_hardware_set_amp(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < frames; f++) {
        synth_tone_frame(pcm, mode, (uint32_t)(f * POC_FRAME_SAMPLES), sr, &ph1, &ph2);
        for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
            int16_t a = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
            if (a > peak) peak = a;
        }
        written += audio_hardware_write_spk(pcm, POC_FRAME_SAMPLES);
    }
    int elapsed = (int)((esp_timer_get_time() - t0) / 1000);
    audio_hardware_set_amp(false);
    ESP_LOGW(TAG, "[%s] %u samples @ %lu Hz, peak %d, took %d ms (expected ~%d)",
             mode == AUDIO_RING ? "ring" : "ringback", (unsigned)written, (unsigned long)sr,
             (int)peak, elapsed, ms);
    if (elapsed < ms * 3 / 4 || elapsed > ms * 5 / 4) {
        ESP_LOGE(TAG, "ring tone ran at the wrong rate -- check audio_hardware_sample_rate()");
    }
}

static void audio_selftest(void)
{
    ESP_LOGW(TAG, "=== AUDIO SELF-TEST ===");

    // Prove the codec is actually configured before blaming the wiring:
    // "never configured", "configured but muted" and "configured, wired
    // wrong" all sound identical from the speaker.
    int reg_fails = audio_hardware_check_codec_regs();
    audio_hardware_probe_asdout_activity();
    audio_hardware_probe_pin_drive();

    if (reg_fails > 0) {
        ESP_LOGE(TAG, "%d codec register(s) wrong -- fix those before trusting "
                      "anything below", reg_fails);
    }

    audio_pass("audio");
    ring_pass(AUDIO_RING, 3000);       // one full double-ring cadence
    ring_pass(AUDIO_RINGBACK, 2000);   // the "on" part of one ringback cycle

    ESP_LOGW(TAG, "=== SELF-TEST COMPLETE ===");
}
#endif // CONFIG_TDECK_MAX_AUDIO_SELFTEST

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  LilyGO T-Deck MAX SIP Phone (via drawbridge PBX) ");
    ESP_LOGI(TAG, "==================================================");

    try_init("nvs_flash", nvs_flash_init());

    // 1. Shared I2C bus, then scan it before trusting any device driver.
    try_init("i2c bus", i2c_master_init());
    i2c_bus_scan();

    // 2. Peripherals. Every one is log-and-continue so a single NAK doesn't
    //    hide the state of the other five.
    try_init("XL9555 expander", xl9555_init());
    // 8 kHz: G.711 is the only codec drawbridge will carry, and nothing in
    // this firmware resamples. The bring-up ran at 16 kHz for a while to rule
    // out the unusually low 2.048 MHz MCLK that 8 k implies; that turned out
    // not to be the problem (the data pins were swapped), so we are back on
    // the rate the SIP path actually uses.
    try_init("ES8311 codec", audio_hardware_init(POC_SAMPLE_RATE_HZ));

    // xl9555_init() brings the speaker amp up enabled. Nothing should be
    // playing at boot, and an idle-but-powered amp both draws current and
    // can hiss/click on DMA underrun -- the sibling tincan project turns
    // it off for exactly this reason. It's re-enabled per call.
    audio_hardware_set_amp(false);

    // xl9555_init() also asserts P1_0, which powers the A7682E 4G modem.
    // This firmware never uses the modem (Wi-Fi only), so that is pure
    // battery drain -- left as-is deliberately rather than changed blind,
    // since the power-sequencing side effects on real hardware are
    // unverified. See docs/HARDWARE_CAVEATS.md.
    try_init("TCA8418 keypad", tca8418_init());
    try_init("GDEQ031T10 e-paper", epaper_display_init());

    if (s_init_failed) {
        ESP_LOGW(TAG, "*** one or more peripherals failed to init (see above) ***");
        ESP_LOGW(TAG, "*** continuing anyway; check the I2C scan for absent devices ***");
    }

#if CONFIG_TDECK_MAX_AUDIO_SELFTEST
    audio_selftest();   // before Wi-Fi, so nothing else competes for the bus
#endif
#if CONFIG_TDECK_MAX_EPD_BENCH
    epaper_bench_run(); // before the render task exists: it owns SPI here
#endif

    // 3. Display task first, so failures after this point can be shown.
    xTaskCreate(epaper_task, "epaper", 4096, nullptr, 3, &s_epaper_task);
#if CONFIG_TDECK_MAX_EPD_BENCH
    ui_bench();
#endif
    ui_post(ui_notice("BOOTING", "joining Wi-Fi"), B_ALL);

    // 4. Wi-Fi (bounded; won't hang forever on a bad SSID).
    esp_err_t wifi_err = wifi_sta_connect(POC_WIFI_SSID, POC_WIFI_PASS);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi did not come up; phone cannot register");
        ui_post(ui_notice("NO WIFI", "no IP address after 30 s",
                          "check the access point,", "then power-cycle"), B_ALL);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wi-Fi up, local IP %s", wifi_local_ip());

    // 5. SIP UAC, registered as a LAN extension.
    static TincanUac uac;
    if (!uac.init(wifi_local_ip(), POC_SIP_LOCAL_PORT, POC_RTP_LOCAL_PORT,
                  POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT, POC_SIP_EXT_SELF)) {
        ESP_LOGE(TAG, "UAC init failed (socket bind?)");
        ui_post(ui_notice("SIP FAILED", "could not open SIP socket",
                          "power-cycle to retry"), B_ALL);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bool registered = uac.registerExt();
    s_uac = &uac;   // before the next ui_model(): it reads uac.registered()

    // 6. Audio pump. Priority above the main loop: starving it is audible,
    //    starving the UI is not. Pinned to core 1 to keep it away from the
    //    Wi-Fi/lwIP stack on core 0.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, nullptr, 1);

    UiState ui = UiState::Idle;
    std::string dialBuffer;
    std::string lastDialled = load_last_dialled();

    // ENDED is the IDLE screen with the result of the call that just ended
    // (UI_DESIGN 2.4): no timer, it stays until a key moves the phone on.
    // Its B_NUMBER is always what ENT will dial. After a FAILED call that is
    // the number that failed (3.3: "the single most likely next action"),
    // held here only while the ENDED screen is up -- it is never written to
    // NVS, so a typo is still never persisted as the redial target.
    bool showEnded = false;
    bool lastCallFailed = false;
    uint32_t lastCallSecs = 0;          // last CONNECTED call; IDLE shows it as LAST CALL
    std::string failedTarget;
    int64_t callStartUs = 0;

    auto redialTarget = [&]() -> const std::string & {
        return (showEnded && lastCallFailed && !failedTarget.empty()) ? failedTarget
                                                                      : lastDialled;
    };
    auto idleModel = [&]() {
        ui_model_t m = ui_model(showEnded ? UI_ENDED : UI_IDLE, redialTarget());
        m.last_call_secs = lastCallSecs;
        m.last_call_failed = showEnded && lastCallFailed;
        return m;
    };
    auto leaveEnded = [&]() {
        showEnded = false;
        failedTarget.clear();
    };

    // Input grace window (#40, UI_DESIGN 2.5). On a transition that changes
    // what ENT and DEL mean, keys are ignored until the new screen is
    // actually on the glass (or 3 s, whichever is first), then anything
    // pressed blind is drained rather than replayed. Without it, a user
    // mid-dial with a finger on ENT answers a call they cannot see yet, and
    // a double-tapped ENT answers and then hangs up.
    int64_t ringBlinkUs = 0;
    bool ringBlinkOn = true;

    uint32_t graceSeq = 0;
    int64_t graceStartUs = 0;
    int graceSwallowed = 0;
    auto startGrace = [&](uint32_t seq) {
        graceSeq = seq;
        graceStartUs = esp_timer_get_time();
        graceSwallowed = 0;
    };

    ui_post(idleModel(), B_ALL);
    bool shownRegistered = uac.registered(), shownWifi = wifi_is_connected();
    ESP_LOGI(TAG, "System operational (registered=%d).", registered);
    if (!lastDialled.empty()) {
        ESP_LOGI(TAG, "ENT from idle will redial %s", lastDialled.c_str());
    }

    // Places a call and drives the UI through it. Shared by redial-from-idle
    // and dial-from-buffer so the two can't drift apart -- they had already
    // grown slightly different logging and teardown before this was factored.
    auto placeCallTo = [&](const std::string &target) {
        leaveEnded();
        ui_post(ui_model(UI_CALLING, target), B_ALL);
        ESP_LOGI(TAG, "dialing %s", target.c_str());

        // Ringback for the whole wait, so an outgoing call is not silent
        // either. placeCall() blocks this task; audio_task plays it.
        audio_hardware_set_amp(true);
        audio_set_mode(AUDIO_RINGBACK);
        bool connected = uac.placeCall(target);
        audio_set_mode(connected ? AUDIO_CALL : AUDIO_IDLE);

        // placeCall() blocks the loop for as long as it rings (#18), so the
        // keypad was never read meanwhile: whatever a frustrated user mashed
        // during the wait is in the FIFO. Drop it rather than replay it into
        // the call that just connected (UI_DESIGN 2.3, "cheap and required").
        int dropped = tca8418_flush();
        if (dropped) ESP_LOGI(TAG, "dropped %d key event(s) pressed while calling", dropped);

        if (connected) {
            // Only remember numbers that actually connected. Persisting a
            // failed attempt would make redial replay a typo.
            if (target != lastDialled) {
                lastDialled = target;
                save_last_dialled(target);
            }
            ui = UiState::InCall;
            audio_hardware_set_amp(true);
            callStartUs = esp_timer_get_time();
            startGrace(ui_post(ui_model(UI_INCALL, target), B_ALL));
        } else {
            ui = UiState::Idle;
            audio_hardware_set_amp(false);
            showEnded = true;
            lastCallFailed = true;
            failedTarget = target;
            ui_post(idleModel(), B_ALL);
            ESP_LOGW(TAG, "call to %s failed", target.c_str());
        }
    };

    for (;;) {
        uac.poll();

        // Keep the registration alive; without this the binding lapses at
        // POC_SIP_REG_EXPIRES and the phone silently stops taking calls.
        uac.maintainRegistration();

        // Link state is drawn in B_STATUS (UI_DESIGN 4.6). A change while
        // idle repaints that band alone (5.2); in any other state the next
        // transition picks it up. If the change also moves another band (the
        // IDLE label says NO SERVICE when unregistered), the driver sees the
        // frame differ in two bands and takes a full refresh instead.
        const bool regNow = uac.registered(), wifiNow = wifi_is_connected();
        if (regNow != shownRegistered || wifiNow != shownWifi) {
            shownRegistered = regNow;
            shownWifi = wifiNow;
            ESP_LOGI(TAG, "link state: %s, %s", regNow ? "registered" : "NOT registered",
                     wifiNow ? "Wi-Fi up" : "Wi-Fi DOWN");
            if (ui == UiState::Idle) ui_post(idleModel(), B_STATUS);
        }

        // Single GPIO read unless the controller actually has events.
        char key = tca8418_key_pending() ? tca8418_get_key() : 0;

        if (graceStartUs) {
            const int64_t heldMs = (esp_timer_get_time() - graceStartUs) / 1000;
            if (ui_drawn(graceSeq) || heldMs >= 3000) {
                graceSwallowed += tca8418_flush();
                ESP_LOGI(TAG, "input grace over after %lld ms (%s), %d key event(s) discarded",
                         (long long)heldMs, ui_drawn(graceSeq) ? "screen visible" : "timeout",
                         graceSwallowed);
                graceStartUs = 0;
            } else if (key) {
                graceSwallowed++;
            }
            key = 0;
        }

        if (uac.hasIncomingCall() && ui != UiState::InCall && ui != UiState::Incoming) {
            ui = UiState::Incoming;
            leaveEnded();
            ESP_LOGI(TAG, "incoming call from %s", uac.incomingCallerId().c_str());
            audio_hardware_set_amp(true);
            audio_set_mode(AUDIO_RING);
            ringBlinkUs = esp_timer_get_time();
            startGrace(ui_post(ui_model(UI_INCOMING, uac.incomingCallerId()), B_ALL));
        }

        switch (ui) {
        case UiState::Idle:
            // DEL from Idle asks to power down. Two single taps rather than a
            // long press: long-press needs both key edges, and
            // tca8418_get_key() discards the release edge, so a hold is not
            // representable against today's API (UI_DESIGN 9.2). A confirm
            // step also stops a stray DEL from switching the phone off
            // mid-shift.
            if (key == '\b') {
                leaveEnded();
                ui = UiState::ConfirmOff;
                ui_post(ui_notice("POWER OFF?", "",
                                  "ENT  power off",
                                  "any other key cancels"), B_ALL);
                break;
            }
            // ENT on an empty buffer redials the number on the screen: the
            // last number that connected, or on the CALL FAILED screen the
            // one that just failed. Inert when there is nothing -- a phone
            // that dials something unpredictable on an idle keypress is worse
            // than one that does nothing.
            if (key == '\r') {
                const std::string target = redialTarget();   // copy: placeCallTo clears it
                if (!target.empty()) placeCallTo(target);
                else ESP_LOGI(TAG, "ENT from idle: nothing to redial yet");
                break;
            }
            if (is_dial_char(key)) {
                leaveEnded();
                dialBuffer.clear();
                dialBuffer += key;
                ui = UiState::Dialing;
                ui_post(ui_model(UI_DIALLING, dialBuffer), B_ALL);
            }
            break;

        case UiState::Dialing:
            if (key == '\b') {
                if (!dialBuffer.empty()) dialBuffer.pop_back();
                else ui = UiState::Idle;
                if (ui == UiState::Idle) ui_post(idleModel(), B_ALL);
                else ui_post(ui_model(UI_DIALLING, dialBuffer), B_NUMBER);
            } else if (is_dial_char(key)) {
                // Silently stop growing at the cap rather than wrapping or
                // truncating on dial -- the user can see the number isn't
                // getting longer.
                if (dialBuffer.size() < DIAL_BUFFER_MAX) {
                    dialBuffer += key;
                    ui_post(ui_model(UI_DIALLING, dialBuffer), B_NUMBER);
                }
            } else if (key == '\r' && !dialBuffer.empty()) {
                std::string target = dialBuffer;
                dialBuffer.clear();
                placeCallTo(target);
            }
            break;

        case UiState::Incoming:
            // Keyboard backlight blinks at 2 Hz while ringing (UI_DESIGN 6):
            // the one alert channel that costs no panel time.
            if (esp_timer_get_time() - ringBlinkUs >= 250000) {
                ringBlinkUs = esp_timer_get_time();
                ringBlinkOn = !ringBlinkOn;
                tca8418_set_backlight(ringBlinkOn);
            }
            if (key == '\r' || key == '\b' || !uac.hasIncomingCall()) {
                tca8418_set_backlight(true);
                ringBlinkOn = true;
            }
            if (key == '\r') {
                uac.answer();
                ui = UiState::InCall;
                audio_set_mode(AUDIO_CALL);
                audio_hardware_set_amp(true);
                callStartUs = esp_timer_get_time();
                startGrace(ui_post(ui_model(UI_INCALL, uac.incomingCallerId()), B_ALL));
            } else if (key == '\b') {
                uac.reject();
                ui = UiState::Idle;
                audio_set_mode(AUDIO_IDLE);
                audio_hardware_set_amp(false);
                ui_post(idleModel(), B_ALL);
            } else if (!uac.hasIncomingCall()) {
                // Caller gave up (CANCEL) before we answered/rejected.
                ui = UiState::Idle;
                audio_set_mode(AUDIO_IDLE);
                audio_hardware_set_amp(false);
                ui_post(idleModel(), B_ALL);
            }
            break;

        case UiState::ConfirmOff:
            if (key == '\r') {
                power_off(uac);             // does not return
            } else if (key != 0) {
                // Any other key cancels, not just DEL -- if you are unsure
                // enough to press something random, you did not mean to shut
                // the phone down.
                ui = UiState::Idle;
                ui_post(idleModel(), B_ALL);
            } else if (uac.hasIncomingCall()) {
                // An incoming call outranks a pending power-off prompt; the
                // hasIncomingCall() check above the switch has already moved
                // us on, this just avoids leaving the prompt on screen.
                ui_post(ui_model(UI_INCOMING, uac.incomingCallerId()), B_ALL);
            }
            break;

        case UiState::InCall:
            // Media is handled entirely by audio_task; nothing to do here
            // but watch for hangup from either end.
            if (key == '\b' || key == '\r') uac.hangup();
            if (uac.callEnded() || !uac.inCall()) {
                ui = UiState::Idle;
                audio_set_mode(AUDIO_IDLE);
                audio_hardware_set_amp(false);
                // Duration is shown once, on the ENDED screen -- a full
                // refresh that was happening anyway -- never as a live timer
                // (UI_DESIGN 5.5). Rounded up so a short call never reads 00:00.
                lastCallSecs = (uint32_t)((esp_timer_get_time() - callStartUs + 999999) / 1000000);
                lastCallFailed = false;
                failedTarget.clear();
                showEnded = true;
                ESP_LOGI(TAG, "call ended after %lu s", (unsigned long)lastCallSecs);
                ui_post(idleModel(), B_ALL);
            }
            break;
        }

        // Control-loop cadence only -- audio is no longer paced by this.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
