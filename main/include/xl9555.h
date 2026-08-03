#ifndef XL9555_H
#define XL9555_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the XL9555 expander over I2C
esp_err_t xl9555_init(void);

// Read Port 0 and Port 1 inputs
esp_err_t xl9555_read_ports(uint8_t *p0_val, uint8_t *p1_val);

// Write Port 0 and Port 1 outputs
esp_err_t xl9555_write_port0(uint8_t val);
esp_err_t xl9555_write_port1(uint8_t val);

// Convenience functions
esp_err_t xl9555_set_speaker_amp(bool enable);
esp_err_t xl9555_set_4g_power(bool enable);
esp_err_t xl9555_set_audio_route(bool use_4g); // false = ES8311, true = A7682E
esp_err_t xl9555_set_motor_enable(bool enable);
esp_err_t xl9555_reset_touch(void);
esp_err_t xl9555_reset_keyboard(void);

#ifdef __cplusplus
}
#endif

#endif // XL9555_H
