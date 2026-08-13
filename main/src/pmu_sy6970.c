#include "pmu_sy6970.h"
#include "board_tdeck_max.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "SY6970";

#define SY6970_I2C_ADDR      0x6A

// REG09 bit 5 -- BATFET_DIS. Setting it forces the battery FET open, which
// is how this board powers itself off. Confirmed against XPowersLib's
// PowersSY6970.tpp: shutdown() -> disableBatterPowerPath() ->
// setRegisterBit(POWERS_PPM_REG_09H, 5).
#define SY6970_REG09         0x09
#define SY6970_REG09_BATFET_DIS_BIT  (1 << 5)

// REG0B bits [7:5] -- VBUS_STAT. Non-zero means something is feeding VBUS.
#define SY6970_REG0B         0x0B
#define SY6970_REG0B_VBUS_STAT_MASK  0xE0

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)reg; *val = 0; return ESP_OK;
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)reg; (void)val; return ESP_OK;
#else
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SY6970_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

esp_err_t pmu_sy6970_vbus_present(bool *present)
{
    if (present == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t v = 0;
    esp_err_t ret = read_reg(SY6970_REG0B, &v);
    if (ret != ESP_OK) return ret;
    *present = (v & SY6970_REG0B_VBUS_STAT_MASK) != 0;
    return ESP_OK;
}

esp_err_t pmu_sy6970_shutdown(void)
{
    uint8_t v = 0;
    esp_err_t ret = read_reg(SY6970_REG09, &v);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "REG09 read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGW(TAG, "REG09 0x%02x -> 0x%02x (BATFET_DIS)", v, v | SY6970_REG09_BATFET_DIS_BIT);
    return write_reg(SY6970_REG09, v | SY6970_REG09_BATFET_DIS_BIT);
}
