// Real GDEQ031T10 (240x320, 1bpp, UC8253-family controller) SPI command
// sequence, ported from LilyGO's own T-Deck-MAX reference driver
// (examples/Elink_paper/GDEQ031T10_Arduino/Display_EPD_W21.cpp) -- same
// panel setting/power-on/refresh/deep-sleep command bytes, adapted from
// Arduino SPI.transfer() + digitalWrite() to ESP-IDF's spi_master driver.
//
// One deliberate deviation from the vendor reference: their busy-wait is an
// unbounded `while(1)` with no timeout. That's a real hang risk (blocks
// forever if the panel is ever unresponsive, e.g. under QEMU with no panel
// attached at all), so epd_wait_busy() here is bounded and returns an error
// instead of spinning forever.
#include "epaper_display.h"
#include "board_tdeck_max.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "EPAPER_DISPLAY";

#define EPD_WIDTH        240
#define EPD_HEIGHT       320
#define EPD_BYTES_PER_ROW (EPD_WIDTH / 8)             // 30
#define EPD_BUF_SIZE      (EPD_BYTES_PER_ROW * EPD_HEIGHT) // 9600, matches vendor's EPD_ARRAY

#define EPD_BUSY_TIMEOUT_MS 5000

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

static spi_device_handle_t s_spi = NULL;
static uint8_t s_fb[EPD_BUF_SIZE];       // 1bpp framebuffer, 1=white 0=black, MSB-first per row
static uint8_t s_old_fb[EPD_BUF_SIZE];   // panel's last-known contents, required by the OLD_DATA transfer

static void set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    uint8_t *byte = &s_fb[y * EPD_BYTES_PER_ROW + (x / 8)];
    uint8_t mask = 0x80 >> (x % 8);
    if (black) *byte &= ~mask;
    else *byte |= mask;
}

static void fill_rect(int x, int y, int w, int h, bool black)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            set_pixel(x + i, y + j, black);
}

// Compact 5x7 digit font (0-9), MSB of each row byte = leftmost column.
// Used for caller ID / extension number readout -- the only free-form text
// this PoC's UI needs to render precisely. Status is drawn as a fixed
// pictogram per state (see epaper_render_call_status) rather than a full
// alphabet font; a real character set is UI-polish scope beyond this PoC.
static const uint8_t s_digit_font[10][7] = {
    {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}, // 0
    {0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70}, // 1
    {0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8}, // 2
    {0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70}, // 3
    {0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10}, // 4
    {0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70}, // 5
    {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}, // 6
    {0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40}, // 7
    {0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70}, // 8
    {0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60}, // 9
};

