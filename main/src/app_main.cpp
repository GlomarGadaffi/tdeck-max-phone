// ─────────────────────────────────────────────────────────────────────────────
//  tdeck-max-phone  —  app_main.cpp
//  SIP phone firmware for the LilyGO T-Deck MAX, registered as a LAN
//  extension to a drawbridge PBX instance (which owns all 3CX integration).
//
//  Task layout (three tasks, deliberately):
//    main       - SIP control, the LVGL pump (keypad + touch + e-paper
//                 render), the phone state machine. Never does slow I/O
//                 once the loop starts. The ONLY task that touches LVGL.
//    audio pump - RTP<->I2S, paced ONLY by the blocking i2s_channel_read().
//    epd_flush  - the 0.7-3 s panel refresh, off the control path entirely
//                 (lvgl_glue.c).
//
//  Boot: peripherals -> LVGL -> screens registered -> Wi-Fi (saved creds
//  or the on-device wizard) -> home grid -> SIP. A phone with no network
//  still boots to the home screen; SIP starts when Wi-Fi arrives.
// ─────────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_netif_sntp.h"
#include "driver/i2c.h"
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "board_tdeck_max.h"
#include "poc_config.h"
#include "net_wifi.h"
#include "xl9555.h"
#include "es8311_audio.h"
#include "tca8418_keypad.h"
#include "touch_cst3530.h"
#include "epaper_display.h"
#include "lvgl_glue.h"
#include "scr_mgr.h"
#include "app_nav.h"
#include "wifi_wizard.h"
#include "view_settings.h"
#include "view_contacts.h"
#include "contacts.h"
#include "call_log.h"
#include "phone_app.h"

static const char *TAG = "APP_MAIN";

// ── boot-time hardware bring-up helpers ─────────────────────────────────────

