#include "es8311_audio.h"
#include "board_tdeck_max.h"
#include "xl9555.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ES8311_AUDIO";

static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;

// ES8311 register map and init sequence, ported from LilyGO's own shipped
// esp_codec_dev ES8311 driver (lib/esp_codec_dev/device/es8311/es8311.c in
// their T-Deck-MAX repo) -- confirmed via review that this file previously
// only stood up the ESP32 I2S peripheral and never actually configured the
// ES8311 chip itself (zero I2C writes to BOARD_ES8311_I2C_ADDR anywhere in
// the tree). The ES8311 powers up in reset with ADC/DAC off; without this
// sequence you get silence both directions with a perfectly healthy-looking
// I2S clock on a scope.
#define ES8311_RESET_REG00       0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09       0x09
#define ES8311_SDPOUT_REG0A      0x0A
#define ES8311_SYSTEM_REG0B      0x0B
#define ES8311_SYSTEM_REG0C      0x0C
#define ES8311_SYSTEM_REG0D      0x0D
#define ES8311_SYSTEM_REG0E      0x0E
#define ES8311_SYSTEM_REG10      0x10
#define ES8311_SYSTEM_REG11      0x11
#define ES8311_SYSTEM_REG12      0x12
#define ES8311_SYSTEM_REG13      0x13
#define ES8311_SYSTEM_REG14      0x14
#define ES8311_ADC_REG15         0x15
#define ES8311_ADC_REG16         0x16
#define ES8311_ADC_REG17         0x17
#define ES8311_ADC_REG1B         0x1B
#define ES8311_ADC_REG1C         0x1C
#define ES8311_DAC_REG31         0x31
#define ES8311_DAC_REG32         0x32
#define ES8311_DAC_REG37         0x37
#define ES8311_GPIO_REG44        0x44
#define ES8311_GP_REG45          0x45

