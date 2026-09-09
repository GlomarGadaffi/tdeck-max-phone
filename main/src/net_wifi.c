// Connection-manager core ported from tdeck-max-phone (main/src/net_wifi.c):
// the reconnect backoff ladder, the disconnect-clears-connected-bit fix, and
// the lock-protected IP snapshot are all unchanged. What's NEW here (this
// project's WiFi wizard needs to scan for APs before any SSID/password is
// known, which tdeck-max-phone never had to do):
//
//   - Driver bring-up (netif/event-loop/wifi_init/handler-registration) is
//     factored into ensure_driver_up(), guarded so it runs exactly once --
//     the original code ran all of it unconditionally inside
//     wifi_sta_connect(), which would abort on a second call
//     (esp_netif_create_default_wifi_sta() asserts if called twice).
//   - wifi_sta_connect()'s STA_START handler used to unconditionally call
//     esp_wifi_connect(). That's now gated on s_connect_requested, which
//     wifi_scan_start() leaves false (bring the driver up, scan, don't
//     associate) and wifi_sta_connect() sets true before starting/kicking
//     the driver.
#include "net_wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "net_wifi";

#define WIFI_GOT_IP_BIT BIT0
// Overridable so the timeout path itself can be exercised quickly.
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 30000
#endif
// Reconnect backoff after a disconnect: 1 s, 2 s, 4 s ... capped. Without
// it an AP outage becomes a reconnect storm.
#define WIFI_RECONNECT_MIN_MS 1000
#define WIFI_RECONNECT_MAX_MS 30000

static EventGroupHandle_t s_wifi_evt;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_ms = WIFI_RECONNECT_MIN_MS;

// s_ip is written from the event-loop task and read from the main task;
// the spinlock makes the 16-byte copy atomic either side.
static portMUX_TYPE s_ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ip[16] = "0.0.0.0";

// New state for scan-before-connect (see file header).
static bool s_driver_up = false;         // esp_wifi_init()+handlers+mode done, exactly once
static bool s_wifi_started = false;      // esp_wifi_start() called, exactly once
static bool s_connect_requested = false; // gates STA_START's auto-connect
static char s_ssid[33] = "";

// Static IP config (Settings app). Applied to the STA netif when it is
// created (ensure_driver_up) and immediately on change if it already exists.
static esp_netif_t *s_netif = NULL;
static bool s_static_set = false;
static char s_static_ip[16], s_static_mask[16], s_static_gw[16];

static void apply_ip_config(void)
{
    if (!s_netif) return;
    if (!s_static_set) {
        esp_netif_dhcpc_start(s_netif); // idempotent-ish: returns an error if already started, harmless
        return;
    }
    esp_netif_ip_info_t info = {0};
    if (esp_netif_str_to_ip4(s_static_ip, &info.ip) != ESP_OK) {
        ESP_LOGE(TAG, "bad static IP \"%s\" -- staying on DHCP", s_static_ip);
        esp_netif_dhcpc_start(s_netif);
        return;
    }
    if (!s_static_mask[0] || esp_netif_str_to_ip4(s_static_mask, &info.netmask) != ESP_OK)
        esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
    if (s_static_gw[0]) esp_netif_str_to_ip4(s_static_gw, &info.gw);
    esp_netif_dhcpc_stop(s_netif);
    esp_err_t err = esp_netif_set_ip_info(s_netif, &info);
    ESP_LOGI(TAG, "static IP %s/%s gw %s: %s", s_static_ip, s_static_mask, s_static_gw, esp_err_to_name(err));
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Only auto-connect if a connect was actually requested -- a scan
        // (wifi_scan_start()) also starts the driver but must not associate.
        if (s_connect_requested) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Drop "connected" immediately so wifi_is_connected() tells the truth
        // during the outage, then retry with backoff -- but only if we were
        // actually trying to be connected; a bare scan can also disconnect
        // (e.g. leftover association) and must not spin up a reconnect timer.
        xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT);
        if (!s_connect_requested) return;
        ESP_LOGW(TAG, "disconnected; retrying in %lu ms", (unsigned long)s_reconnect_ms);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_ms * 1000ULL);
        s_reconnect_ms = (s_reconnect_ms * 2 > WIFI_RECONNECT_MAX_MS) ? WIFI_RECONNECT_MAX_MS
                                                                      : s_reconnect_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        char ip[16];
        esp_ip4addr_ntoa(&evt->ip_info.ip, ip, sizeof(ip));
        taskENTER_CRITICAL(&s_ip_lock);
        memcpy(s_ip, ip, sizeof(s_ip));
        taskEXIT_CRITICAL(&s_ip_lock);
        s_reconnect_ms = WIFI_RECONNECT_MIN_MS;
        ESP_LOGI(TAG, "got IP %s", ip);
        xEventGroupSetBits(s_wifi_evt, WIFI_GOT_IP_BIT);
    }
}

