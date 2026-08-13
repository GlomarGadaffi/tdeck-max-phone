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
    // REG44 is "GPIO, dac2adc for test" -- an internal DAC->ADC loopback.
    // The vendor driver gates it on cfg.no_dac_ref: 0x58 routes the DAC as
    // the ADC's reference (their no_dac_ref == false branch), 0x08 leaves
    // the real analog input connected.
    //
    // This was 0x58, copied unconditionally without carrying the flag over.
    // The effect is that the ADC listens to the DAC instead of the
    // microphone -- so capture returns exactly what the DAC is playing,
    // which during a silent moment is bit-exact zero. That matched the
    // observed symptom precisely: registers all correct, every I2S slot
    // mask reading zero. This board has a real differential analog mic on
    // MIC1P/MIC1N, so we want the input connected: 0x08.
    ret |= es8311_write_reg(ES8311_GPIO_REG44, 0x08);

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
    // REG0E powers the analog capture blocks. Reset default is 0x6A, with
    // PDN_PGA (bit 6) and PDN_MOD (bit 5) SET = powered down. We must clear
    // exactly those two and leave the rest of the default alone -> 0x0A.
    //
    // This was 0x02, copied from LilyGO's esp_codec_dev driver. That value
    // additionally clears bit 3, which the reset default sets. Their only
    // ES8311 examples (playFormSD, playWAV) are playback-only, so their
    // capture path was never exercised on this board and 0x02 was never
    // validated for recording. Confirmed on hardware: with 0x02 the ADC
    // returned bit-exact zero on every I2S slot mask (left/right/both)
    // while all other registers read back correct.
    ret |= es8311_write_reg(ES8311_SYSTEM_REG0E, 0x0A);
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

// ── Bench diagnostics ───────────────────────────────────────────────────────
// A dead ES8311 ADC and a wrong I2S slot mask both produce bit-exact silence,
// so these two helpers exist to tell them apart on hardware.

void audio_hardware_dump_codec_regs(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGW(TAG, "[sim] codec register dump skipped");
#else
    // If these read back as written, the codec accepted our init and the
    // fault is downstream (I2S slot/format). If they read 0x00/0xFF, the
    // I2C writes didn't stick and the codec was never really configured.
    struct { uint8_t reg; const char *name; uint8_t expect; } regs[] = {
        {ES8311_RESET_REG00,       "RESET/mode",      0x80},
        {ES8311_CLK_MANAGER_REG01, "clk src",         0x3F},
        {ES8311_SDPIN_REG09,       "DAC serial fmt",  0x0C},
        {ES8311_SDPOUT_REG0A,      "ADC serial fmt",  0x0C},
        {ES8311_SYSTEM_REG0D,      "power up",        0x01},
        {ES8311_SYSTEM_REG0E,      "ADC/PGA power",   0x0A},
        {ES8311_SYSTEM_REG12,      "DAC enable",      0x00},
        {ES8311_SYSTEM_REG14,      "mic sel/PGA",     0x1A},
        {ES8311_ADC_REG15,         "ADC ramp",        0x40},
        {ES8311_ADC_REG16,         "mic gain",        0x04},
        {ES8311_ADC_REG17,         "ADC volume",      0xBF},
        {ES8311_DAC_REG31,         "DAC mute",        0x00},
        {ES8311_DAC_REG32,         "DAC volume",      0xBF},
    };
    // Full sweep first: the targeted table below only checks what we wrote,
    // which can't reveal a register we never touched holding a bad default.
    ESP_LOGW(TAG, "--- ES8311 FULL DUMP 0x00-0x1C, 0x31-0x37, 0x44-0x45 ---");
    {
        char line[96];
        int n = 0;
        for (uint8_t r = 0x00; r <= 0x1C; r++) {
            uint8_t v = 0;
            if (es8311_read_reg(r, &v) != ESP_OK) v = 0xEE;
            n += snprintf(line + n, sizeof(line) - n, "%02x:%02x ", r, v);
            if ((r % 8) == 7 || r == 0x1C) { ESP_LOGW(TAG, "  %s", line); n = 0; line[0] = 0; }
        }
        n = 0; line[0] = 0;
        for (uint8_t r = 0x31; r <= 0x37; r++) {
            uint8_t v = 0;
            if (es8311_read_reg(r, &v) != ESP_OK) v = 0xEE;
            n += snprintf(line + n, sizeof(line) - n, "%02x:%02x ", r, v);
        }
        for (uint8_t r = 0x44; r <= 0x45; r++) {
            uint8_t v = 0;
            if (es8311_read_reg(r, &v) != ESP_OK) v = 0xEE;
            n += snprintf(line + n, sizeof(line) - n, "%02x:%02x ", r, v);
        }
        ESP_LOGW(TAG, "  %s", line);
        uint8_t id1 = 0, id2 = 0, ver = 0;
        es8311_read_reg(0xFD, &id1); es8311_read_reg(0xFE, &id2); es8311_read_reg(0xFF, &ver);
        ESP_LOGW(TAG, "  CHIPID1=0x%02x CHIPID2=0x%02x VER=0x%02x (expect 83/11/xx)", id1, id2, ver);
    }

    ESP_LOGW(TAG, "--- ES8311 register readback (addr 0x%02x) ---", BOARD_ES8311_I2C_ADDR);
    int mismatches = 0, failures = 0;
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        uint8_t v = 0;
        esp_err_t err = es8311_read_reg(regs[i].reg, &v);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  0x%02x %-16s READ FAILED (%d)", regs[i].reg, regs[i].name, err);
            failures++;
            continue;
        }
        bool ok = (v == regs[i].expect);
        if (!ok) mismatches++;
        ESP_LOGW(TAG, "  0x%02x %-16s = 0x%02x (wrote 0x%02x) %s",
                 regs[i].reg, regs[i].name, v, regs[i].expect, ok ? "" : "  <-- MISMATCH");
    }
    if (failures) {
        ESP_LOGE(TAG, "--- %d register READS failed: codec not responding on I2C ---", failures);
    } else if (mismatches) {
        ESP_LOGE(TAG, "--- %d registers differ: init did NOT take ---", mismatches);
    } else {
        ESP_LOGW(TAG, "--- all registers match: codec IS configured; look at I2S slot/format ---");
    }
