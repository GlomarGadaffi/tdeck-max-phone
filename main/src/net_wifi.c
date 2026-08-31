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
// Overridable so the timeout path itself can be exercised quickly (QEMU
// emulates RF calibration ~100x slower than realtime, so waiting out the
// real 30s would take about an hour of wall clock):
//   idf.py -DWIFI_CONNECT_TIMEOUT_MS=3000 build
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 30000
#endif
// Reconnect backoff after a disconnect: 1 s, 2 s, 4 s ... capped. Without
// it an AP outage becomes a reconnect storm (ISSUES.md #3).
#define WIFI_RECONNECT_MIN_MS 1000
#define WIFI_RECONNECT_MAX_MS 30000

static EventGroupHandle_t s_wifi_evt;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_ms = WIFI_RECONNECT_MIN_MS;

// s_ip is written from the event-loop task and read from the main task;
// the spinlock makes the 16-byte copy atomic either side (ISSUES.md #4).
static portMUX_TYPE s_ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ip[16] = "0.0.0.0";

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Drop "connected" immediately so wifi_is_connected() tells the truth
        // during the outage (ISSUES.md #1), then retry with backoff.
        xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT);
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

esp_err_t wifi_sta_connect(const char *ssid, const char *pass)
{
    s_wifi_evt = xEventGroupCreate();

    // Both of these are process-wide singletons that another component (or a
    // second call here) may already have brought up; INVALID_STATE then just
    // means "already done", not a failure (ISSUES.md #2).
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();

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

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    // Bounded, not portMAX_DELAY: a typo'd SSID or wrong PSK otherwise
    // hangs here forever with no further output, which during bring-up is
    // indistinguishable from a crash. The driver keeps retrying in the
    // background either way (see WIFI_EVENT_STA_DISCONNECTED above), so a
    // timeout costs nothing but lets the caller report the failure.
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
    // Snapshot under the lock; callers use the result immediately (they
    // always have) and the snapshot buffer is only ever written here on the
    // main task.
    static char snapshot[16];
    taskENTER_CRITICAL(&s_ip_lock);
    memcpy(snapshot, s_ip, sizeof(snapshot));
    taskEXIT_CRITICAL(&s_ip_lock);
    return snapshot;
}
