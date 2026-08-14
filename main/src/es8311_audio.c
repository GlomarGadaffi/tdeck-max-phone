// ES8311 audio via Espressif's own esp_codec_dev component.
//
// This file previously hand-transcribed the codec init from LilyGO's vendored
// copy of that same component. That was a mistake: the driver is parameterised
// by es8311_codec_cfg_t, and transcribing its code paths without the flags
// that select between them produced a codec that reported every register
// correct while being inert in both directions. The clearest example was
// REG44, the DAC->ADC test loopback, which the driver gates on `no_dac_ref`
// and which we wrote unconditionally -- pointing the ADC at the DAC.
//
// esp_codec_dev is an Espressif IDF component (from ESP-ADF), not an Arduino
// library: it declares `idf: >=4.0` and installs via the component manager.
// We drive it with an explicit config instead of reimplementing it.
#include "es8311_audio.h"
#include "board_tdeck_max.h"
#include "poc_config.h"
#include "xl9555.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "es8311_codec.h"

static const char *TAG = "ES8311_AUDIO";

static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;

// Everything below except s_sample_rate is referenced only from the real
// hardware paths (each is inside a `#else` of a CONFIG_TDECK_MAX_SIM_MODE
// guard), so defining them unconditionally gave eight -Wunused-variable
// warnings in the QEMU sim build. That quietly falsified the zero-warning
// claim in docs/BENCH_TEST.md, since the sim config is only built when
// someone explicitly asks for it.
#if !CONFIG_TDECK_MAX_SIM_MODE
static const audio_codec_data_if_t *s_data_if = NULL;
static const audio_codec_ctrl_if_t *s_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_gpio_if = NULL;
static const audio_codec_if_t      *s_codec_if = NULL;
static esp_codec_dev_handle_t       s_dev = NULL;
static bool                         s_pins_swapped = false;
#endif

// Reported by audio_hardware_sample_rate() on both paths, so it is not
// guarded -- callers that synthesise audio must not assume POC_SAMPLE_RATE_HZ.
static uint32_t                     s_sample_rate = 0;

// The codec device is opened 2-channel, so wire traffic is interleaved
// stereo while our SIP path is mono. Scratch buffers do the conversion
// without allocating per frame.
#define AUDIO_STEREO_SCRATCH 512
#if !CONFIG_TDECK_MAX_SIM_MODE
static int16_t s_stereo_rx[AUDIO_STEREO_SCRATCH];
static int16_t s_stereo_tx[AUDIO_STEREO_SCRATCH];
#endif

uint32_t audio_hardware_sample_rate(void) { return s_sample_rate; }

