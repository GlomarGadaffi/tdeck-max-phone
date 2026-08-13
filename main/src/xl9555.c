#include "xl9555.h"
#include "board_tdeck_max.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "XL9555";

// XL9555 Registers
#define XL9555_REG_INPUT_P0     0x00
#define XL9555_REG_INPUT_P1     0x01
#define XL9555_REG_OUTPUT_P0    0x02
#define XL9555_REG_OUTPUT_P1    0x03
#define XL9555_REG_CONFIG_P0    0x06
#define XL9555_REG_CONFIG_P1    0x07

static uint8_t s_p0_out = 0x00;
static uint8_t s_p1_out = 0x00;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] write_reg(0x%02x, 0x%02x) skipped", reg, val);
    return ESP_OK;
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (XL9555_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

esp_err_t xl9555_init(void)
{
    // Set Port 0 & Port 1 pins as outputs (0x00 = all output)
    esp_err_t ret = write_reg(XL9555_REG_CONFIG_P0, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure Port 0 directions");
        return ret;
    }
    ret = write_reg(XL9555_REG_CONFIG_P1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure Port 1 directions");
        return ret;
    }

    // Default states
    // P0_0 (6609_EN) must be HIGH for the audio analog path to work -- see
    // the note in board_tdeck_max.h. LilyGO's own playback example sets it
    // even though it never uses the modem.
    s_p0_out = XL9555_P0_6609_EN | XL9555_P0_SPK_AMP_EN | XL9555_P0_TOUCH_RST;
    s_p1_out = XL9555_P1_4G_PWR | XL9555_P1_KEYBOARD_RST;
    write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
    write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);

    ESP_LOGI(TAG, "XL9555 expander initialized successfully");
    return ESP_OK;
}

esp_err_t xl9555_write_port0(uint8_t val)
{
    s_p0_out = val;
    return write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
}

esp_err_t xl9555_write_port1(uint8_t val)
{
    s_p1_out = val;
    return write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
}

esp_err_t xl9555_set_speaker_amp(bool enable)
{
    if (enable) s_p0_out |= XL9555_P0_SPK_AMP_EN;
    else s_p0_out &= ~XL9555_P0_SPK_AMP_EN;
    return write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
}

esp_err_t xl9555_set_4g_power(bool enable)
{
    if (enable) s_p1_out |= XL9555_P1_4G_PWR;
    else s_p1_out &= ~XL9555_P1_4G_PWR;
    return write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
}

esp_err_t xl9555_set_audio_route(bool use_4g)
{
    if (use_4g) s_p1_out |= XL9555_P1_AUDIO_ROUTE;
    else s_p1_out &= ~XL9555_P1_AUDIO_ROUTE;
    return write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
}

esp_err_t xl9555_set_motor_enable(bool enable)
{
    if (enable) s_p0_out |= XL9555_P0_DRV2605_EN;
    else s_p0_out &= ~XL9555_P0_DRV2605_EN;
    return write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
}

esp_err_t xl9555_reset_touch(void)
{
    s_p0_out &= ~XL9555_P0_TOUCH_RST;
    write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_p0_out |= XL9555_P0_TOUCH_RST;
    return write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
}

esp_err_t xl9555_reset_keyboard(void)
{
    s_p1_out &= ~XL9555_P1_KEYBOARD_RST;
    write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_p1_out |= XL9555_P1_KEYBOARD_RST;
    return write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
}
