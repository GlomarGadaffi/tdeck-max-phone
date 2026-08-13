#ifndef PMU_SY6970_H
#define PMU_SY6970_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SY6970 charger / power-path controller (I2C 0x6A).
//
// Only the shutdown path is implemented -- charging and fuel gauging are the
// BQ27220's job and nothing in this firmware needs them yet.

// True if USB/VBUS is supplying the board. Matters because shutdown() cannot
// work while it is: the BATFET only gates the *battery* path.
esp_err_t pmu_sy6970_vbus_present(bool *present);

// Real power off: force the battery FET open (REG09 bit 5, BATFET_DIS), the
// same thing LilyGO's factory firmware does via XPowersLib's PPM.shutdown().
//
// Returns ESP_OK if the register write landed -- which is NOT the same as the
// board actually powering down. On USB the system keeps running from VBUS.
// The only ways back on are the PWR button or reconnecting power.
esp_err_t pmu_sy6970_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // PMU_SY6970_H