esp_err_t audio_hardware_init(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Initializing ES8311 via esp_codec_dev at %lu Hz...", sample_rate);

    // 1. I2S channels. esp_codec_dev consumes the handles we create; it does
    //    not install the driver itself. Full duplex on one port so TX and RX
    //    share the clock generator (the codec has one BCLK/WS pair).
    s_sample_rate = sample_rate;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // zero the DMA buffer on underrun instead of looping stale audio
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    // NOTE: slot_cfg here is essentially decorative. esp_codec_dev_open()
    // calls i2s_channel_reconfig_std_slot() + _std_clock() on both channels
    // (managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c,
    // set_drv_fs()) and rebuilds the slot config from the sample info we
    // pass to esp_codec_dev_open(). With channel = 2 it forces STEREO slots
    // and slot_mask = I2S_STD_SLOT_BOTH no matter what we put here. Two
    // bench sessions were spent toggling MONO/STEREO on this line before
    // reading that function. The values below are kept consistent with what
    // the driver will impose so the config isn't misleading.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_SCLK,
            .ws   = BOARD_I2S_LRCK,
            // GPIO40 out / GPIO17 in -- the opposite of what the vendor's
            // ASDOUT/DSDIN names imply. See board_tdeck_max.h for the
            // measurements that settled it.
            .dout = BOARD_I2S_DOUT,
            .din  = BOARD_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // must match es_cfg.mclk_div below
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    // Route analog audio to the local codec rather than the A7682E.
    xl9555_set_audio_route(false);

#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGD(TAG, "[sim] esp_codec_dev stack skipped");
    return ESP_OK;
#else
    // 2. Control interface over the already-installed legacy I2C bus.
    //    CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE keeps the component on the
    //    legacy API so we don't need the new driver's bus handle here.
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = BOARD_ES8311_I2C_ADDR << 1,   // component expects the shifted 8-bit form
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == NULL) { ESP_LOGE(TAG, "i2c ctrl if failed"); return ESP_FAIL; }

    // 3. Data interface bound to our I2S handles.
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_I2S_NUM,
        .rx_handle = rx_chan,
        .tx_handle = tx_chan,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL) { ESP_LOGE(TAG, "i2s data if failed"); return ESP_FAIL; }

    s_gpio_if = audio_codec_new_gpio();

    // 4. The codec itself. These are the flags whose absence caused the
    //    hand-rolled version to fail.
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = s_ctrl_if,
        .gpio_if     = s_gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,  // we need ADC and DAC
        .pa_pin      = -1,        // amp sits on the XL9555, handled separately
        .master_mode = false,     // ESP32 drives BCLK/WS, so codec is slave
        .use_mclk    = true,      // MCLK is wired on BOARD_I2S_MCLK
        .digital_mic = false,     // analog differential mic on MIC1P/MIC1N
        .invert_mclk = false,
        .invert_sclk = false,
        .no_dac_ref  = true,      // do NOT feed the DAC back into capture
        .mclk_div    = 256,       // matches I2S_MCLK_MULTIPLE_256
        // Left zeroed, esp_codec_dev_col_calc_hw_gain() substitutes exactly
        // these defaults (esp_codec_dev_vol.c). Stated explicitly so a diff
        // against IDF's own i2s_es8311 example shows no phantom difference.
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (s_codec_if == NULL) { ESP_LOGE(TAG, "es8311_codec_new failed"); return ESP_FAIL; }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); return ESP_FAIL; }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .channel_mask    = 0x03,   // both slots; the driver would infer this anyway
        .sample_rate     = sample_rate,
    };
    int rc = esp_codec_dev_open(s_dev, &fs);
    if (rc != 0) { ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", rc); return ESP_FAIL; }

    // These two are not optional garnish. esp_codec_dev_open() finishes by
    // calling _update_codec_setting(), which pushes its *initial* volume --
    // zero, straight from calloc -- through the curve to -96 dB and writes
    // REG32 = 0x00. That is a hard DAC mute. If the call below fails or is
    // skipped the codec stays muted, the amp still clicks on enable, and the
    // board looks exactly like a dead speaker. Hence the return checks.
    // Raise the ceiling before setting the volume. esp_codec_dev's built-in
    // curve maps volume 100 to 0 dB, so the top of the user's knob left the
    // ES8311 well short of what it can do (REG32 goes to +32 dB). This curve
    // must be installed before set_out_vol(), since that is what evaluates it.
    esp_codec_dev_vol_map_t vol_map[2] = {
        { .vol = 0,   .db_value = -50.0f },
        { .vol = 100, .db_value = POC_SPK_MAX_DB },
    };
    esp_codec_dev_vol_curve_t curve = { .vol_map = vol_map, .count = 2 };
    rc = esp_codec_dev_set_vol_curve(s_dev, &curve);
    if (rc != 0) ESP_LOGW(TAG, "set_vol_curve failed: %d -- falling back to 0 dB ceiling", rc);

    rc = esp_codec_dev_set_out_vol(s_dev, POC_SPK_VOLUME);
    if (rc != 0) ESP_LOGE(TAG, "set_out_vol failed: %d -- DAC IS STILL MUTED", rc);
    rc = esp_codec_dev_set_in_gain(s_dev, POC_MIC_GAIN_DB);
    if (rc != 0) ESP_LOGE(TAG, "set_in_gain failed: %d -- mic PGA at 0 dB", rc);

    int v32 = 0;
    esp_codec_dev_read_reg(s_dev, 0x32, &v32);
    ESP_LOGI(TAG, "gain staging: spk %d/100 (ceiling %+.0f dB) -> REG32 0x%02X = %+.1f dB, "
                  "mic PGA %.0f dB",
             POC_SPK_VOLUME, (double)POC_SPK_MAX_DB, v32 & 0xFF,
             (double)((v32 & 0xFF) * 0.5f - 95.5f), (double)POC_MIC_GAIN_DB);

    ESP_LOGI(TAG, "Audio hardware initialized successfully (esp_codec_dev) @ %lu Hz", sample_rate);
    return ESP_OK;
#endif
}

