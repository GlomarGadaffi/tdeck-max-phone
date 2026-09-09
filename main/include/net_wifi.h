// Core connection-manager API ported verbatim from tdeck-max-phone
// (main/include/net_wifi.h). Scan/RSSI/SSID additions below are new for
// this project's WiFi wizard (see wifi_wizard.c) and diagnostics view.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

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

// ── New for this project: scan-before-connect + diagnostics ────────────────
//
// wifi_sta_connect()'s STA_START handler used to unconditionally call
// esp_wifi_connect(), which is wrong for a wizard that needs to scan for
// APs before any SSID/password exists. wifi_scan_start() now shares the
// same idempotent driver bring-up as wifi_sta_connect() but does NOT
// request a connect; wifi_sta_connect() explicitly requests one before
// starting the driver. See net_wifi.c for the shared bring-up helper.

// Bring the WiFi driver up in STA mode (idempotent) and run a blocking
// active scan. Safe to call before any SSID/password is known -- does not
// attempt to associate. Returns the number of APs found (possibly 0), or
// -1 on error.
int wifi_scan_start(void);

// Copy up to max_results scan records (sorted strongest-first by the
// driver) into out. Call only after a successful wifi_scan_start(). Returns
// the number of records actually copied.
uint16_t wifi_scan_get_results(wifi_ap_record_t *out, uint16_t max_results);

// Current RSSI in dBm, or 0 if not connected. For the diagnostics view.
int8_t wifi_get_rssi(void);

// SSID of the network we're connected to (or attempting), "" if none.
const char *wifi_get_ssid(void);

// ── New for the Settings app ───────────────────────────────────────────────

// Same as wifi_sta_connect() but returns immediately after kicking the
// driver -- poll wifi_is_connected(). For callers on a watchdog-monitored
// task (the LVGL pump) that can't block 30s.
esp_err_t wifi_sta_connect_start(const char *ssid, const char *pass);

// Static IPv4 config, remembered for every driver bring-up and applied to
// the STA netif immediately if it already exists. ip "" (or NULL) = DHCP.
// mask/gw may be "" (mask defaults to 255.255.255.0, gw to none).
void wifi_set_static_ip(const char *ip, const char *mask, const char *gw);

#ifdef __cplusplus
}
#endif