// Log-and-continue instead of ESP_ERROR_CHECK. On a board with six devices
// sharing one I2C bus, aborting on the first NAK gives a panic-reboot loop
// with no UI and no indication of WHICH device failed. Continuing lets the
// boot log report every failure at once and still reach the I2C scan. Set
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
// have: "is the bus even alive, and which chip is missing".
static void i2c_bus_scan(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGI(TAG, "[sim] I2C bus scan skipped");
#else
    struct { uint8_t addr; const char *name; } expected[] = {
        {0x18, "ES8311 audio codec"},
        {0x1A, "CST3530 touch"},
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

#if CONFIG_TDECK_MAX_AUDIO_SELFTEST
// Speaker and microphone failures are indistinguishable during a call --
// both yield silence. This separates them: an audible tone proves the
// DAC/I2S/amp path, and a non-zero mic level proves the ADC path.
static int32_t audio_pass(const char *label)
{
    const uint32_t sr = audio_hardware_sample_rate();
    const int frames = (int)((sr * 2) / POC_FRAME_SAMPLES);

    audio_hardware_set_amp(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGW(TAG, "[%s] playing 1 kHz tone for 2 s @ %lu Hz -- LISTEN NOW", label, sr);

    int16_t tone[POC_FRAME_SAMPLES];
    const float step = 2.0f * 3.14159265f * 1000.0f / (float)sr;
    float phase = 0.0f;
    size_t written_total = 0;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < POC_FRAME_SAMPLES; i++) {
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

// A one-line splash while a blocking boot step (saved-credential Wi-Fi
// connect, up to 30 s) runs before the shell exists.
static lv_obj_t *splash(const char *text)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *content = app_nav_frame(scr, "T-Deck Phone", "");
    lv_obj_t *l = lv_label_create(content);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 8);
    lvgl_glue_request_full_refresh();
    lv_disp_load_scr(scr);
    lvgl_glue_flush_now();
    return scr;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  LilyGO T-Deck MAX SIP Phone (via drawbridge PBX) ");
    ESP_LOGI(TAG, "==================================================");
    {
        static const char *const reasons[] = {
            "unknown", "power-on", "external pin", "software reset", "PANIC",
            "interrupt watchdog", "TASK WATCHDOG", "other watchdog", "deep sleep wake",
            "brownout", "SDIO", "USB", "JTAG", "efuse", "power glitch", "CPU lockup",
        };
        int r = (int)esp_reset_reason();
        ESP_LOGW(TAG, "reset reason: %d (%s)", r,
                 (r >= 0 && r < (int)(sizeof(reasons) / sizeof(reasons[0]))) ? reasons[r] : "?");
    }

    try_init("nvs_flash", nvs_flash_init());

    // 1. Shared I2C bus, then scan it before trusting any device driver.
    try_init("i2c bus", i2c_master_init());
    i2c_bus_scan();

    // 2. Peripherals. Every one is log-and-continue.
    try_init("XL9555 expander", xl9555_init());
    // 8 kHz: G.711 is the only codec drawbridge will carry, and nothing in
    // this firmware resamples.
    try_init("ES8311 codec", audio_hardware_init(POC_SAMPLE_RATE_HZ));
    // xl9555_init() brings the speaker amp up enabled; nothing should be
    // playing at boot. Re-enabled per call.
    audio_hardware_set_amp(false);
    try_init("TCA8418 keypad", tca8418_init());
    try_init("GDEQ031T10 e-paper", epaper_display_init());
    // Optional: a unit without a working touch layer is still fully usable
    // from the keypad.
    if (touch_cst3530_init() == ESP_OK) ESP_LOGI(TAG, "init OK   : CST3530 touch");
    else                                ESP_LOGW(TAG, "init SKIP : CST3530 touch (keypad only)");

    if (s_init_failed) {
        ESP_LOGW(TAG, "*** one or more peripherals failed to init (see above) ***");
        ESP_LOGW(TAG, "*** continuing anyway; check the I2C scan for absent devices ***");
    }

#if CONFIG_TDECK_MAX_AUDIO_SELFTEST
    audio_selftest();   // before Wi-Fi, so nothing else competes for the bus
#endif

    // 3. LVGL + the shell. From here on this task is the only LVGL owner.
    lv_group_t *group = lvgl_glue_init();
    if (!group) {
        ESP_LOGE(TAG, "LVGL init failed (draw buffer allocation) -- halting");
        vTaskDelete(NULL);
        return;
    }
    scr_mgr_init();
    try_init("contacts", contacts_init());
    try_init("call log", call_log_init());

    phone_app_register_screens();
    view_contacts_register();
    view_settings_register();
    app_nav_register("Phone",    LV_SYMBOL_CALL,     SCR_PHONE);
    app_nav_register("Contacts", LV_SYMBOL_LIST,     SCR_CONTACTS);
    app_nav_register("Settings", LV_SYMBOL_SETTINGS, SCR_SETTINGS);
    app_nav_init();

    settings_apply_at_boot(); // static-IP config, TZ, front-light -- before the STA netif exists

    // 4. Wi-Fi. Saved credentials (from the wizard or Settings) win; the
    //    first boot runs the on-device wizard. A failed saved connect still
    //    reaches the home screen -- net_wifi keeps retrying in the
    //    background and SIP starts when the link arrives.
    lv_obj_t *boot_scr = NULL;
    if (wifi_wizard_has_saved_credentials()) {
        boot_scr = splash("Connecting to Wi-Fi...");
        esp_err_t err = wifi_wizard_connect_saved();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saved Wi-Fi credentials didn't connect (%s); continuing without",
                     esp_err_to_name(err));
        }
    } else {
        wifi_wizard_run(group); // blocks until connected; persists credentials
    }
    if (wifi_is_connected()) ESP_LOGI(TAG, "Wi-Fi up, local IP %s", wifi_local_ip());

    // Local clock for the status bar: TZ default from Kconfig (Settings
    // overrides via NVS, applied above), time via SNTP once the network is up.
    if (!getenv("TZ")) setenv("TZ", CONFIG_TDECK_MAX_TZ, 1);
    tzset();
    {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TDECK_MAX_SNTP_SERVER);
        esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
        if (err != ESP_OK) ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }

    // 5. Home grid, then SIP (registerExt() blocks up to ~6 s -- before the
    //    watchdog is armed).
    app_nav_show_home();
    if (boot_scr) lv_obj_del(boot_scr);
    if (wifi_is_connected()) {
        if (phone_app_start() != ESP_OK) ESP_LOGE(TAG, "SIP did not start; will retry from the loop");
    }
    ESP_LOGI(TAG, "System operational (wifi=%d sip=%d registered=%d).",
             (int)wifi_is_connected(), (int)phone_app_started(), (int)phone_app_registered());

    // 6. Steady state. Nothing in this loop may block for long; the task
    //    watchdog turns a silent hang into a panic + core dump.
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        phone_app_tick();
        lvgl_glue_pump();
        app_nav_status_tick();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
