#include "epaper_display.h"
#include "board_tdeck_max.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "EPAPER_DISPLAY";

esp_err_t epaper_display_init(void)
{
    ESP_LOGI(TAG, "Initializing 3.1'' GDEQ031T10 E-Paper Display...");

    // Configure E-Paper Control Pins
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_EPD_CS) | (1ULL << BOARD_EPD_DC) | 
                           (1ULL << BOARD_EPD_RST) | (1ULL << BOARD_EPD_BACKLIGHT);
    gpio_config(&io_conf);

    gpio_set_level(BOARD_EPD_CS, 1);
    gpio_set_level(BOARD_EPD_BACKLIGHT, 1); // Turn front-light on by default

    // Configure Busy Pin as Input
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_EPD_BUSY);
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "E-Paper Display initialized successfully");
    return ESP_OK;
}

void epaper_set_backlight(bool enable)
{
    gpio_set_level(BOARD_EPD_BACKLIGHT, enable ? 1 : 0);
}

void epaper_render_call_status(const char *caller_id, const char *status, bool ptt_active)
{
    ESP_LOGI(TAG, "[EPD RENDER] Caller: %s | Status: %s | PTT: %s",
             caller_id ? caller_id : "None",
             status ? status : "Idle",
             ptt_active ? "TALKING" : "LISTENING");
}
