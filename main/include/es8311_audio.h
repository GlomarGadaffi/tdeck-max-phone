#ifndef ES8311_AUDIO_H
#define ES8311_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bench diagnostics (see CONFIG_TDECK_MAX_AUDIO_SELFTEST).
// Dump key ES8311 registers over I2C so we can tell "codec never configured"
// from "codec fine, wrong I2S slot" -- the two produce identical silence.
void audio_hardware_dump_codec_regs(void);

// Reconfigure ONLY the RX channel's slot mask and re-measure the mic.
// mask: 0 = left, 1 = right, 2 = both. Returns peak sample magnitude.
int audio_hardware_probe_mic_slot(int mask);

// Probe whether the codec drives ASDOUT, and whether BCLK is running.
void audio_hardware_probe_asdout_activity(void);

// Initialize I2S peripheral and ES8311 audio codec at specified sample rate (8000 Hz or 16000 Hz)
esp_err_t audio_hardware_init(uint32_t sample_rate);

// Enable/disable speaker output amplifier
void audio_hardware_set_amp(bool enable);

// Read microphone PCM samples (16-bit mono)
size_t audio_hardware_read_mic(int16_t *buf, size_t samples);

// Write speaker PCM samples (16-bit mono)
size_t audio_hardware_write_spk(const int16_t *buf, size_t samples);

#ifdef __cplusplus
}
#endif

#endif // ES8311_AUDIO_H
