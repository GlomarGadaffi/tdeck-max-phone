// GDEQ031T10 (240x320, 1bpp, UC8253 controller) SPI driver.
//
// Reference: the command sequence and every register VALUE in this file come
// from the driver that the Meshtastic t-deck-max build runs on this exact
// board. That build is the hardware-verified reference for this panel:
//
//   GlomarGadaffi/tdeck-max-meshtastic (draft PR #10680)
//     variants/esp32s3/t-deck-max/platformio.ini
//       EINK_DISPLAY_MODEL=GxEPD2_310_GDEQ031T10, zinggjm/GxEPD2@1.6.9
//     src/graphics/EInkDisplay2.cpp (T_DECK_MAX branch)
//       init(115200, true, 2, false, SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0))
//   GxEPD2 1.6.9 src/gdeq/GxEPD2_310_GDEQ031T10.cpp
//       _InitDisplay(), _Update_Full(), _Update_Part(), _setPartialRamArea()
//
// LilyGO's own demo (T-Deck-MAX examples/Elink_paper/GDEQ031T10_Arduino/
// Display_EPD_W21.cpp), which the first version of this driver was ported
// from, agrees wherever the two overlap: its EPD_Init_Fast() is GxEPD2's fast
// full refresh and its EPD_Init_Part() is GxEPD2's partial refresh.
//
// Only hardware facts (command bytes, register values, timings) are taken
// from those sources. The code is written fresh for ESP-IDF; GxEPD2 and
// Meshtastic are GPL-3.0 and nothing is copied from them.
//
// What changed against the previous version of this file, and why:
//
//   * Full refresh now forces the fast waveform: CCSET(0xE0)=0x02,
//     TSSET(0xE5)=0x5A, CDI(0x50)=0x97. The old init wrote none of these,
//     so the controller used its temperature-sensed OTP waveform -- GxEPD2's
//     header gives that as 3082 ms against 1015 ms for the fast one, which is
//     exactly the 3277 ms measured in UI_DESIGN 5.7 once reset and SPI are
//     added. Cost, stated by GxEPD2 itself: the forced-temperature waveform
//     gives up the extended low-temperature range.
//   * Partial refresh exists: the same window/waveform sequence GxEPD2 uses
//     (0x91 / 0x90 window / data / 0x92, then CCSET 0x02, TSSET 0x79,
//     CDI 0xD7). Old-plane data is the real s_old_fb contents, as GxEPD2
//     keeps it, not the vendor demo's 0xFF placeholder (partFlag).
//   * PSR is two bytes, 0x1F 0x0D, as GxEPD2 writes it (the demo writes one).
//   * SPI clock 4 MHz (Meshtastic's SPISettings), reset pulse 2 ms low
//     (Meshtastic's reset_duration), busy timeout 10 s (GxEPD2's).
//
// One deliberate deviation from the Meshtastic build: it sets
// EINK_NOT_HIBERNATE and leaves the controller awake between updates, because
// GxEPD2 only rewrites the "previous" plane after a refresh and needs the
// controller RAM to survive until the next one. This driver rewrites BOTH
// planes from s_old_fb/s_fb on every refresh, so it does not depend on
// controller RAM at all, and keeps the hard-reset -> refresh -> power off ->
// deep sleep cycle this board has been running since #9 (the same lifecycle
// GxEPD2 implements in its own hibernate() path). If partial refresh ever
// misbehaves after deep sleep (UI_DESIGN U10), set EPD_DEEP_SLEEP_BETWEEN to 0
// first: that keeps the power-off but skips 0x07.
//
// The busy-wait is bounded (both references spin forever) so an unresponsive
// panel -- or QEMU with no panel at all -- fails a refresh instead of hanging
// the render task.
#include "epaper_display.h"
#include "board_tdeck_max.h"
#include "ui_render.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "EPAPER_DISPLAY";

