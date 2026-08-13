// ─────────────────────────────────────────────────────────────────────────────
//  tdeck-max-phone  —  app_main.cpp
//  SIP Phone Firmware for LilyGO T-Deck MAX, registered as a LAN extension
//  to a drawbridge PBX instance (which owns all 3CX integration).
//
//  Task layout (three tasks, deliberately):
//    main       - SIP control, keypad, UI state machine. Never does slow I/O.
//    audio pump - RTP<->I2S, paced ONLY by the blocking i2s_channel_read().
//    e-paper    - the 2-3 s panel refresh, off the control path entirely.
//
//  Audio and display each used to run inline on the main loop, which made a
//  call sound choppy (~20 pkt/s instead of 50, with latency that grew for the
//  whole call) and blacked audio out for seconds on every screen change.
// ─────────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <cctype>
#include <cstring>
#include <cmath>
#include <string>

#include "board_tdeck_max.h"
#include "poc_config.h"
#include "net_wifi.h"
#include "xl9555.h"
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
enum class UiState { Idle, Dialing, Incoming, InCall };

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
// A full-screen refresh on this panel is ~2-3 s of panel time plus SPI. Doing
// that inline meant every dialled digit and every call-state change stalled
// SIP and audio for seconds. The main loop now just posts a request.

struct RenderRequest {
    char caller[24];
    char status[24];
    bool active;
};

static QueueHandle_t s_render_q = nullptr;

static void ui_render(const char *caller, const char *status, bool active)
{
    if (!s_render_q) return;
    RenderRequest r{};
    std::strncpy(r.caller, caller ? caller : "", sizeof(r.caller) - 1);
    std::strncpy(r.status, status ? status : "", sizeof(r.status) - 1);
    r.active = active;
    // Depth-1 queue with overwrite: only the newest screen matters, and this
    // must never block the control path even if the panel is mid-refresh.
    xQueueOverwrite(s_render_q, &r);
}

static void epaper_task(void *)
{
    RenderRequest r;
    for (;;) {
        if (xQueueReceive(s_render_q, &r, portMAX_DELAY) == pdTRUE) {
            epaper_render_call_status(r.caller, r.status, r.active);
        }
    }
}

// ── audio pump task ─────────────────────────────────────────────────────────
// Paced solely by the blocking i2s_channel_read() -- one 20 ms frame in, one
// out, 50 frames/sec. Adding any vTaskDelay here (as the old inline version
// effectively did) directly reduces packet rate and grows one-way latency for
// the whole call.

static TincanUac *s_uac = nullptr;

