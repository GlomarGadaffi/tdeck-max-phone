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
