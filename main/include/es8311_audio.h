#ifndef ES8311_AUDIO_H
#define ES8311_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bench diagnostics (see CONFIG_TDECK_MAX_AUDIO_SELFTEST).
// Read back the ES8311 registers that can silently kill audio and check each
// against what the driver should have written. Returns the number of FAILs.
int audio_hardware_check_codec_regs(void);

// Probe whether the codec drives ASDOUT, and whether BCLK is running.
void audio_hardware_probe_asdout_activity(void);

// Decide, electrically, which of DSDIN/ASDOUT the codec actually drives.
// Applies an internal pull-up then a pull-down to each data pin and reports
// whether the pin followed the pull (nothing driving it) or ignored it
// (something is). See the implementation for how to read the result.
void audio_hardware_probe_pin_drive(void);

// Swap the I2S data pins (dout <-> din) at runtime so a single boot can test
// both orientations. `swapped == false` restores the datasheet-named wiring
// (dout = DSDIN/17, din = ASDOUT/40).
esp_err_t audio_hardware_set_pins_swapped(bool swapped);

// Initialize I2S peripheral and ES8311 audio codec at specified sample rate (8000 Hz or 16000 Hz)
esp_err_t audio_hardware_init(uint32_t sample_rate);

// The rate the codec was actually opened at. Callers that synthesise or
// consume audio must use this rather than assuming POC_SAMPLE_RATE_HZ --
// generating a tone for one rate and playing it at another skews both pitch
// and duration, which cost us a bench session.
uint32_t audio_hardware_sample_rate(void);

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