static void audio_task(void *)
{
    int16_t pcm[POC_FRAME_SAMPLES];
    static const int16_t silence[POC_FRAME_SAMPLES] = {0};
    bool pumping = false;

    for (;;) {
        if (!s_uac || !s_uac->inCall()) {
            // Not in a call: nothing to pace against, so idle politely.
            pumping = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!pumping) {
            // Entering a call -- drop anything the far end queued before we
            // started pumping, so we don't inherit that as permanent latency.
            s_uac->flushRtpBacklog();
            pumping = true;
        }

        // Blocking read IS the clock for this loop.
        size_t got = audio_hardware_read_mic(pcm, POC_FRAME_SAMPLES);
        if (got > 0) s_uac->sendAudioFrame(pcm, got);

        size_t rx = s_uac->recvAudioFrame(pcm, POC_FRAME_SAMPLES);
        if (rx > 0) {
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

#if CONFIG_TDECK_MAX_AUDIO_SELFTEST
// Speaker and microphone failures are indistinguishable during a call --
// both yield silence. This separates them: an audible tone proves the
// DAC/I2S/amp path, and a non-zero mic level proves the ADC path without
// anyone having to hear anything.
static void audio_selftest(void)
{
    ESP_LOGW(TAG, "=== AUDIO SELF-TEST ===");

    // Before measuring anything: prove whether our I2C init actually landed
    // in the chip. "codec never configured" and "codec fine, wrong slot"
    // are indistinguishable from the audio alone.
    audio_hardware_dump_codec_regs();
    audio_hardware_probe_asdout_activity();

    // -- 1. Speaker: 1 kHz sine at POC_SAMPLE_RATE_HZ, ~2 s, amp on.
    audio_hardware_set_amp(true);
    vTaskDelay(pdMS_TO_TICKS(50));   // let the amp settle before driving it
    ESP_LOGW(TAG, "[1/2] playing 1 kHz tone for 2 s -- LISTEN NOW");

    int16_t tone[POC_FRAME_SAMPLES];
    const float step = 2.0f * 3.14159265f * 1000.0f / (float)POC_SAMPLE_RATE_HZ;
    float phase = 0.0f;
    const int frames = (POC_SAMPLE_RATE_HZ * 2) / POC_FRAME_SAMPLES;
    size_t written_total = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
            tone[i] = (int16_t)(8000.0f * sinf(phase));   // ~-12 dBFS, not full scale
            phase += step;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        written_total += audio_hardware_write_spk(tone, POC_FRAME_SAMPLES);
    }
    ESP_LOGW(TAG, "[1/2] tone done -- %u samples accepted by I2S (expected %d)",
             (unsigned)written_total, frames * POC_FRAME_SAMPLES);
    if (written_total == 0) {
        ESP_LOGE(TAG, "[1/2] I2S accepted ZERO samples -- TX path is not running at all");
    }

    // -- 2. Microphone: capture ~2 s and report level.
    ESP_LOGW(TAG, "[2/2] recording 2 s -- SPEAK NOW / make noise");
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
    ESP_LOGW(TAG, "[2/2] mic: %u samples, peak=%d rms=%d, silent/empty frames=%u/%d",
             (unsigned)total, (int)peak, rms, (unsigned)zeroFrames, frames);

    if (total == 0) {
        ESP_LOGE(TAG, "[2/2] mic returned NO DATA -- I2S RX not running");
    } else if (peak == 0) {
        ESP_LOGE(TAG, "[2/2] mic is bit-exact ZERO -- ADC dead or wrong I2S slot (see #27)");
    } else if (peak < 50) {
        ESP_LOGW(TAG, "[2/2] mic level very low (peak=%d) -- check PGA gain", (int)peak);
    } else {
        ESP_LOGW(TAG, "[2/2] mic is LIVE (peak=%d) -- ADC path good", (int)peak);
    }

    // If the mic read zero, sweep the I2S slot masks. A non-zero peak on a
    // different mask proves #27 (wrong slot) rather than a codec fault.
    if (peak == 0) {
        ESP_LOGW(TAG, "[3/3] mic was zero -- sweeping I2S slot masks (make noise)");
        int pl = audio_hardware_probe_mic_slot(0);
        int pr = audio_hardware_probe_mic_slot(1);
        int pb = audio_hardware_probe_mic_slot(2);
        if (pl <= 0 && pr <= 0 && pb <= 0) {
            ESP_LOGE(TAG, "[3/3] ALL slot masks read zero -- not a slot problem; "
                          "look at the codec capture path (REG0E/REG14/REG17)");
        } else {
            ESP_LOGW(TAG, "[3/3] a slot mask produced signal -- this is #27; "
                          "left=%d right=%d both=%d", pl, pr, pb);
        }
        // Restore the configuration the phone actually runs with.
        audio_hardware_probe_mic_slot(0);
    }

    audio_hardware_set_amp(false);
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

    // 3. Display task first, so failures after this point can be shown.
    s_render_q = xQueueCreate(1, sizeof(RenderRequest));
    xTaskCreate(epaper_task, "epaper", 4096, nullptr, 3, nullptr);
    ui_render(POC_SIP_EXT_SELF, "Booting", false);

    // 4. Wi-Fi (bounded; won't hang forever on a bad SSID).
    esp_err_t wifi_err = wifi_sta_connect(POC_WIFI_SSID, POC_WIFI_PASS);
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi did not come up; phone cannot register");
        ui_render(POC_SIP_EXT_SELF, "No WiFi", false);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Wi-Fi up, local IP %s", wifi_local_ip());

    // 5. SIP UAC, registered as a LAN extension.
    static TincanUac uac;
    if (!uac.init(wifi_local_ip(), POC_SIP_LOCAL_PORT, POC_RTP_LOCAL_PORT,
                  POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT, POC_SIP_EXT_SELF)) {
        ESP_LOGE(TAG, "UAC init failed (socket bind?)");
        ui_render(POC_SIP_EXT_SELF, "SIP init fail", false);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bool registered = uac.registerExt();
    s_uac = &uac;

    // 6. Audio pump. Priority above the main loop: starving it is audible,
    //    starving the UI is not. Pinned to core 1 to keep it away from the
    //    Wi-Fi/lwIP stack on core 0.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 6, nullptr, 1);

    UiState ui = UiState::Idle;
    std::string dialBuffer;
    ui_render(POC_SIP_EXT_SELF, registered ? "Idle" : "Register failed", false);
    ESP_LOGI(TAG, "System operational (registered=%d).", registered);

    // NOTE: the real T-Deck MAX keypad is a 4x10 QWERTY matrix, not a
    // numeric keypad -- only '0', DEL, and ENT are mapped today (#17).
    // Digits 1-9 live behind an ALT/SYM shift layer whose mapping isn't
    // confirmed, so dialling out is limited until that's derived on
    // hardware. Answer/reject/hangup need no digit entry and are fully
    // wired below.
    for (;;) {
        uac.poll();

        // Keep the registration alive; without this the binding lapses at
        // POC_SIP_REG_EXPIRES and the phone silently stops taking calls.
        if (uac.maintainRegistration() && ui == UiState::Idle) {
            ui_render(POC_SIP_EXT_SELF, "Reg failed", false);
        }

        // Single GPIO read unless the controller actually has events.
        char key = tca8418_key_pending() ? tca8418_get_key() : 0;

        if (uac.hasIncomingCall() && ui != UiState::InCall && ui != UiState::Incoming) {
            ui = UiState::Incoming;
            ESP_LOGI(TAG, "incoming call from %s", uac.incomingCallerId().c_str());
            ui_render(uac.incomingCallerId().c_str(), "Incoming Call", false);
        }

        switch (ui) {
        case UiState::Idle:
#ifdef POC_TEST_DIAL
            // Bench workaround for #17 -- ENT from Idle dials the configured
            // test target directly, since '*' and 1-9 can't be typed yet.
            if (key == '\r') {
                const char *target = POC_TEST_DIAL;
                ESP_LOGI(TAG, "test-dial -> %s", target);
                ui_render(target, "Calling...", false);
                if (uac.placeCall(target)) {
                    ui = UiState::InCall;
                    audio_hardware_set_amp(true);
                    ui_render(target, "In Call", true);
                    ESP_LOGI(TAG, "call up -- speak now, echo should return your voice");
                } else {
                    ui_render(target, "Call Failed", false);
                    ESP_LOGW(TAG, "test-dial to %s failed", target);
                }
                break;
            }
            if (std::isdigit((unsigned char)key)) {
#else
            if (key == '\r' || std::isdigit((unsigned char)key)) {
#endif
                dialBuffer.clear();
                if (std::isdigit((unsigned char)key)) dialBuffer += key;
                ui = UiState::Dialing;
                ui_render(dialBuffer.c_str(), "Dialing", false);
            }
            break;

        case UiState::Dialing:
            if (key == '\b') {
                if (!dialBuffer.empty()) dialBuffer.pop_back();
                else ui = UiState::Idle;
                ui_render(dialBuffer.c_str(), ui == UiState::Idle ? "Idle" : "Dialing", false);
            } else if (std::isdigit((unsigned char)key)) {
                dialBuffer += key;
                ui_render(dialBuffer.c_str(), "Dialing", false);
            } else if (key == '\r' && !dialBuffer.empty()) {
                ui_render(dialBuffer.c_str(), "Calling...", false);
                ESP_LOGI(TAG, "dialing %s", dialBuffer.c_str());
                if (uac.placeCall(dialBuffer)) {
                    ui = UiState::InCall;
                    audio_hardware_set_amp(true);
                    ui_render(dialBuffer.c_str(), "In Call", true);
                } else {
                    ui = UiState::Idle;
                    ui_render(dialBuffer.c_str(), "Call Failed", false);
                }
                dialBuffer.clear();
            }
            break;

        case UiState::Incoming:
            if (key == '\r') {
                uac.answer();
                ui = UiState::InCall;
                audio_hardware_set_amp(true);
                ui_render(uac.incomingCallerId().c_str(), "In Call", true);
            } else if (key == '\b') {
                uac.reject();
                ui = UiState::Idle;
                ui_render(POC_SIP_EXT_SELF, "Idle", false);
            } else if (!uac.hasIncomingCall()) {
                // Caller gave up (CANCEL) before we answered/rejected.
                ui = UiState::Idle;
                ui_render(POC_SIP_EXT_SELF, "Idle", false);
            }
            break;

        case UiState::InCall:
            // Media is handled entirely by audio_task; nothing to do here
            // but watch for hangup from either end.
            if (key == '\b' || key == '\r') uac.hangup();
            if (uac.callEnded() || !uac.inCall()) {
                ui = UiState::Idle;
                audio_hardware_set_amp(false);
                ui_render(POC_SIP_EXT_SELF, "Idle", false);
            }
            break;
        }

        // Control-loop cadence only -- audio is no longer paced by this.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
