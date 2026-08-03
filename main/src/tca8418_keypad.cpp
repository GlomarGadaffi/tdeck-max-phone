#include "tca8418_keypad.h"
#include "board_tdeck_max.h"
#include "xl9555.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TCA8418_KEYPAD";

esp_err_t tca8418_init(void)
{
    ESP_LOGI(TAG, "Initializing TCA8418 keypad controller...");

    // Reset TCA8418 via XL9555
    xl9555_reset_keyboard();

    // Configure Keyboard Backlight LED Pin
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_KEYBOARD_LED);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(BOARD_KEYBOARD_LED, 1); // Enable backlight by default

    // Configure Keyboard Interrupt Pin
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_KEYBOARD_INT);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "TCA8418 keypad initialized successfully");
    return ESP_OK;
}

char tca8418_get_key(void)
{
    // Mock / Polling stub for TCA8418 FIFO key retrieval
    // In production, reads the TCA8418 FIFO register over I2C on interrupt
    return 0;
}

void tca8418_set_backlight(bool enable)
{
    gpio_set_level(BOARD_KEYBOARD_LED, enable ? 1 : 0);
}
