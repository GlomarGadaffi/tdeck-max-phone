#ifndef ES8311_AUDIO_H
#define ES8311_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