esp_err_t audio_hardware_set_pins_swapped(bool swapped)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)swapped; return ESP_OK;
#else
    if (tx_chan == NULL || rx_chan == NULL) return ESP_ERR_INVALID_STATE;

    i2s_std_gpio_config_t gpio_cfg = {
        .mclk = BOARD_I2S_MCLK,
        .bclk = BOARD_I2S_SCLK,
        .ws   = BOARD_I2S_LRCK,
        .dout = swapped ? BOARD_I2S_DIN  : BOARD_I2S_DOUT,
        .din  = swapped ? BOARD_I2S_DOUT : BOARD_I2S_DIN,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    };

    // The channels must be stopped before the GPIO matrix can be re-pointed.
    ESP_ERROR_CHECK(i2s_channel_disable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_disable(rx_chan));
    esp_err_t err = i2s_channel_reconfig_std_gpio(tx_chan, &gpio_cfg);
    if (err == ESP_OK) err = i2s_channel_reconfig_std_gpio(rx_chan, &gpio_cfg);
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    if (err == ESP_OK) {
        s_pins_swapped = swapped;
        ESP_LOGW(TAG, "I2S data pins now: dout=GPIO%d din=GPIO%d (%s)",
                 gpio_cfg.dout, gpio_cfg.din,
                 swapped ? "SWAPPED / LilyGO setPins order" : "datasheet naming");
    } else {
        ESP_LOGE(TAG, "pin reconfig failed: %s", esp_err_to_name(err));
    }
    return err;
#endif
}

void audio_hardware_set_amp(bool enable)
{
    // pa_pin is -1 in the codec config: the amplifier enable is an XL9555
    // expander pin, not an ESP32 GPIO, so the codec driver can't drive it.
    xl9555_set_speaker_amp(enable);
}

size_t audio_hardware_read_mic(int16_t *buf, size_t samples)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)buf; (void)samples; return 0;
#else
    if (s_dev == NULL) return 0;
    // The device is opened 2-channel (matching LilyGO's working config), so
    // the codec delivers interleaved stereo frames. Our callers are mono
    // G.711, so read 2x and keep the left channel.
    if (samples > AUDIO_STEREO_SCRATCH / 2) samples = AUDIO_STEREO_SCRATCH / 2;
    int bytes = (int)(samples * 2 * sizeof(int16_t));
    if (esp_codec_dev_read(s_dev, s_stereo_rx, bytes) != 0) return 0;
    for (size_t i = 0; i < samples; i++) buf[i] = s_stereo_rx[2 * i];
    return samples;
#endif
}

size_t audio_hardware_write_spk(const int16_t *buf, size_t samples)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    (void)buf; (void)samples; return samples;
#else
    if (s_dev == NULL) return 0;
    // Duplicate mono into both channels of an interleaved stereo frame.
    if (samples > AUDIO_STEREO_SCRATCH / 2) samples = AUDIO_STEREO_SCRATCH / 2;
    for (size_t i = 0; i < samples; i++) {
        s_stereo_tx[2 * i]     = buf[i];
        s_stereo_tx[2 * i + 1] = buf[i];
    }
    int bytes = (int)(samples * 2 * sizeof(int16_t));
    if (esp_codec_dev_write(s_dev, s_stereo_tx, bytes) != 0) return 0;
    return samples;