// Bring the WiFi driver up in STA mode, exactly once. Safe to call from
// both wifi_scan_start() and wifi_sta_connect(), in either order.
static esp_err_t ensure_driver_up(void)
{
    if (s_driver_up) return ESP_OK;

    s_wifi_evt = xEventGroupCreate();

    // Both of these are process-wide singletons; INVALID_STATE means
    // "already done", not a failure.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta(); // asserts if called twice -- guarded by s_driver_up
    apply_ip_config();

    const esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_driver_up = true;
    return ESP_OK;
}

void wifi_set_static_ip(const char *ip, const char *mask, const char *gw)
{
    s_static_set = (ip && ip[0]);
    snprintf(s_static_ip, sizeof(s_static_ip), "%s", ip ? ip : "");
    snprintf(s_static_mask, sizeof(s_static_mask), "%s", mask ? mask : "");
    snprintf(s_static_gw, sizeof(s_static_gw), "%s", gw ? gw : "");
    apply_ip_config(); // no-op until the netif exists
}

esp_err_t wifi_sta_connect_start(const char *ssid, const char *pass)
{
    esp_err_t err = ensure_driver_up();
    if (err != ESP_OK) return err;
    s_connect_requested = true;
    xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    ESP_LOGI(TAG, "connecting to \"%s\" (async) ...", ssid);
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect(); // drop any current association first; STA_DISCONNECTED's backoff reconnects with the new config
        esp_wifi_connect();
    }
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char *ssid, const char *pass)
{
    esp_err_t err = ensure_driver_up();
    if (err != ESP_OK) return err;

    s_connect_requested = true;

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    // Checked, not ESP_ERROR_CHECK'd: this runs on every first-boot wizard
    // attempt with user-typed credentials, and main.c's whole philosophy is
    // log-and-continue so one bad call doesn't hide/abort everything else.
    // A reboot here would also re-run the wizard from scratch instead of
    // just showing the error screen the caller already has ready.
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    if (!s_wifi_started) {
        err = esp_wifi_start(); // fires STA_START -> connects, s_connect_requested is now true
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
    } else {
        // Driver was already started by a prior wifi_scan_start() -- STA_START
        // won't fire again, so kick the connect directly.
        esp_wifi_connect();
    }

    // Bounded, not portMAX_DELAY: a typo'd SSID or wrong PSK otherwise hangs
    // here forever with no further output. The driver keeps retrying in the
    // background either way (see WIFI_EVENT_STA_DISCONNECTED above).
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_GOT_IP_BIT)) {
        ESP_LOGE(TAG, "no IP after %d s -- check SSID/password/AP reachability",
                 WIFI_CONNECT_TIMEOUT_MS / 1000);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    if (!s_wifi_evt) return false;
    return (xEventGroupGetBits(s_wifi_evt) & WIFI_GOT_IP_BIT) != 0;
}

const char *wifi_local_ip(void)
{
    static char snapshot[16];
    taskENTER_CRITICAL(&s_ip_lock);
    memcpy(snapshot, s_ip, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_ip_lock);
    return snapshot;
}

int wifi_scan_start(void)
{
    esp_err_t err = ensure_driver_up();
    if (err != ESP_OK) return -1;

    if (!s_wifi_started) {
        // s_connect_requested is still false here, so the STA_START handler
        // is a no-op -- the driver comes up without associating.
        err = esp_wifi_start();
        if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start (for scan) failed: %d", err); return -1; }
        s_wifi_started = true;
    }

    wifi_scan_config_t scan_cfg = {0}; // all channels, active scan, default dwell times
    err = esp_wifi_scan_start(&scan_cfg, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num); // non-destructive; the actual list is consumed by wifi_scan_get_results()
    ESP_LOGI(TAG, "scan found %u AP(s)", num);
    return (int)num;
}

uint16_t wifi_scan_get_results(wifi_ap_record_t *out, uint16_t max_results)
{
    uint16_t num = max_results;
    esp_err_t err = esp_wifi_scan_get_ap_records(&num, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %d", err);
        return 0;
    }
    return num;
}

int8_t wifi_get_rssi(void)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

const char *wifi_get_ssid(void)
{
    return s_ssid;
}
