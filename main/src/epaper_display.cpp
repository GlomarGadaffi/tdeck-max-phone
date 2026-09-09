// SPI/command layer ported from tdeck-max-phone (main/src/epaper_display.cpp):
// same GDEQ031T10 (240x320, 1bpp, UC8253-family) command sequence, itself
// ported from LilyGO's own reference (examples/Elink_paper/GDEQ031T10_Arduino/
// Display_EPD_W21.cpp). One deliberate deviation carried over from that repo:
// the vendor's busy-wait is an unbounded `while(1)`; epd_wait_busy() here is
// bounded and returns instead of hanging forever.
//
// NOT ported from the source file: the 5x7 dialpad font, glyph_index(),
// draw_glyph()/draw_digit_string(), set_pixel()/fill_rect(), and
// epaper_render_call_status() -- all phone-dialpad-specific. This project
// renders through LVGL (lvgl_glue.c) instead and only needs the raw
// full-refresh push, exported as epaper_flush().
#include "epaper_display.h"
#include "board_tdeck_max.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "EPAPER_DISPLAY";

#define EPD_BUSY_TIMEOUT_MS 5000
#define EPD_FRONTLIGHT_DEFAULT_PCT 100 // 10-25 is plenty indoors; 100 = the old bare-GPIO behavior

// GDEQ031T10 command bytes, from the vendor reference.
#define EPD_CMD_PSR          0x00
#define EPD_CMD_POWER_ON     0x04
#define EPD_CMD_POWER_OFF    0x02
#define EPD_CMD_DEEP_SLEEP   0x07
#define EPD_CMD_OLD_DATA     0x10
#define EPD_CMD_NEW_DATA     0x13
#define EPD_CMD_REFRESH      0x12
#define EPD_PSR_DEFAULT      0x1F
#define EPD_DEEP_SLEEP_KEY   0xA5
// Waveform-select registers used by the vendor's fast/partial init variants
// (Display_EPD_W21.cpp: EPD_Init_Fast / EPD_Init_Part). Undocumented in the
// UC8253 datasheet; values are the vendor's, verbatim.
#define EPD_CMD_LUT_SEL_E0   0xE0
#define EPD_CMD_LUT_SEL_E5   0xE5
#define EPD_CMD_CDI          0x50   // VCOM and data interval setting
#define EPD_E0_FAST_PART     0x02
#define EPD_E5_FAST          0x5A   // vendor: ~1.0s
#define EPD_E5_PARTIAL       0x79
#define EPD_CDI_PARTIAL      0xD7

#if !CONFIG_TDECK_MAX_SIM_MODE
static spi_device_handle_t s_spi = NULL;
#endif
static uint8_t s_old_fb[EPD_BUF_SIZE]; // panel's last-known contents, required by the OLD_DATA transfer

