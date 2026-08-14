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
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/i2c.h"
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
static void power_off(TincanUac &uac)
{
    ESP_LOGW(TAG, "powering off");

    // Don't leave the far end talking to a corpse.
    if (uac.inCall()) {
        uac.hangup();
        vTaskDelay(pdMS_TO_TICKS(150));   // let the BYE actually get out
    }
    audio_hardware_set_amp(false);

    bool onUsb = false;
    if (pmu_sy6970_vbus_present(&onUsb) != ESP_OK) {
        ESP_LOGW(TAG, "could not read VBUS state; assuming battery");
        onUsb = false;
    }

    // Posted through the queue, not rendered inline: epaper_task owns the SPI
    // bus and driving it from here would race a refresh already in flight.
    // The panel needs roughly two seconds for a full refresh.
    ui_render("", onUsb ? "Sleeping (USB)" : "Powered Off", false);
    vTaskDelay(pdMS_TO_TICKS(2500));

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

    for (;;) {
        if (!s_uac || !s_uac->inCall()) {
            // Not in a call: nothing to pace against, so idle politely.
            pumping = false;
            duckFrames = 0;
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
            s_uac->sendAudioFrame(micbuf, got);
        }

        size_t rx = s_uac->recvAudioFrame(pcm, POC_FRAME_SAMPLES);
        if (rx > 0) {
            // Decide "is the far end talking" on the RAW decoded frame, before
            // the gain below. Measuring post-gain would mean the threshold
            // silently retunes itself every time POC_RX_GAIN_DB changes.
            int32_t sumAbs = 0;
            for (size_t i = 0; i < rx; i++) {
                sumAbs += pcm[i] < 0 ? -pcm[i] : pcm[i];
            }
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
            // DEL from Idle asks to power down. Two single taps rather than a
            // long press: hold detection depends on the TCA8418 press/release
            // polarity, and tca8418_keypad.cpp:180 reads press as (raw & 0x80)
            // while the Adafruit driver in the vendor tree documents the
            // opposite -- unverified, and the same inversion was a real bug in
            // peri_keypad.cpp. A confirm step also stops a stray DEL from
            // switching the phone off mid-shift. See docs/UI_DESIGN.md.
            if (key == '\b') {
                ui = UiState::ConfirmOff;
                ui_render("Power off?", "ENT=yes DEL=no", false);
                break;
            }
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

        case UiState::ConfirmOff:
            if (key == '\r') {
                power_off(uac);             // does not return
            } else if (key != 0) {
                // Any other key cancels, not just DEL -- if you are unsure
                // enough to press something random, you did not mean to shut
                // the phone down.
                ui = UiState::Idle;
                ui_render(POC_SIP_EXT_SELF, "Idle", false);
            } else if (uac.hasIncomingCall()) {
                // An incoming call outranks a pending power-off prompt; the
                // hasIncomingCall() check above the switch has already moved
                // us on, this just avoids leaving the prompt on screen.
                ui_render(uac.incomingCallerId().c_str(), "Incoming Call", false);
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