#define EPD_WIDTH         UI_W        // 240
#define EPD_HEIGHT        UI_H        // 320
#define EPD_BYTES_PER_ROW UI_STRIDE   // 30
#define EPD_BUF_SIZE      UI_FB_SIZE  // 9600, matches vendor's EPD_ARRAY

#define EPD_SPI_HZ          (4 * 1000 * 1000) // Meshtastic: SPISettings(4000000, MSBFIRST, SPI_MODE0)
#define EPD_RESET_LOW_MS    2                 // Meshtastic: init(..., reset_duration = 2, ...)
#define EPD_RESET_SETTLE_MS 10                // GxEPD2 _reset(): max(reset_duration, 10)
#define EPD_BUSY_TIMEOUT_MS 10000             // GxEPD2: busy_timeout 10000000 us

// 1 = power off AND deep sleep after every refresh (this board's lifecycle
// since #9). 0 = power off only. See the header comment; this is the first
// knob to turn if partial refresh misbehaves after a deep sleep.
#define EPD_DEEP_SLEEP_BETWEEN 1

// UC8253 command bytes.
#define EPD_CMD_PSR          0x00  // panel setting
#define EPD_CMD_POWER_OFF    0x02
#define EPD_CMD_POWER_ON     0x04
#define EPD_CMD_DEEP_SLEEP   0x07
#define EPD_CMD_OLD_DATA     0x10  // "previous" plane: what is on the glass now
#define EPD_CMD_REFRESH      0x12
#define EPD_CMD_NEW_DATA     0x13  // "current" plane: what should be there
#define EPD_CMD_CDI          0x50  // VCOM and data interval setting
#define EPD_CMD_PTL_WINDOW   0x90  // partial window
#define EPD_CMD_PTL_IN       0x91  // enter partial mode
#define EPD_CMD_PTL_OUT      0x92  // leave partial mode
#define EPD_CMD_CCSET        0xE0  // cascade setting
#define EPD_CMD_TSSET        0xE5  // force temperature (selects the OTP waveform)

// Register values, verbatim from GxEPD2 1.6.9 GxEPD2_310_GDEQ031T10.cpp.
#define EPD_PSR_0            0x1F  // _InitDisplay(): KW mode, LUT from OTP
#define EPD_PSR_1            0x0D
#define EPD_CCSET_TSFIX      0x02  // use the TSSET value instead of the sensor
#define EPD_TSSET_FULL_FAST  0x5A  // _Update_Full(): "90, 1015000us"
#define EPD_TSSET_PARTIAL    0x79  // _Update_Part(): "121"
#define EPD_CDI_FULL         0x97  // _Update_Full()
#define EPD_CDI_PARTIAL      0xD7  // _Update_Part()
#define EPD_DEEP_SLEEP_KEY   0xA5  // hibernate(): "check code"

#if !CONFIG_TDECK_MAX_SIM_MODE
static spi_device_handle_t s_spi = NULL;   // unused (and undefined) in sim builds
#endif
static uint8_t s_fb[EPD_BUF_SIZE];       // 1bpp framebuffer, 1=white 0=black, MSB-first per row
static uint8_t s_old_fb[EPD_BUF_SIZE];   // what is on the glass: the OLD_DATA plane

// True once a full refresh has laid down a base image, so s_old_fb really
// does match the glass. A partial refresh only drives pixels whose old and new
// bits differ, so on a glass that does not match s_old_fb it would leave
// stale content behind. LilyGO's demo says the same thing about its base map:
// "this function is necessary, please do not delete it!!!".
static bool s_glass_valid = false;

// Waveform time of the most recent refresh (REFRESH command to BUSY release).
static int s_last_waveform_ms = 0;

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

// Push a block of rows as ONE transaction rather than one per byte. The naive
// per-byte loop costs a driver round-trip per byte (~300 ms of pure overhead
// per full screen, twice per refresh); this is a single DMA burst. D/C is a
// level held across the payload, so it only needs setting once.
static void epd_write_data_bulk(const uint8_t *buf, size_t len)
{
    gpio_set_level(BOARD_EPD_DC, 1); // data
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = buf;
    spi_device_transmit(s_spi, &t);
}

