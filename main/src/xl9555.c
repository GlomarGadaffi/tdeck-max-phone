// Ported verbatim from tdeck-max-phone (main/src/xl9555.c). This brings up
// EVERY XL9555 rail (including ones this project never uses -- LoRa, GPS,
// 4G, haptics) because partial rail assertion has already bitten that
// project once: 4 of 11 pins left a codec answering on I2C with a dead
// ADC/DAC and an IMU that never appeared on the bus at all. Don't "trim"
// this to only the rails this project needs without measuring first.
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

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] read_reg(0x%02x) skipped", reg);
    *val = 0;
    return ESP_OK;
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (XL9555_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // repeated start
    i2c_master_write_byte(cmd, (XL9555_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
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
    // Full power-rail bring-up, mirroring LilyGO's factory example: assert
    // EVERY enable HIGH, one at a time, with a settle between each.
    //
    // We previously set only 4 of 11 pins in a single burst. That left the
    // 1.8V rail (P0_3), LoRa, GPS and haptics rails unpowered, and produced
    // a codec that answered on I2C with every register correct while its
    // ADC and DAC were both dead -- and a BHI260AP that never appeared on
    // the bus at all. These are power enables; partial assertion leaves
    // devices half-powered rather than simply absent.
    //
    // Sequenced one pin per write (not one bulk write) because the vendor
    // does it that way with a delay between each, and inrush on several
    // rails coming up simultaneously is a plausible reason why.
    static const uint8_t p0_seq[] = {
        XL9555_P0_6609_EN, XL9555_P0_LORA_EN, XL9555_P0_GPS_EN,
        XL9555_P0_1V8_EN,  XL9555_P0_LORA_SEL, XL9555_P0_DRV2605_EN,
        XL9555_P0_SPK_AMP_EN, XL9555_P0_TOUCH_RST,
    };
    static const uint8_t p1_seq[] = {
        XL9555_P1_4G_PWR, XL9555_P1_KEYBOARD_RST, XL9555_P1_AUDIO_ROUTE,
        XL9555_P1_ANT_SWITCH,
    };

    // Every write below is checked and logged (not fire-and-forget): a
    // silent NAK here is exactly the "answers on I2C but half-powered"
    // failure mode the comment above describes, and it must not be masked
    // by an unconditional ESP_OK.
    esp_err_t bringup_ret = ESP_OK;

    s_p0_out = 0;
    for (size_t i = 0; i < sizeof(p0_seq) / sizeof(p0_seq[0]); i++) {
        s_p0_out |= p0_seq[i];
        esp_err_t r = write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Port 0 bring-up write %u/%u failed (mask 0x%02x): %s",
                     (unsigned)i + 1, (unsigned)(sizeof(p0_seq) / sizeof(p0_seq[0])),
                     s_p0_out, esp_err_to_name(r));
            bringup_ret = r;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_p1_out = 0;
    for (size_t i = 0; i < sizeof(p1_seq) / sizeof(p1_seq[0]); i++) {
        s_p1_out |= p1_seq[i];
        esp_err_t r = write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Port 1 bring-up write %u/%u failed (mask 0x%02x): %s",
                     (unsigned)i + 1, (unsigned)(sizeof(p1_seq) / sizeof(p1_seq[0])),
                     s_p1_out, esp_err_to_name(r));
            bringup_ret = r;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Let the rails settle before anything talks to a peripheral.
    vTaskDelay(pdMS_TO_TICKS(50));

    // AUDIO_SEL comes up HIGH with the rest, but HIGH routes audio to the
    // A7682E. This project has no audio path at all, but we still drop it
    // back LOW to match the hardware-verified source exactly rather than
    // leave an unverified state behind.
    s_p1_out &= ~XL9555_P1_AUDIO_ROUTE;
    esp_err_t r = write_reg(XL9555_REG_OUTPUT_P1, s_p1_out);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drop AUDIO_ROUTE back low: %s", esp_err_to_name(r));
        bringup_ret = r;
    }

    // Deterministic touch reset pulse, as the vendor does, so the controller
    // can't sit half-powered after a warm reset.
    s_p0_out &= ~XL9555_P0_TOUCH_RST;
    r = write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Touch reset (assert) write failed: %s", esp_err_to_name(r));
        bringup_ret = r;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    s_p0_out |= XL9555_P0_TOUCH_RST;
    r = write_reg(XL9555_REG_OUTPUT_P0, s_p0_out);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Touch reset (release) write failed: %s", esp_err_to_name(r));
        bringup_ret = r;
    }
    vTaskDelay(pdMS_TO_TICKS(60));

    if (bringup_ret == ESP_OK) {
        ESP_LOGI(TAG, "XL9555 expander initialized successfully");
    } else {
        ESP_LOGE(TAG, "XL9555 expander initialized with at least one failed write "
                       "-- a rail may be left half-powered, see errors above");
    }
    return bringup_ret;
}

esp_err_t xl9555_read_ports(uint8_t *p0_val, uint8_t *p1_val)
{
    esp_err_t r0 = read_reg(XL9555_REG_INPUT_P0, p0_val);
    if (r0 != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Port 0 input: %s", esp_err_to_name(r0));
        return r0;
    }
    esp_err_t r1 = read_reg(XL9555_REG_INPUT_P1, p1_val);
    if (r1 != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Port 1 input: %s", esp_err_to_name(r1));
        return r1;
    }
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