#endif
}

// ── Bench diagnostics ───────────────────────────────────────────────────────

// Every one of these can silently produce total silence while the codec
// answers on I2C and reports a valid chip ID -- which is the exact failure
// we have been staring at. `mask` restricts the comparison to the bits that
// matter (0xFF = whole byte).
int audio_hardware_check_codec_regs(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGW(TAG, "[sim] codec register check skipped");
    return 0;
#else
    if (s_dev == NULL) { ESP_LOGW(TAG, "codec not open, skipping check"); return -1; }

    struct { int reg; int expect; int mask; const char *what; } checks[] = {
        {0x00, 0x80, 0xC0, "RESET: powered, slave mode"},
        {0x01, 0x3F, 0xC0, "CLK: MCLK from pad, not inverted"},
        {0x09, 0x0C, 0x4F, "SDPIN: 16-bit, DAC path enabled"},
        {0x0A, 0x0C, 0x4F, "SDPOUT: 16-bit, ADC path enabled"},
        {0x0D, 0x01, 0xFF, "SYSTEM: analog powered up"},
        {0x0E, 0x02, 0xFF, "SYSTEM: ADC/PGA powered"},
        {0x12, 0x00, 0xFF, "SYSTEM: DAC powered up"},
        {0x14, 0x1A, 0xFF, "SYSTEM: analog mic selected (not DMIC)"},
        // REG16 holds the PGA step index, not dB: 0=0dB, 1=6dB ... 7=42dB.
        {0x16, (int)(POC_MIC_GAIN_DB / 6.0f), 0xFF, "ADC: mic PGA = POC_MIC_GAIN_DB"},
        {0x17, 0xBF, 0xFF, "ADC: digital volume 0 dB"},
        {0x44, 0x08, 0xFF, "GPIO: no DAC->ADC reference"},
    };

    int fails = 0;
    ESP_LOGW(TAG, "--- ES8311 register check ---");
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        int v = 0;
        if (esp_codec_dev_read_reg(s_dev, checks[i].reg, &v) != 0) {
            ESP_LOGE(TAG, "  REG%02X  READ FAILED          %s", checks[i].reg, checks[i].what);
            fails++;
            continue;
        }
        bool ok = ((v & checks[i].mask) == (checks[i].expect & checks[i].mask));
        if (!ok) fails++;
        ESP_LOGW(TAG, "  REG%02X = 0x%02X  want 0x%02X/%02X  %s  %s",
                 checks[i].reg, v & 0xFF, checks[i].expect, checks[i].mask,
                 ok ? "ok  " : "FAIL", checks[i].what);
    }

    // REG32 (DAC volume) and REG31 (DAC mute) get their own treatment: 0x00
    // in REG32 is -95.5 dB, i.e. a mute that looks identical to broken
    // hardware. esp_codec_dev_open() leaves it there unless set_out_vol()
    // lands afterwards.
    int v32 = 0, v31 = 0;
    esp_codec_dev_read_reg(s_dev, 0x32, &v32);
    esp_codec_dev_read_reg(s_dev, 0x31, &v31);
    if ((v32 & 0xFF) == 0x00) {
        ESP_LOGE(TAG, "  REG32 = 0x00   DAC IS MUTED (-95.5 dB) -- set_out_vol never landed");
        fails++;
    } else {
        ESP_LOGW(TAG, "  REG32 = 0x%02X  ok   DAC volume (~%d dB)",
                 v32 & 0xFF, (int)((v32 & 0xFF) * 0.5f - 95.5f));
    }
    if ((v31 & 0x60) != 0) {
        ESP_LOGE(TAG, "  REG31 = 0x%02X  FAIL DAC mute bits set", v31 & 0xFF);
        fails++;
    } else {
        ESP_LOGW(TAG, "  REG31 = 0x%02X  ok   DAC unmuted", v31 & 0xFF);
    }

    // Clock dividers, informational -- these depend on the open rate.
    char line[96]; int n = 0;
    for (int r = 0x02; r <= 0x08; r++) {
        int v = 0;
        if (esp_codec_dev_read_reg(s_dev, r, &v) != 0) v = 0xEE;
        n += snprintf(line + n, sizeof(line) - n, "%02x:%02x ", r, v & 0xFF);
    }
    ESP_LOGW(TAG, "  clock dividers @ %lu Hz: %s", s_sample_rate, line);

    ESP_LOGW(TAG, "--- register check: %d FAIL(s) ---", fails);
    return fails;
