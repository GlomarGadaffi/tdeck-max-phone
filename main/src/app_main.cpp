// ─────────────────────────────────────────────────────────────────────────────
//  tdeck-max-phone  —  app_main.cpp
//  Media-Anchored SIP Phone Firmware for LilyGO T-Deck MAX
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
#include "TDeckMaxAudioAnchor.hpp"
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
    ESP_LOGI(TAG, "  LilyGO T-Deck MAX Media-Anchored SIP Phone      ");
    ESP_LOGI(TAG, "==================================================");

    ESP_ERROR_CHECK(nvs_flash_init());

    // 1. Initialize Shared I2C Bus & XL9555 Expander
    i2c_master_init();
    ESP_ERROR_CHECK(xl9555_init());

    // 2. Initialize ES8311 Audio Codec (8000 Hz for G.711 SIP)
    ESP_ERROR_CHECK(audio_hardware_init(8000));

    // 3. Initialize TCA8418 QWERTY Keypad
    ESP_ERROR_CHECK(tca8418_init());

    // 4. Initialize 3.1" Front-Lit E-Paper Display
    ESP_ERROR_CHECK(epaper_display_init());

    // 5. Connect to Wi-Fi (blocks until a DHCP lease is obtained)
    ESP_ERROR_CHECK(wifi_sta_connect(POC_WIFI_SSID, POC_WIFI_PASS));
    ESP_LOGI(TAG, "Wi-Fi up, local IP %s", wifi_local_ip());

    // 6. Bring up Media Anchor & Handset UAC
    TDeckMaxAudioAnchor anchor;
    anchor.init("http://127.0.0.1", "tdeck_max", "secret", "100");
    anchor.start();

    TincanUac uac;
    uac.init("127.0.0.1", 5060, 4000, "127.0.0.1", 5060, "100", "102");
    uac.startMediaLoop();

    // 7. Update E-Paper Screen with Initial State
    epaper_render_call_status("Ext 100", "Registered (Idle)", false);

    ESP_LOGI(TAG, "System operational. Press Keypad or Trackball PTT to dial.");

    // Main event pump loop
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
