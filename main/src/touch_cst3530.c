// See touch_cst3530.h. Protocol facts come from Hynitron's own cst3xx driver
// (hyn_cst3xx.c + hyn_i2c.c in the Meshtastic T-Deck-MAX port, which logs
// "Hynitron touch fw_ver:1010001" on this exact board):
//   - register addresses are 16-bit, sent big-endian (0xD000 -> D0 00),
//     write-then-read with a repeated start;
//   - init: reset, ~50 ms, write 0xD101 (info mode), 1 ms, read 28 bytes at
//     0xD1F4 (res x/y at [4..7], chip type [18..19], fw ver [20..23]), then
//     write 0xD109 (normal mode);
//   - report: read 7 bytes at 0xD000; valid iff buf[6]==0xAB && buf[0]!=0xAB;
//     buf[5]&0x7F = finger count, bit7 = key event (ignored here);
//     x = buf[1]<<4 | buf[3]>>4, y = buf[2]<<4 | buf[3]&0xF, z = buf[4],
//     (buf[0]&0x0F)==0x06 = contact; then write 0xD000AB (D0 00 AB) to tell
//     the chip the frame was consumed.
#include "touch_cst3530.h"
#include "board_tdeck_max.h"
#include "xl9555.h"
#include "epaper_display.h"   // EPD_WIDTH/EPD_HEIGHT = the display coordinate space
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "CST3530";

#define TOUCH_I2C_ADDR 0x1A

// Orientation of the touch layer relative to the 240x320 portrait display.
// UNVERIFIED on hardware -- defaults to identity. Enable
// CONFIG_TDECK_MAX_TOUCH_DEBUG, touch each corner, and set these from
// the logged raw values.
#define TOUCH_SWAP_XY   0
#define TOUCH_MIRROR_X  0
#define TOUCH_MIRROR_Y  0

static bool s_present = false;
static uint16_t s_res_x = 0, s_res_y = 0; // chip-reported native resolution (0 = unknown)

#if !CONFIG_TDECK_MAX_SIM_MODE
static esp_err_t wr_reg(uint32_t reg, uint8_t reg_len, uint8_t *rbuf, size_t rlen)
{
    uint8_t wbuf[4] = {0};
    for (int i = reg_len; i > 0; i--) {
        wbuf[i - 1] = (uint8_t)reg;
        reg >>= 8;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, wbuf, reg_len, true);
    if (rlen) {
        i2c_master_start(cmd); // repeated start
        i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_READ, true);
        i2c_master_read(cmd, rbuf, rlen, I2C_MASTER_LAST_NACK);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        // DIAG (rate-limited): a touch read failing every poll would also
        // starve the keypad on the shared bus -- make it visible.
        static int64_t s_last_err_us = 0;
        static uint32_t s_err_count = 0;
        s_err_count++;
        int64_t now = esp_timer_get_time();
        if (now - s_last_err_us > 2000000) {
            s_last_err_us = now;
            ESP_LOGW(TAG, "DIAG i2c err %s (x%lu so far) reg 0x%lx", esp_err_to_name(ret),
                     (unsigned long)s_err_count, (unsigned long)reg);
        }
    }
    return ret;
}
#endif

esp_err_t touch_cst3530_init(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] touch init skipped");
    return ESP_ERR_NOT_FOUND;
#else
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << BOARD_TOUCH_INT);
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    // Hynitron's own init pulses reset then waits 50 ms before talking to it,
    // regardless of any earlier pulse (xl9555_init() already did one).
    xl9555_reset_touch();
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t info[28] = {0};
    esp_err_t err = wr_reg(0xD101, 2, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    if (err == ESP_OK) err = wr_reg(0xD1F4, 2, info, sizeof(info));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no CST3xx at 0x%02x (%s) -- touch disabled", TOUCH_I2C_ADDR, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    s_res_x = (uint16_t)((info[5] << 8) | info[4]);
    s_res_y = (uint16_t)((info[7] << 8) | info[6]);
    uint16_t chip = (uint16_t)((info[19] << 8) | info[18]);
    uint32_t fw = ((uint32_t)info[23] << 24) | ((uint32_t)info[22] << 16) | ((uint32_t)info[21] << 8) | info[20];
    ESP_LOGI(TAG, "chip type 0x%04x fw 0x%08lx native res %ux%u", chip, (unsigned long)fw, s_res_x, s_res_y);

    err = wr_reg(0xD109, 2, NULL, 0); // normal reporting mode
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to enter normal mode (%s) -- touch disabled", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    s_present = true;
    return ESP_OK;
#endif
}

bool touch_cst3530_present(void)
{
    return s_present;
}

bool touch_cst3530_read(int16_t *x, int16_t *y)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)x; (void)y;
    return false;
#else
    if (!s_present) return false;

    uint8_t buf[7];
    if (wr_reg(0xD000, 2, buf, sizeof(buf)) != ESP_OK) return false;
    // Frame-consumed tail write, unconditionally, as the reference driver does.
    wr_reg(0xD000AB, 3, NULL, 0);

    if (buf[6] != 0xAB || buf[0] == 0xAB) return false;
    if (buf[5] & 0x80) return false;              // key event, not a finger
    uint8_t fingers = buf[5] & 0x7F;
    if (fingers == 0) return false;

    uint16_t rx = (uint16_t)(((uint16_t)buf[1] << 4) | ((buf[3] >> 4) & 0x0F));
    uint16_t ry = (uint16_t)(((uint16_t)buf[2] << 4) | (buf[3] & 0x0F));
    bool contact = (buf[0] & 0x0F) == 0x06;

#if CONFIG_TDECK_MAX_TOUCH_DEBUG
    static int64_t s_last_log = 0;
    int64_t now = esp_timer_get_time();
    if (now - s_last_log > 200000) {
        s_last_log = now;
        ESP_LOGI(TAG, "raw x=%u y=%u z=%u ev=0x%02x fingers=%u", rx, ry, buf[4], buf[0] & 0x0F, fingers);
    }
#endif
    if (!contact) return false;

    // Scale from the chip's native grid to the display if it reports one.
    uint32_t sx = rx, sy = ry;
    if (s_res_x > 1 && s_res_x != EPD_WIDTH)  sx = (uint32_t)rx * (EPD_WIDTH - 1) / (s_res_x - 1);
    if (s_res_y > 1 && s_res_y != EPD_HEIGHT) sy = (uint32_t)ry * (EPD_HEIGHT - 1) / (s_res_y - 1);

#if TOUCH_SWAP_XY
    { uint32_t t = sx; sx = sy; sy = t; }
#endif
#if TOUCH_MIRROR_X
    sx = (EPD_WIDTH - 1) - sx;
#endif
#if TOUCH_MIRROR_Y
    sy = (EPD_HEIGHT - 1) - sy;
#endif
    if (sx > EPD_WIDTH - 1)  sx = EPD_WIDTH - 1;
    if (sy > EPD_HEIGHT - 1) sy = EPD_HEIGHT - 1;
    *x = (int16_t)sx;
    *y = (int16_t)sy;
    return true;
#endif
}