#endif
}

// Which of the two data pins is actually driven?
//
// The IO-pad pull resistors are independent of the GPIO matrix, so we can
// apply them without detaching the I2S peripheral. A pin something is
// actively driving (~tens of ohms) ignores a ~45k internal pull; a pin
// nothing drives follows it.
//
// GPIO17 is the control: we drive it, so it MUST ignore the pull. If it
// doesn't, the method is invalid and the GPIO40 result means nothing.
//
// Caveat: an external pull-up/down on the net would also make a floating
// pin read as "driven". Treat this as corroboration, not proof -- the
// swap test in audio_selftest() is the ground truth.
void audio_hardware_probe_pin_drive(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGW(TAG, "[sim] pin drive probe skipped");
#else
    struct { int pin; const char *name; } pins[] = {
        {BOARD_I2S_DOUT, "GPIO40 esp->codec (we drive)"},
        {BOARD_I2S_DIN,  "GPIO17 codec->esp (codec drives)"},
    };
    ESP_LOGW(TAG, "--- pin drive probe (pull-up vs pull-down) ---");
    for (size_t p = 0; p < sizeof(pins)/sizeof(pins[0]); p++) {
        int up = 0, dn = 0;
        gpio_set_pull_mode(pins[p].pin, GPIO_PULLUP_ONLY);
        for (volatile int d = 0; d < 20000; d++) { }
        for (int i = 0; i < 1000; i++) up += gpio_get_level(pins[p].pin);
        gpio_set_pull_mode(pins[p].pin, GPIO_PULLDOWN_ONLY);
        for (volatile int d = 0; d < 20000; d++) { }
        for (int i = 0; i < 1000; i++) dn += gpio_get_level(pins[p].pin);
        gpio_set_pull_mode(pins[p].pin, GPIO_FLOATING);

        // "Followed the pull" = high with pull-up, low with pull-down.
        bool followed = (up > 900 && dn < 100);
        ESP_LOGW(TAG, "  %-32s pullup_high=%4d/1000 pulldown_high=%4d/1000  %s",
                 pins[p].name, up, dn,
                 followed ? "FLOATING - nothing drives this pin"
                          : "DRIVEN - something is holding it");
    }
#endif
}

void audio_hardware_probe_asdout_activity(void)
{
#if CONFIG_TDECK_MAX_SIM_MODE
    ESP_LOGW(TAG, "[sim] pin probes skipped");
#else
    struct { int pin; const char *name; } pins[] = {
        {BOARD_I2S_DIN,    "DIN (codec->esp)"},
        {BOARD_I2S_MCLK,   "MCLK"},
        {BOARD_I2S_LRCK,   "WS/LRCK"},
        {BOARD_I2S_DOUT,   "DOUT(esp->codec)"},
        {BOARD_I2S_SCLK,   "BCLK"},
    };
    for (size_t p = 0; p < sizeof(pins)/sizeof(pins[0]); p++) {
        int hi = 0, lo = 0, edges = 0, last = -1;
        for (int i = 0; i < 20000; i++) {
            int v = gpio_get_level(pins[p].pin);
            if (v) hi++; else lo++;
            if (last >= 0 && v != last) edges++;
            last = v;
        }
        ESP_LOGW(TAG, "%-20s GPIO%-2d hi=%-6d lo=%-6d edges=%-6d %s",
                 pins[p].name, pins[p].pin, hi, lo, edges,
                 edges > 0 ? "ACTIVE" : "static");
    }
#endif
}