static void draw_digit(int x, int y, int digit, int scale)
{
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = s_digit_font[digit][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x80 >> col)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

// Draws digits only; any other character (including '+', spaces, letters
// in a caller name) is rendered as a blank gap. Good enough for phone
// numbers/extensions, which is what this field actually carries.
static void draw_digit_string(int x, int y, const char *s, int scale, int spacing)
{
    int cx = x;
    for (const char *p = s; p && *p; p++) {
        if (*p >= '0' && *p <= '9') {
            draw_digit(cx, y, *p - '0', scale);
        }
        cx += 5 * scale + spacing;
        if (cx > EPD_WIDTH - 5 * scale) break; // clip to panel width
    }
}

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
// ones. The naive per-byte loop costs a driver round-trip per byte (~300 ms
// of pure overhead per refresh, twice per screen); this is a single DMA
// burst. D/C is a level held across the payload, so it only needs setting
// once.
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

static bool epd_panel_init(void)
{
    gpio_set_level(BOARD_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    epd_write_cmd(EPD_CMD_PSR);
    epd_write_data(EPD_PSR_DEFAULT);

    epd_write_cmd(EPD_CMD_POWER_ON);
    return epd_wait_busy();
}

static void epd_power_off_and_sleep(void)
{
    epd_write_cmd(EPD_CMD_POWER_OFF);
    epd_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(100));
    epd_write_cmd(EPD_CMD_DEEP_SLEEP);
    epd_write_data(EPD_DEEP_SLEEP_KEY);
}

// Full-screen refresh: panel needs both its last-known contents (OLD_DATA)
// and the new frame (NEW_DATA) to compute the waveform, then a REFRESH
// pulse. This always full-refreshes (no partial-refresh support here --
// GDEQ031T10 supports it per the vendor reference, but that needs the
// base-map RAM dance in EPD_SetRAMValue_BaseMap/EPD_Dis_Part, deferred as
// UI-polish scope). Re-initializes the panel every call per the vendor's
// own guidance ("Re-initialization is required for every full screen
// update"), and deep-sleeps afterward to protect panel lifespan.
static void epd_full_refresh(const uint8_t *new_fb)
{
    if (!epd_panel_init()) return;

    epd_write_cmd(EPD_CMD_OLD_DATA);
    epd_write_data_bulk(s_old_fb, EPD_BUF_SIZE);

    epd_write_cmd(EPD_CMD_NEW_DATA);
    epd_write_data_bulk(new_fb, EPD_BUF_SIZE);
    memcpy(s_old_fb, new_fb, EPD_BUF_SIZE);

    epd_write_cmd(EPD_CMD_REFRESH);
    vTaskDelay(pdMS_TO_TICKS(1));
    epd_wait_busy();

    epd_power_off_and_sleep();
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
    gpio_set_level(BOARD_EPD_BACKLIGHT, 1); // Turn front-light on by default

    // SPI2 is shared with the LoRa radio and the SD card. Park their CS
    // lines HIGH (deselected) before any transaction, or an undriven CS can
    // float low and corrupt e-paper traffic. Called out as a bench
    // verification item in docs/HARDWARE_CAVEATS.md; doing it in code costs
    // nothing and removes the failure mode.
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

    memset(s_fb, 0xFF, sizeof(s_fb));
    memset(s_old_fb, 0xFF, sizeof(s_old_fb));

#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] SPI bus/panel init skipped");
#else
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = BOARD_SPI_SCK;
    bus_cfg.mosi_io_num = BOARD_SPI_MOSI;
    // MISO is wired even though the e-paper never drives it: this is the
    // SHARED SPI2 bus (e-paper CS 34, LoRa CS 3, SD CS 48). Whoever
    // initializes the bus first fixes its pin set for everyone, so leaving
    // MISO at -1 here would silently make the SX1262 and the SD card
    // unusable the moment either is added.
    bus_cfg.miso_io_num = BOARD_SPI_MISO;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = EPD_BUF_SIZE;
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { // ALREADY shared with LoRa/SD on real hardware
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

    memset(s_fb, 0xFF, sizeof(s_fb));

    // Caller ID / extension, large digits centered near the top.
    if (caller_id && caller_id[0]) {
        draw_digit_string(20, 40, caller_id, 6, 10);
    }

    // Status pictogram: a placeholder visual state indicator (filled =
    // active/ringing, outline = idle) rather than rendered status text --
    // a real icon set / font is UI-polish scope beyond this PoC.
    bool active = ptt_active || (status && strstr(status, "Call") != NULL) ||
                  (status && strstr(status, "Ring") != NULL);
    if (active) {
        fill_rect(EPD_WIDTH / 2 - 30, 140, 60, 60, true);
    } else {
        fill_rect(EPD_WIDTH / 2 - 30, 140, 60, 4, true);
        fill_rect(EPD_WIDTH / 2 - 30, 196, 60, 4, true);
        fill_rect(EPD_WIDTH / 2 - 30, 140, 4, 60, true);
        fill_rect(EPD_WIDTH / 2 + 26, 140, 4, 60, true);
    }

#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] full refresh skipped");
#else
    epd_full_refresh(s_fb);
#endif
}