// BUSY reads LOW while the controller is working (GxEPD2 constructs this
// panel with busy_level LOW). The 1 ms lead-in is GxEPD2's "margin to become
// active": BUSY may not have dropped yet when the command that raised it was
// only just clocked out. Returns false on timeout instead of hanging.
static bool epd_wait_busy(const char *what)
{
    vTaskDelay(1);   // one tick = 1 ms at CONFIG_FREERTOS_HZ=1000
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(BOARD_EPD_BUSY) != 1) {
        if ((esp_timer_get_time() - start) / 1000 > EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY wait timed out (%s)", what);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

// Hard reset, then panel setting. GxEPD2 reaches the same state through
// _reset() + _InitDisplay() whenever it wakes the controller from hibernate,
// which is the state this driver leaves it in after every refresh.
static void epd_reset_and_init(void)
{
    gpio_set_level(BOARD_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));                       // GxEPD2: delay(10) before the pulse
    gpio_set_level(BOARD_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(EPD_RESET_LOW_MS) + 1);     // >= 2 ms whatever the tick phase
    gpio_set_level(BOARD_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(EPD_RESET_SETTLE_MS));

    epd_write_cmd(EPD_CMD_PSR);
    epd_write_data(EPD_PSR_0);
    epd_write_data(EPD_PSR_1);
}

// 0x90 window. X is in PIXELS, byte-aligned, with an inclusive end rounded up
// to the last pixel of its byte -- GxEPD2's _setPartialRamArea() writes
// `x & 0xFFF8` and `(x + w - 1) | 7`. That settles UI_DESIGN U9, but every
// window here is still full width (x 0..239) per #42, so a band is a
// contiguous slice of the framebuffer and needs no per-row masking.
static void epd_set_window(int y0, int y1)
{
    const int x0 = 0;
    const int x1 = (EPD_WIDTH - 1) | 7;   // 239
    epd_write_cmd(EPD_CMD_PTL_WINDOW);
    epd_write_data((uint8_t)x0);
    epd_write_data((uint8_t)x1);
    epd_write_data((uint8_t)(y0 >> 8));
    epd_write_data((uint8_t)(y0 & 0xFF));
    epd_write_data((uint8_t)(y1 >> 8));
    epd_write_data((uint8_t)(y1 & 0xFF));
    epd_write_data(0x01);
}

static void epd_power_off_and_sleep(void)
{
    epd_write_cmd(EPD_CMD_POWER_OFF);
    epd_wait_busy("power off");
#if EPD_DEEP_SLEEP_BETWEEN
    vTaskDelay(pdMS_TO_TICKS(100));   // LilyGO demo EPD_DeepSleep(): "At least 100ms delay"
    epd_write_cmd(EPD_CMD_DEEP_SLEEP);
    epd_write_data(EPD_DEEP_SLEEP_KEY);
#endif
}

// One refresh of rows y0..y1 (inclusive, full width). `partial` selects the
// waveform: false = GxEPD2 _Update_Full() fast full refresh, true =
// _Update_Part(). Both planes are written every time -- OLD from s_old_fb,
// NEW from new_fb -- inside a 0x91/0x90 window, exactly as GxEPD2's
// _writeImage() does; for a full refresh the window is the whole panel.
//
// Returns the wall time in ms, or -1 if the panel did not respond (in which
// case the glass is no longer known to match s_old_fb).
static int epd_refresh_rows(const uint8_t *new_fb, int y0, int y1, bool partial)
{
    int64_t t_start = esp_timer_get_time();
    const size_t off = (size_t)y0 * EPD_BYTES_PER_ROW;
    const size_t len = (size_t)(y1 - y0 + 1) * EPD_BYTES_PER_ROW;

    epd_reset_and_init();

    epd_write_cmd(EPD_CMD_PTL_IN);
    epd_set_window(y0, y1);
    epd_write_cmd(EPD_CMD_OLD_DATA);
    epd_write_data_bulk(s_old_fb + off, len);
    epd_write_cmd(EPD_CMD_NEW_DATA);
    epd_write_data_bulk(new_fb + off, len);
    epd_write_cmd(EPD_CMD_PTL_OUT);

    if (partial) {
        epd_write_cmd(EPD_CMD_PTL_IN);
        epd_set_window(y0, y1);
    }
    epd_write_cmd(EPD_CMD_CCSET);
    epd_write_data(EPD_CCSET_TSFIX);
    epd_write_cmd(EPD_CMD_TSSET);
    epd_write_data(partial ? EPD_TSSET_PARTIAL : EPD_TSSET_FULL_FAST);
    epd_write_cmd(EPD_CMD_CDI);
    epd_write_data(partial ? EPD_CDI_PARTIAL : EPD_CDI_FULL);

    epd_write_cmd(EPD_CMD_POWER_ON);
    bool ok = epd_wait_busy("power on");
    if (ok) {
        int64_t t_wave = esp_timer_get_time();
        epd_write_cmd(EPD_CMD_REFRESH);
        ok = epd_wait_busy(partial ? "partial refresh" : "full refresh");
        // The waveform alone: the glass is final when this ends. The rest of
        // the wall time is reset, SPI, power on/off and the deep-sleep delay.
        s_last_waveform_ms = (int)((esp_timer_get_time() - t_wave) / 1000);
    }
    if (partial) epd_write_cmd(EPD_CMD_PTL_OUT);

    epd_power_off_and_sleep();

    if (!ok) {
        s_glass_valid = false;
        return -1;
    }
    memcpy(s_old_fb + off, new_fb + off, len);
    return (int)((esp_timer_get_time() - t_start) / 1000);
}
#else
// Sim build: no panel, but keep the bookkeeping identical so the refresh
// policy above this layer behaves the same under QEMU.
static int epd_refresh_rows(const uint8_t *new_fb, int y0, int y1, bool partial)
{
    (void)partial;
    const size_t off = (size_t)y0 * EPD_BYTES_PER_ROW;
    memcpy(s_old_fb + off, new_fb + off, (size_t)(y1 - y0 + 1) * EPD_BYTES_PER_ROW);
    return 0;
}
#endif // !CONFIG_TDECK_MAX_SIM_MODE

// Full-screen refresh. Re-initializes the panel every call (LilyGO: "Re-
// initialization is required for every full screen update") and powers it
// down afterwards. Also the only thing that makes s_glass_valid true.
static void epd_full_refresh(const uint8_t *new_fb)
{
    int ms = epd_refresh_rows(new_fb, 0, EPD_HEIGHT - 1, false);
    if (ms < 0) return;
    s_glass_valid = true;
    ESP_LOGI(TAG, "full refresh: %d ms (waveform %d ms)", ms, s_last_waveform_ms);
}

// Partial refresh of rows y0..y1 (full width). Not reachable from the render
// path yet -- that is #42, which needs the band API (#38) first. Until then
// its only caller is the CONFIG_TDECK_MAX_EPD_BENCH measurement below.
[[maybe_unused]] static bool epd_partial_refresh(const uint8_t *new_fb, int y0, int y1)
{
    if (!s_glass_valid) {
        // Nothing to diff against: the glass is unknown, so lay a base down.
        epd_full_refresh(new_fb);
        return false;
    }
    int ms = epd_refresh_rows(new_fb, y0, y1, true);
    if (ms < 0) return false;
    ESP_LOGI(TAG, "partial refresh rows %d-%d: %d ms (waveform %d ms)", y0, y1, ms,
             s_last_waveform_ms);
    return true;
}

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
    gpio_set_level(BOARD_EPD_RST, 1);       // GxEPD2 presets RST high before the first pulse
    gpio_set_level(BOARD_EPD_BACKLIGHT, 1); // Turn front-light on by default

    // SPI2 is shared with the LoRa radio and the SD card. Park their CS
    // lines HIGH (deselected) before any transaction, or an undriven CS can
    // float low and corrupt e-paper traffic. Meshtastic's T_DECK_MAX branch
    // of EInkDisplay::connect() does the same before touching the panel.
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
    s_glass_valid = false;

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
    dev_cfg.clock_speed_hz = EPD_SPI_HZ;
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

static bool all_num_glyphs(const char *s)
{
    for (const char *p = s; *p; p++)
        if (!ui_num_has_glyph(*p)) return false;
    return true;
}

void epaper_render_call_status(const char *caller_id, const char *status, bool ptt_active)
{
    ESP_LOGI(TAG, "[EPD RENDER] Caller: %s | Status: %s | PTT: %s",
             caller_id ? caller_id : "None",
             status ? status : "Idle",
             ptt_active ? "TALKING" : "LISTENING");

    ui_clear(s_fb);

    // Caller ID / extension. A dialable string keeps the large F_NUM digits;
    // anything else (an alphanumeric caller ID, "Power off?") used to render
    // as blank gaps and is now F_UI text at 2x.
    if (caller_id && caller_id[0]) {
        if (all_num_glyphs(caller_id)) ui_draw_num(s_fb, 20, 40, caller_id, 6, 10);
        else ui_draw_text_centred(s_fb, 48, caller_id, 2);
    }

    // The status string itself, F_UI at 2x (#37). Until now it was only ever
    // string-matched into the pictogram below, so "Incoming Call", "Dialing"
    // and "Call Failed" were indistinguishable on the glass.
    if (status && status[0]) ui_draw_text_centred(s_fb, 104, status, 2);

    // Status pictogram: filled = active/ringing, outline = idle. Still the
    // string-matching placeholder; the band API (#38) replaces it with an
    // explicit screen enum.
    bool active = ptt_active || (status && strstr(status, "Call") != NULL) ||
                  (status && strstr(status, "Ring") != NULL);
    if (active) {
        ui_fill_rect(s_fb, EPD_WIDTH / 2 - 30, 168, 60, 60, true);
    } else {
        ui_fill_rect(s_fb, EPD_WIDTH / 2 - 30, 168, 60, 4, true);
        ui_fill_rect(s_fb, EPD_WIDTH / 2 - 30, 224, 60, 4, true);
        ui_fill_rect(s_fb, EPD_WIDTH / 2 - 30, 168, 4, 60, true);
        ui_fill_rect(s_fb, EPD_WIDTH / 2 + 26, 168, 4, 60, true);
    }

    epd_full_refresh(s_fb);
}

#if CONFIG_TDECK_MAX_EPD_BENCH
// Bench measurement for #43 / UI_DESIGN U8: partial-refresh wall time has
// never been measured on this panel. Runs once at boot, before the render
// task exists, so nothing else is on the SPI bus.
//
// What the panel shows while it runs (for whoever is watching the glass):
//   1. full refresh: a border and a solid bar at the bottom
//   2. 10 partials of rows 88-159 (the B_NUMBER band): a counter 1..10
//   3. 5 partials of the whole panel (Meshtastic's mode on this board):
//      a block that walks left to right along the bottom bar
//   4. one full refresh to finish, leaving the counter at 10
// Ghosting, smearing or a shifted band during 2-3 is the thing to look for.
static void bench_frame(int counter, int block)
{
    ui_clear(s_fb);
    ui_fill_rect(s_fb, 0, 0, EPD_WIDTH, 3, true);
    ui_fill_rect(s_fb, 0, EPD_HEIGHT - 3, EPD_WIDTH, 3, true);
    ui_fill_rect(s_fb, 0, 0, 3, EPD_HEIGHT, true);
    ui_fill_rect(s_fb, EPD_WIDTH - 3, 0, 3, EPD_HEIGHT, true);
    ui_fill_rect(s_fb, 20, 280, EPD_WIDTH - 40, 20, true);
    if (block >= 0) ui_fill_rect(s_fb, 24 + block * 40, 284, 32, 12, false);
    char num[8];
    snprintf(num, sizeof(num), "%d", counter);
    ui_draw_num(s_fb, 60, 100, num, 7, 8);   // inside rows 88-159
}

static void bench_report(const char *what, const int *ms, int n)
{
    int lo = 1 << 30, hi = -1;
    long sum = 0;
    int good = 0;
    for (int i = 0; i < n; i++) {
        if (ms[i] < 0) continue;
        good++;
        sum += ms[i];
        if (ms[i] < lo) lo = ms[i];
        if (ms[i] > hi) hi = ms[i];
    }
    if (good == 0) {
        ESP_LOGE(TAG, "epd bench %s: every refresh FAILED", what);
        return;
    }
    ESP_LOGW(TAG, "epd bench %s: n=%d ok=%d min=%d avg=%ld max=%d ms",
             what, n, good, lo, sum / good, hi);
}

void epaper_bench_run(void)
{
    ESP_LOGW(TAG, "=== E-PAPER BENCH (CONFIG_TDECK_MAX_EPD_BENCH) ===");
    int full_ms[2], band_ms[10], whole_ms[5];
    int full_wave[2], band_wave[10], whole_wave[5];

    bench_frame(0, -1);
    full_ms[0] = epd_refresh_rows(s_fb, 0, EPD_HEIGHT - 1, false);
    full_wave[0] = full_ms[0] < 0 ? -1 : s_last_waveform_ms;
    if (full_ms[0] >= 0) s_glass_valid = true;
    ESP_LOGW(TAG, "epd bench full[0]: %d ms (waveform %d)", full_ms[0], full_wave[0]);

    for (int i = 0; i < 10; i++) {
        bench_frame(i + 1, -1);
        band_ms[i] = s_glass_valid ? epd_refresh_rows(s_fb, 88, 159, true) : -1;
        band_wave[i] = band_ms[i] < 0 ? -1 : s_last_waveform_ms;
        ESP_LOGW(TAG, "epd bench partial rows 88-159 [%d]: %d ms (waveform %d)", i,
                 band_ms[i], band_wave[i]);
    }
    for (int i = 0; i < 5; i++) {
        bench_frame(10, i);
        whole_ms[i] = s_glass_valid ? epd_refresh_rows(s_fb, 0, EPD_HEIGHT - 1, true) : -1;
        whole_wave[i] = whole_ms[i] < 0 ? -1 : s_last_waveform_ms;
        ESP_LOGW(TAG, "epd bench partial rows 0-319 [%d]: %d ms (waveform %d)", i,
                 whole_ms[i], whole_wave[i]);
    }

    bench_frame(10, -1);
    full_ms[1] = epd_refresh_rows(s_fb, 0, EPD_HEIGHT - 1, false);
    full_wave[1] = full_ms[1] < 0 ? -1 : s_last_waveform_ms;
    if (full_ms[1] >= 0) s_glass_valid = true;

    bench_report("full (fast waveform), wall", full_ms, 2);
    bench_report("full (fast waveform), waveform only", full_wave, 2);
    bench_report("partial 72-row band, wall", band_ms, 10);
    bench_report("partial 72-row band, waveform only", band_wave, 10);
    bench_report("partial whole panel, wall", whole_ms, 5);
    bench_report("partial whole panel, waveform only", whole_wave, 5);
    ESP_LOGW(TAG, "=== E-PAPER BENCH COMPLETE ===");
}
#endif // CONFIG_TDECK_MAX_EPD_BENCH