#if !CONFIG_TDECK_MAX_SIM_MODE
static void epd_write_byte(uint8_t val, bool is_data)
{
    gpio_set_level(BOARD_EPD_DC, is_data ? 1 : 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &val;
    spi_device_transmit(s_spi, &t);
}

static inline void epd_write_cmd(uint8_t cmd) { epd_write_byte(cmd, false); }
static inline void epd_write_data(uint8_t data) { epd_write_byte(data, true); }

// Push a whole framebuffer as ONE transaction rather than 9,600 single-byte
// ones -- a single DMA burst instead of a driver round-trip per byte. D/C is
// a level held across the payload, so it only needs setting once.
static void epd_write_data_bulk(const uint8_t *buf, size_t len)
{
    gpio_set_level(BOARD_EPD_DC, 1); // data
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = buf;
    spi_device_transmit(s_spi, &t);
}

// Bounded version of the vendor reference's lcd_chkstatus(): BUSY reads
// HIGH when the panel is idle/ready. Returns false on timeout instead of
// hanging forever.
static bool epd_wait_busy(void)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(BOARD_EPD_BUSY) != 1) {
        if ((esp_timer_get_time() - start) / 1000 > EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY wait timed out");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

static bool epd_panel_init(epd_refresh_mode_t mode)
{
    gpio_set_level(BOARD_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    epd_write_cmd(EPD_CMD_PSR);
    epd_write_data(EPD_PSR_DEFAULT);

    epd_write_cmd(EPD_CMD_POWER_ON);
    if (!epd_wait_busy()) return false;

    // Vendor's EPD_Init_Fast / EPD_Init_Part: same as EPD_Init, then select
    // the faster waveform. Order (after POWER_ON's busy wait) is the vendor's.
    if (mode == EPD_REFRESH_FAST || mode == EPD_REFRESH_PARTIAL) {
        epd_write_cmd(EPD_CMD_LUT_SEL_E0);
        epd_write_data(EPD_E0_FAST_PART);
        epd_write_cmd(EPD_CMD_LUT_SEL_E5);
        epd_write_data(mode == EPD_REFRESH_FAST ? EPD_E5_FAST : EPD_E5_PARTIAL);
        if (mode == EPD_REFRESH_PARTIAL) {
            epd_write_cmd(EPD_CMD_CDI);
            epd_write_data(EPD_CDI_PARTIAL);
        }
    }
    return true;
}

static void epd_power_off_and_sleep(void)
{
    epd_write_cmd(EPD_CMD_POWER_OFF);
    epd_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(100));
    epd_write_cmd(EPD_CMD_DEEP_SLEEP);
    epd_write_data(EPD_DEEP_SLEEP_KEY);
}

// Whole-screen refresh in the requested waveform: panel needs both its
// last-known contents (OLD_DATA) and the new frame (NEW_DATA) to compute
// the waveform, then a REFRESH pulse. The data path is identical for all
// three modes (vendor's EPD_WhiteScreen_ALL vs EPD_Dis_PartAll differ only
// in the init variant). Re-initializes the panel every call per the
// vendor's own guidance, and deep-sleeps afterward to protect panel
// lifespan -- safe for PARTIAL too, since the RST pulse in init wakes it
// and OLD data is re-sent rather than relying on retained RAM.
// Panel power state. While the user is interacting, consecutive refreshes
// skip the reset/PSR/POWER_ON re-init (63ms) and the POWER_OFF + 100ms +
// deep-sleep tail (142ms): the controller stays powered with its LUT
// selection intact, and only the RAM writes + DRF happen -- the same
// "power on once, refresh many, hibernate later" flow GxEPD2 uses for this
// panel. epaper_sleep() (called by the flush task after an idle timeout)
// powers off and deep-sleeps; the next refresh re-inits from cold. A mode
// change (partial <-> full) always re-inits, since the waveform registers
// are written in init.
static bool s_awake = false;
static epd_refresh_mode_t s_awake_mode = EPD_REFRESH_FULL;

static void epd_refresh(const uint8_t *new_fb, epd_refresh_mode_t mode)
{
    int64_t t_start = esp_timer_get_time();

    if (!s_awake || s_awake_mode != mode) {
        if (!epd_panel_init(mode)) return;
        s_awake = true;
        s_awake_mode = mode;
    }
    int64_t t_init = esp_timer_get_time();

    epd_write_cmd(EPD_CMD_OLD_DATA);
    epd_write_data_bulk(s_old_fb, EPD_BUF_SIZE);

    epd_write_cmd(EPD_CMD_NEW_DATA);
    epd_write_data_bulk(new_fb, EPD_BUF_SIZE);
    memcpy(s_old_fb, new_fb, EPD_BUF_SIZE);
    int64_t t_data = esp_timer_get_time();

    epd_write_cmd(EPD_CMD_REFRESH);
    vTaskDelay(pdMS_TO_TICKS(1));
    epd_wait_busy();
    int64_t t_refr = esp_timer_get_time();

    // No POWER_OFF/deep-sleep here any more -- see s_awake / epaper_sleep().
    int64_t t_end = esp_timer_get_time();

    // Phase breakdown so the per-refresh overhead (init / data / waveform /
    // power-off) can be attacked with numbers rather than guesses.
    static const char *const names[] = {"full", "fast", "partial"};
    ESP_LOGI(TAG, "%s refresh: %lld ms (init %lld, data %lld, waveform %lld, off %lld)",
             names[mode], (t_end - t_start) / 1000, (t_init - t_start) / 1000,
             (t_data - t_init) / 1000, (t_refr - t_data) / 1000, (t_end - t_refr) / 1000);
}
#endif // !CONFIG_TDECK_MAX_SIM_MODE

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

    // Front-light on an LEDC PWM channel rather than a bare GPIO, so it can
    // be dimmed: an e-paper front-light is a glare source on a matte panel
    // and 10-25% is usually plenty indoors. Default duty kept at 100% for
    // now (unchanged behavior); EPD_FRONTLIGHT_DEFAULT_PCT / the setter below.
    ledc_timer_config_t lt = {};
    lt.speed_mode = LEDC_LOW_SPEED_MODE;
    lt.duty_resolution = LEDC_TIMER_10_BIT;
    lt.timer_num = LEDC_TIMER_0;
    lt.freq_hz = 5000;
    lt.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {};
    lc.gpio_num = BOARD_EPD_BACKLIGHT;
    lc.speed_mode = LEDC_LOW_SPEED_MODE;
    lc.channel = LEDC_CHANNEL_0;
    lc.timer_sel = LEDC_TIMER_0;
    lc.duty = 0;
    lc.hpoint = 0;
    ledc_channel_config(&lc);
    epaper_set_frontlight_pct(EPD_FRONTLIGHT_DEFAULT_PCT);

    // SPI2 is shared with the LoRa radio and the SD card. Park their CS
    // lines HIGH (deselected) before any transaction, or an undriven CS can
    // float low and corrupt e-paper traffic.
    gpio_config_t cs_conf = {};
    cs_conf.intr_type = GPIO_INTR_DISABLE;
    cs_conf.mode = GPIO_MODE_OUTPUT;
    cs_conf.pin_bit_mask = (1ULL << BOARD_LORA_CS) | (1ULL << BOARD_SD_CS);
    gpio_config(&cs_conf);
    gpio_set_level(BOARD_LORA_CS, 1);
    gpio_set_level(BOARD_SD_CS, 1);

    // Configure Busy Pin as Input
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOARD_EPD_BUSY);
    gpio_config(&io_conf);

    memset(s_old_fb, 0xFF, sizeof(s_old_fb));

#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] SPI bus/panel init skipped");
#else
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = BOARD_SPI_SCK;
    bus_cfg.mosi_io_num = BOARD_SPI_MOSI;
    // MISO is wired even though the e-paper never drives it: this is the
    // SHARED SPI2 bus (e-paper CS 34, LoRa CS 3, SD CS 48). Whoever
    // initializes the bus first fixes its pin set for everyone.
    bus_cfg.miso_io_num = BOARD_SPI_MISO;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = EPD_BUF_SIZE;
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { // ALREADY shared, e.g. by another initializer
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", ret);
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = 10 * 1000 * 1000; // matches vendor's SPISettings(10000000, ...)
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = BOARD_EPD_CS;
    dev_cfg.queue_size = 1;
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d", ret);
        return ret;
    }
#endif

    ESP_LOGI(TAG, "E-Paper Display initialized successfully");
    return ESP_OK;
}

static uint8_t s_frontlight_pct = EPD_FRONTLIGHT_DEFAULT_PCT;

void epaper_set_frontlight_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_frontlight_pct = pct;
    uint32_t duty = (1023u * pct) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void epaper_set_backlight(bool enable)
{
    uint32_t duty = enable ? (1023u * s_frontlight_pct) / 100u : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void epaper_flush_mode(const uint8_t *fb, epd_refresh_mode_t mode)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] refresh (mode %d) skipped", (int)mode);
    (void)fb;
#else
    epd_refresh(fb, mode);
#endif
}

void epaper_flush(const uint8_t *fb)
{
    epaper_flush_mode(fb, EPD_REFRESH_FULL);
}

void epaper_sleep(void)
{
#if !CONFIG_TDECK_MAX_SIM_MODE
    if (!s_awake) return;
    int64_t t0 = esp_timer_get_time();
    epd_power_off_and_sleep(); // bistable: image holds with zero static drain
    s_awake = false;
    ESP_LOGI(TAG, "panel deep sleep after idle (%lld ms)", (esp_timer_get_time() - t0) / 1000);
#endif
}