// Defined only for real-hardware builds: in sim mode the sole caller
// (es8311_codec_init) is compiled out too, so defining these would just
// produce -Wunused-function noise.
#if !CONFIG_TDECK_MAX_SIM_MODE
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_ES8311_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_ES8311_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // repeated start
    i2c_master_write_byte(cmd, (BOARD_ES8311_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Clock coefficients for MCLK=2.048MHz / 8kHz sample rate, from the vendor
// driver's coeff_div[] table -- exact match, not interpolated. 2.048MHz is
// what BOARD_I2S_MCLK actually outputs here: I2S_STD_CLK_DEFAULT_CONFIG()
// (used below in audio_hardware_init) sets mclk_multiple=256, and
// 8000 Hz * 256 = 2,048,000 Hz.
#define ES8311_PRE_DIV  0x01
#define ES8311_PRE_MULT 0x01 // x1
#define ES8311_ADC_DIV  0x01
#define ES8311_DAC_DIV  0x01
#define ES8311_FS_MODE  0x00 // single speed
#define ES8311_LRCK_H   0x00
#define ES8311_LRCK_L   0xFF
#define ES8311_BCLK_DIV 0x04
#define ES8311_ADC_OSR  0x10
#define ES8311_DAC_OSR  0x20

static esp_err_t es8311_codec_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t regv;

    // Enhance I2C noise immunity -- vendor driver writes this twice
    // deliberately (their comment: first write occasionally fails on this
    // chip).
    ret |= es8311_write_reg(ES8311_GPIO_REG44, 0x08);
    ret |= es8311_write_reg(ES8311_GPIO_REG44, 0x08);

    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= es8311_write_reg(ES8311_ADC_REG16, 0x24);
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG0B, 0x00);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG0C, 0x00);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG10, 0x1F);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG11, 0x7F);
    ret |= es8311_write_reg(ES8311_RESET_REG00, 0x80);

    // ESP32-S3 I2S is the bus MASTER (drives BCLK/WS -- see
    // audio_hardware_init's I2S_ROLE_MASTER below), so the codec must be
    // configured as I2S SLAVE.
    es8311_read_reg(ES8311_RESET_REG00, &regv);
    regv &= 0xBF; // slave mode
    ret |= es8311_write_reg(ES8311_RESET_REG00, regv);

    // MCLK is physically wired (BOARD_I2S_MCLK), not inverted.
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);

    es8311_read_reg(ES8311_CLK_MANAGER_REG06, &regv);
    regv &= ~0x20; // SCLK not inverted
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG06, regv);

    ret |= es8311_write_reg(ES8311_SYSTEM_REG13, 0x10);
    ret |= es8311_write_reg(ES8311_ADC_REG1B, 0x0A);
    ret |= es8311_write_reg(ES8311_ADC_REG1C, 0x6A);
    ret |= es8311_write_reg(ES8311_GPIO_REG44, 0x58); // internal ref signal (ADCL+DACR)

    // I2S standard (Philips) format, 16-bit slot width.
    uint8_t dac_iface, adc_iface;
    es8311_read_reg(ES8311_SDPIN_REG09, &dac_iface);
    es8311_read_reg(ES8311_SDPOUT_REG0A, &adc_iface);
    dac_iface = (dac_iface & 0xFC) | 0x0C; // format=normal I2S, bits=16
    adc_iface = (adc_iface & 0xFC) | 0x0C;
    ret |= es8311_write_reg(ES8311_SDPIN_REG09, dac_iface);
    ret |= es8311_write_reg(ES8311_SDPOUT_REG0A, adc_iface);

    // Clock coefficients for 8 kHz @ 2.048 MHz MCLK.
    es8311_read_reg(ES8311_CLK_MANAGER_REG02, &regv);
    regv &= 0x07;
    regv |= (ES8311_PRE_DIV - 1) << 5;
    regv |= 0 << 3; // pre_multi=x1 -> 0b00
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG02, regv);

    regv = ((ES8311_ADC_DIV - 1) << 4) | ((ES8311_DAC_DIV - 1) << 0);
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG05, regv);

    es8311_read_reg(ES8311_CLK_MANAGER_REG03, &regv);
    regv &= 0x80;
    regv |= (ES8311_FS_MODE << 6) | ES8311_ADC_OSR;
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG03, regv);

    es8311_read_reg(ES8311_CLK_MANAGER_REG04, &regv);
    regv &= 0x80;
    regv |= ES8311_DAC_OSR;
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG04, regv);

    es8311_read_reg(ES8311_CLK_MANAGER_REG07, &regv);
    regv &= 0xC0;
    regv |= ES8311_LRCK_H;
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG07, regv);

    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG08, ES8311_LRCK_L);

    es8311_read_reg(ES8311_CLK_MANAGER_REG06, &regv);
    regv &= 0xE0;
    regv |= (ES8311_BCLK_DIV < 19) ? (ES8311_BCLK_DIV - 1) : ES8311_BCLK_DIV;
    ret |= es8311_write_reg(ES8311_CLK_MANAGER_REG06, regv);

    // Start: enable both ADC and DAC paths (this phone uses both).
    ret |= es8311_write_reg(ES8311_ADC_REG17, 0xBF);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02);
    ret |= es8311_write_reg(ES8311_SYSTEM_REG12, 0x00); // enable DAC
    ret |= es8311_write_reg(ES8311_SYSTEM_REG14, 0x1A); // analog mic PGA, no digital mic

    ret |= es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01); // power up
    ret |= es8311_write_reg(ES8311_ADC_REG15, 0x40);
    ret |= es8311_write_reg(ES8311_DAC_REG37, 0x08);
    ret |= es8311_write_reg(ES8311_GP_REG45, 0x00);

    // Defaults: mic gain 24dB (a common starting point for an electret
    // mic; ES8311_ADC_REG16 gain enum value == register value directly),
    // DAC volume 0dB (0xBF -- ES8311's 0.5dB/step volume register is
    // conventionally centered so 0dB sits at 0xBF; the exact curve wasn't
    // independently verified against a datasheet in this session), and
    // unmuted. Both are safe starting points to bench-tune once hardware
    // is available, not load-bearing for "does audio work at all."
    ret |= es8311_write_reg(ES8311_ADC_REG16, 0x04);
    ret |= es8311_write_reg(ES8311_DAC_REG32, 0xBF);
    es8311_read_reg(ES8311_DAC_REG31, &regv);
    ret |= es8311_write_reg(ES8311_DAC_REG31, regv & 0x9F); // unmute

    return ret;
}
#endif // !CONFIG_TDECK_MAX_SIM_MODE

esp_err_t audio_hardware_init(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Initializing ES8311 I2S audio driver at %lu Hz...", sample_rate);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_SCLK,
            .ws   = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DSDIN,
            .din  = BOARD_I2S_ASDOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // Ensure audio routing is set to ES8311 local codec
    xl9555_set_audio_route(false);

#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] ES8311 codec register init skipped");
#else
    esp_err_t codec_ret = es8311_codec_init();
    if (codec_ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 codec register init failed (I2C NAK?): %d", codec_ret);
        return codec_ret;
    }
#endif

    ESP_LOGI(TAG, "Audio hardware initialized successfully");
    return ESP_OK;
}

void audio_hardware_set_amp(bool enable)
{
    xl9555_set_speaker_amp(enable);
}

size_t audio_hardware_read_mic(int16_t *buf, size_t samples)
{
    size_t bytes_read = 0;
    if (rx_chan == NULL) return 0;
    esp_err_t err = i2s_channel_read(rx_chan, buf, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(50));
    if (err != ESP_OK) return 0;
    return bytes_read / sizeof(int16_t);
}

size_t audio_hardware_write_spk(const int16_t *buf, size_t samples)
{
    size_t bytes_written = 0;
    if (tx_chan == NULL) return 0;
    esp_err_t err = i2s_channel_write(tx_chan, buf, samples * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(50));
    if (err != ESP_OK) return 0;
    return bytes_written / sizeof(int16_t);
}
