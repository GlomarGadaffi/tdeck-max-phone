#include "net_wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "net_wifi";

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_CONNECT_TIMEOUT_MS 30000
static EventGroupHandle_t s_wifi_evt;
static char s_ip[16] = "0.0.0.0";

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected; retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip, sizeof(s_ip));
        ESP_LOGI(TAG, "got IP %s", s_ip);
        xEventGroupSetBits(s_wifi_evt, WIFI_GOT_IP_BIT);
    }
}

esp_err_t wifi_sta_connect(const char *ssid, const char *pass)
{
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

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

const char *wifi_local_ip(void) { return s_ip; }
