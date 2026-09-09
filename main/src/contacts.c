#include "contacts.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CONTACTS";
static const char *NVS_NS  = "contacts";
static const char *NVS_KEY = "v1";

static contact_t s_list[CONTACTS_MAX];
static int s_count = 0;

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(((const contact_t *)a)->name, ((const contact_t *)b)->name);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, s_list, sizeof(contact_t) * (size_t)s_count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t contacts_init(void)
{
    s_count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_list);
    err = nvs_get_blob(h, NVS_KEY, s_list, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    s_count = (int)(len / sizeof(contact_t));
    for (int i = 0; i < s_count; i++) {
        s_list[i].name[CONTACT_NAME_MAX] = '\0';
        s_list[i].number[CONTACT_NUMBER_MAX] = '\0';
    }
    ESP_LOGI(TAG, "%d contact(s) loaded", s_count);
    return ESP_OK;
}

int contacts_count(void)
{
    return s_count;
}

const contact_t *contacts_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_list[idx];
}

int contacts_find_by_number(const char *number)
{
    if (!number || !number[0]) return -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].number, number) == 0) return i;
    }
    return -1;
}

esp_err_t contacts_set(int idx, const contact_t *c)
{
    if (!c || !c->number[0]) return ESP_ERR_INVALID_ARG;
    if (idx == -1) {
        if (s_count >= CONTACTS_MAX) return ESP_ERR_NO_MEM;
        idx = s_count++;
    } else if (idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    s_list[idx] = *c;
    s_list[idx].name[CONTACT_NAME_MAX] = '\0';
    s_list[idx].number[CONTACT_NUMBER_MAX] = '\0';
    if (!s_list[idx].name[0]) strncpy(s_list[idx].name, c->number, CONTACT_NAME_MAX);
    qsort(s_list, (size_t)s_count, sizeof(contact_t), cmp_name);
    return save();
}

esp_err_t contacts_delete(int idx)
{
    if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG;
    for (int i = idx; i < s_count - 1; i++) s_list[i] = s_list[i + 1];
    s_count--;
    return save();
}

const char *contacts_display_name(const char *number)
{
    int i = contacts_find_by_number(number);
    return i >= 0 ? s_list[i].name : number;
}
