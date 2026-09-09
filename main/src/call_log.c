#include "call_log.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CALL_LOG";
static const char *NVS_NS  = "phone";
static const char *NVS_KEY = "log";

static call_log_entry_t s_log[CALL_LOG_MAX];
static int s_count = 0;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, s_log, sizeof(call_log_entry_t) * (size_t)s_count) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

esp_err_t call_log_init(void)
{
    s_count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t len = sizeof(s_log);
    err = nvs_get_blob(h, NVS_KEY, s_log, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    s_count = (int)(len / sizeof(call_log_entry_t));
    for (int i = 0; i < s_count; i++) s_log[i].number[CALL_LOG_NUMBER_MAX] = '\0';
    ESP_LOGI(TAG, "%d recent call(s) loaded", s_count);
    return ESP_OK;
}

int call_log_count(void)
{
    return s_count;
}

const call_log_entry_t *call_log_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_log[idx];
}

void call_log_add(const char *number, call_dir_t dir, bool answered, uint16_t duration_s)
{
    if (!number || !number[0]) return;
    if (s_count < CALL_LOG_MAX) s_count++;
    for (int i = s_count - 1; i > 0; i--) s_log[i] = s_log[i - 1];
    memset(&s_log[0], 0, sizeof(s_log[0]));
    strncpy(s_log[0].number, number, CALL_LOG_NUMBER_MAX);
    s_log[0].dir = (uint8_t)dir;
    s_log[0].answered = answered ? 1 : 0;
    s_log[0].duration_s = duration_s;
    save();
}

const char *call_log_last_dialled(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_log[i].dir == CALL_DIR_OUT && s_log[i].answered) return s_log[i].number;
    }
    return "";
}