#endif
}

int audio_hardware_probe_mic_slot(int mask)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)mask; return -1;
#else
    if (rx_chan == NULL) return -1;

    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    const char *name = "left";
    if (mask == 1)      { slot.slot_mask = I2S_STD_SLOT_RIGHT; name = "right"; }
    else if (mask == 2) { slot.slot_mask = I2S_STD_SLOT_BOTH;  name = "both";
                          slot.slot_mode = I2S_SLOT_MODE_STEREO; }
    else                { slot.slot_mask = I2S_STD_SLOT_LEFT; }

    // Reconfiguring slots requires the channel disabled.
    i2s_channel_disable(rx_chan);
    esp_err_t err = i2s_channel_reconfig_std_slot(rx_chan, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  slot=%-5s reconfig failed (%d)", name, err);
        i2s_channel_enable(rx_chan);
        return -1;
    }
    i2s_channel_enable(rx_chan);

    // Discard the first buffers so we measure steady state, not startup.
    int16_t buf[256];
    for (int i = 0; i < 5; i++) audio_hardware_read_mic(buf, 256);

    int32_t peak = 0;
    size_t total = 0;
    for (int f = 0; f < 40; f++) {
        size_t got = audio_hardware_read_mic(buf, 256);
        for (size_t i = 0; i < got; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        total += got;
    }
    ESP_LOGW(TAG, "  slot=%-5s peak=%-6d samples=%u", name, (int)peak, (unsigned)total);
    return (int)peak;
#endif
}

// Is the codec driving its data-out line at all? I2S owning the pin doesn't
// stop us reading the pad's input level. Constant level across a burst of
// samples means the ES8311 is not transmitting; observed transitions mean it
// is, and the fault is on the ESP32 receive side instead.
void audio_hardware_probe_asdout_activity(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGW(TAG, "[sim] ASDOUT probe skipped");
#else
    int hi = 0, lo = 0, edges = 0, last = -1;
    for (int i = 0; i < 20000; i++) {
        int v = gpio_get_level(BOARD_I2S_ASDOUT);
        if (v) hi++; else lo++;
        if (last >= 0 && v != last) edges++;
        last = v;
    }
    ESP_LOGW(TAG, "ASDOUT(GPIO%d): hi=%d lo=%d edges=%d -> %s",
             BOARD_I2S_ASDOUT, hi, lo, edges,
             edges > 0 ? "CODEC IS DRIVING DATA" : "line static: codec NOT transmitting");

    // MCLK is the one the ES8311's clock manager actually needs. Without it
    // the chip is inert in BOTH directions while I2C still works perfectly
    // -- which is exactly the symptom here.
    int mh = 0, ml = 0, medges = 0; last = -1;
    for (int i = 0; i < 20000; i++) {
        int v = gpio_get_level(BOARD_I2S_MCLK);
        if (v) mh++; else ml++;
        if (last >= 0 && v != last) medges++;
        last = v;
    }
    ESP_LOGW(TAG, "MCLK(GPIO%d):   hi=%d lo=%d edges=%d -> %s",
             BOARD_I2S_MCLK, mh, ml, medges,
             medges > 0 ? "MCLK PRESENT" : "NO MCLK -- codec has no master clock!");

    // WS/LRCK: without frame sync the codec never frames a sample in either
    // direction. At 8 kHz this is ~32x slower than BCLK, so expect few edges.
    int wh = 0, wl = 0, wedges = 0; last = -1;
    for (int i = 0; i < 20000; i++) {
        int v = gpio_get_level(BOARD_I2S_LRCK);
        if (v) wh++; else wl++;
        if (last >= 0 && v != last) wedges++;
        last = v;
    }
    ESP_LOGW(TAG, "WS/LRCK(GPIO%d): hi=%d lo=%d edges=%d -> %s",
             BOARD_I2S_LRCK, wh, wl, wedges,
             wedges > 0 ? "frame sync running" : "NO WS -- codec never frames a sample!");

    // DSDIN: our data OUT to the codec. If this is static while we are
    // playing a tone, the ESP32 is not actually shifting samples out.
    int dh = 0, dl = 0, dedges = 0; last = -1;
    for (int i = 0; i < 20000; i++) {
        int v = gpio_get_level(BOARD_I2S_DSDIN);
        if (v) dh++; else dl++;
        if (last >= 0 && v != last) dedges++;
        last = v;
    }
    ESP_LOGW(TAG, "DSDIN(GPIO%d):  hi=%d lo=%d edges=%d -> %s",
             BOARD_I2S_DSDIN, dh, dl, dedges,
             dedges > 0 ? "ESP32 is shifting data out" : "static (expected if playing silence)");

    int bh = 0, bl = 0, bedges = 0; last = -1;
    for (int i = 0; i < 20000; i++) {
        int v = gpio_get_level(BOARD_I2S_SCLK);
        if (v) bh++; else bl++;
        if (last >= 0 && v != last) bedges++;
        last = v;
    }
    ESP_LOGW(TAG, "BCLK(GPIO%d):   hi=%d lo=%d edges=%d -> %s",
             BOARD_I2S_SCLK, bh, bl, bedges,
             bedges > 0 ? "clock running" : "NO CLOCK - I2S not driving the bus");
#endif
}
