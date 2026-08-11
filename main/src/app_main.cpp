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

#include "board_tdeck_max.h"
#include "poc_config.h"
#include "net_wifi.h"
#include "xl9555.h"
#include "es8311_audio.h"
#include "tca8418_keypad.h"
#include "epaper_display.h"
#include "tincan_uac.hpp"

static const char *TAG = "APP_MAIN";

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

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  LilyGO T-Deck MAX SIP Phone (via drawbridge PBX) ");
    ESP_LOGI(TAG, "==================================================");

    ESP_ERROR_CHECK(nvs_flash_init());

    // 1. Initialize Shared I2C Bus & XL9555 Expander
    i2c_master_init();
    ESP_ERROR_CHECK(xl9555_init());

    // 2. Initialize ES8311 Audio Codec (8000 Hz for G.711 SIP)
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
    epaper_render_call_status(POC_SIP_EXT_SELF, registered ? "Idle" : "Register failed", false);

    // NOTE: this loop auto-answers any inbound call and never dials out --
    // it exists to prove the SIP/RTP/G.711/audio-hardware path works
    // end-to-end. Keypad-driven dial/answer/hangup and full e-paper call
    // state UI are WS5 scope (#11), building on this loop.
    ESP_LOGI(TAG, "System operational (registered=%d). Waiting for calls...", registered);

    int16_t pcm[POC_FRAME_SAMPLES];
    bool wasInCall = false;

    for (;;) {
        uac.poll();

        if (uac.hasIncomingCall() && !uac.inCall()) {
            ESP_LOGI(TAG, "auto-answering call from %s", uac.incomingCallerId().c_str());
            uac.answer();
        }

        if (uac.inCall()) {
            if (!wasInCall) {
                audio_hardware_set_amp(true);
                epaper_render_call_status(uac.incomingCallerId().c_str(), "In Call", true);
                wasInCall = true;
            }
            size_t got = audio_hardware_read_mic(pcm, POC_FRAME_SAMPLES);
            if (got > 0) uac.sendAudioFrame(pcm, got);

            size_t rxSamples = uac.recvAudioFrame(pcm, POC_FRAME_SAMPLES);
            if (rxSamples > 0) {
                audio_hardware_write_spk(pcm, rxSamples);
            } else {
                int16_t silence[POC_FRAME_SAMPLES] = {0};
                audio_hardware_write_spk(silence, POC_FRAME_SAMPLES);
            }
        } else if (wasInCall) {
            audio_hardware_set_amp(false);
            wasInCall = false;
        }

        if (uac.callEnded()) {
            epaper_render_call_status(POC_SIP_EXT_SELF, "Idle", false);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // ~one G.711 frame period
    }
}
