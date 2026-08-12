#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connect as a Wi-Fi station and block until an IPv4 lease is obtained, or
// ESP_ERR_TIMEOUT after 30 s. The driver keeps retrying in the background
// after a timeout, so callers may continue and poll wifi_is_connected().
esp_err_t wifi_sta_connect(const char *ssid, const char *pass);

// True once a DHCP lease has been obtained.
bool wifi_is_connected(void);

// Our DHCP-assigned address as a dotted string. Valid after wifi_sta_connect().
const char *wifi_local_ip(void);

#ifdef __cplusplus
}
#endif
