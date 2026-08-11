// ─────────────────────────────────────────────────────────────────────────────
//  tdeck-max-phone  —  app_main.cpp
//  SIP Phone Firmware for LilyGO T-Deck MAX, registered as a LAN extension
//  to a drawbridge PBX instance (which owns all 3CX integration).
// ─────────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include <cctype>
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

static void i2c_master_init(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = BOARD_I2C_SDA;
    conf.scl_io_num = BOARD_I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    ESP_ERROR_CHECK(i2c_param_config(BOARD_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(BOARD_I2C_PORT, conf.mode, 0, 0, 0));
}

static void run_call_audio_pump(TincanUac &uac)
{
    int16_t pcm[POC_FRAME_SAMPLES];
    size_t got = audio_hardware_read_mic(pcm, POC_FRAME_SAMPLES);
    if (got > 0) uac.sendAudioFrame(pcm, got);

    size_t rxSamples = uac.recvAudioFrame(pcm, POC_FRAME_SAMPLES);
    if (rxSamples > 0) {
        audio_hardware_write_spk(pcm, rxSamples);
    } else {
        int16_t silence[POC_FRAME_SAMPLES] = {0};
        audio_hardware_write_spk(silence, POC_FRAME_SAMPLES);
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  LilyGO T-Deck MAX SIP Phone (via drawbridge PBX) ");
    ESP_LOGI(TAG, "==================================================");

    ESP_ERROR_CHECK(nvs_flash_init());

    // 1. Initialize Shared I2C Bus & XL9555 Expander
    i2c_master_init();
    ESP_ERROR_CHECK(xl9555_init());

    // 2. Initialize ES8311 Audio Codec (8000 Hz for G.711 SIP). The I2S
    // driver runs full-duplex (simultaneous mic read + speaker write) out
    // of the box -- confirmed via QEMU boot log: "the rx channel on I2S0
    // is switched from master to slave for full-duplex mode" -- so this
    // phone doesn't need tincan's half-duplex PTT compromise.
    ESP_ERROR_CHECK(audio_hardware_init(POC_SAMPLE_RATE_HZ));

    // 3. Initialize TCA8418 QWERTY Keypad
    ESP_ERROR_CHECK(tca8418_init());

    // 4. Initialize 3.1" Front-Lit E-Paper Display
    ESP_ERROR_CHECK(epaper_display_init());

    // 5. Connect to Wi-Fi (blocks until a DHCP lease is obtained)
    ESP_ERROR_CHECK(wifi_sta_connect(POC_WIFI_SSID, POC_WIFI_PASS));
    ESP_LOGI(TAG, "Wi-Fi up, local IP %s", wifi_local_ip());

    // 6. Bring up the SIP UAC and register as a LAN extension.
    TincanUac uac;
    if (!uac.init(wifi_local_ip(), POC_SIP_LOCAL_PORT, POC_RTP_LOCAL_PORT,
                  POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT, POC_SIP_EXT_SELF)) {
        ESP_LOGE(TAG, "UAC init failed");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bool registered = uac.registerExt();
    UiState ui = UiState::Idle;
    std::string dialBuffer;
    epaper_render_call_status(POC_SIP_EXT_SELF, registered ? "Idle" : "Register failed", false);
    ESP_LOGI(TAG, "System operational (registered=%d).", registered);

    // NOTE: the real T-Deck MAX keypad is a 4x10 QWERTY matrix, not a
    // numeric keypad -- only '0', DEL, and ENT are mapped today (see #8).
    // Digits 1-9 live behind an ALT/SYM shift layer this PoC doesn't have
    // a confirmed mapping for yet, so dialing out is currently limited to
    // whatever's reachable with '0' alone. Answering/rejecting/hanging up
    // an inbound call (which is what actually proves the 3CX path works)
    // needs no digit entry and is fully wired below. Tracked as a known
    // limitation for #13's bench-test doc.
    for (;;) {
        uac.poll();
        char key = tca8418_get_key();

        // An inbound offer can arrive in any UI state except while already
        // on a call (TincanUac itself sends 486 Busy for that case) or
        // while we're mid-dial-out (placeCall() blocks and won't return
        // control here until it resolves -- a known single-call-setup-at-
        // a-time limitation of this single-threaded loop).
        if (uac.hasIncomingCall() && ui != UiState::InCall && ui != UiState::Incoming) {
            ui = UiState::Incoming;
            ESP_LOGI(TAG, "incoming call from %s", uac.incomingCallerId().c_str());
            epaper_render_call_status(uac.incomingCallerId().c_str(), "Incoming Call", false);
        }

        switch (ui) {
        case UiState::Idle:
            if (key == '\r' || std::isdigit((unsigned char)key)) {
                dialBuffer.clear();
                if (std::isdigit((unsigned char)key)) dialBuffer += key;
                ui = UiState::Dialing;
                epaper_render_call_status(dialBuffer.c_str(), "Dialing", false);
            }
            break;

        case UiState::Dialing:
            if (key == '\b') {
                if (!dialBuffer.empty()) dialBuffer.pop_back();
                else ui = UiState::Idle;
                epaper_render_call_status(dialBuffer.c_str(), ui == UiState::Idle ? "Idle" : "Dialing", false);
            } else if (std::isdigit((unsigned char)key)) {
                dialBuffer += key;
                epaper_render_call_status(dialBuffer.c_str(), "Dialing", false);
            } else if (key == '\r' && !dialBuffer.empty()) {
                epaper_render_call_status(dialBuffer.c_str(), "Calling...", false);
                ESP_LOGI(TAG, "dialing %s", dialBuffer.c_str());
                if (uac.placeCall(dialBuffer)) {
                    ui = UiState::InCall;
                    epaper_render_call_status(dialBuffer.c_str(), "In Call", true);
                    audio_hardware_set_amp(true);
                } else {
                    ui = UiState::Idle;
                    epaper_render_call_status(dialBuffer.c_str(), "Call Failed", false);
                }
                dialBuffer.clear();
            }
            break;

        case UiState::Incoming:
            if (key == '\r') {
                uac.answer();
                ui = UiState::InCall;
                epaper_render_call_status(uac.incomingCallerId().c_str(), "In Call", true);
                audio_hardware_set_amp(true);
            } else if (key == '\b') {
                uac.reject();
                ui = UiState::Idle;
                epaper_render_call_status(POC_SIP_EXT_SELF, "Idle", false);
            } else if (!uac.hasIncomingCall()) {
                // Caller gave up (CANCEL) before we answered/rejected.
                ui = UiState::Idle;
                epaper_render_call_status(POC_SIP_EXT_SELF, "Idle", false);
            }
            break;

        case UiState::InCall:
            run_call_audio_pump(uac);
            if (key == '\b' || key == '\r') uac.hangup();
            if (uac.callEnded()) {
                ui = UiState::Idle;
                audio_hardware_set_amp(false);
                epaper_render_call_status(POC_SIP_EXT_SELF, "Idle", false);
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // ~one G.711 frame period
    }
}
