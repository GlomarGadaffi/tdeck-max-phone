#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connect as a Wi-Fi station and BLOCK until an IPv4 lease is obtained.
esp_err_t wifi_sta_connect(const char *ssid, const char *pass);

// Our DHCP-assigned address as a dotted string. Valid after wifi_sta_connect().
const char *wifi_local_ip(void);

#ifdef __cplusplus
}
#endif
