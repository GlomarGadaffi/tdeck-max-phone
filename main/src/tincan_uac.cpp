#include "tincan_uac.hpp"
#include "es8311_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TINCAN_UAC";

TincanUac::TincanUac() {}
TincanUac::~TincanUac() { stopMediaLoop(); }

bool TincanUac::init(const std::string& localIp, int localSipPort, int localRtpPort,
                     const std::string& serverIp, int serverPort,
                     const std::string& selfExt, const std::string& calleeExt)
{
    _selfExt = selfExt;
    _calleeExt = calleeExt;
    ESP_LOGI(TAG, "Initializing tincan UAC handset: Ext %s calling %s on PBX %s:%d",
             selfExt.c_str(), calleeExt.c_str(), serverIp.c_str(), serverPort);
    return true;
}

void TincanUac::startMediaLoop()
{
    _running.store(true);
    ESP_LOGI(TAG, "tincan media loop started (Half-duplex PTT / Full-duplex RTP)");
}

void TincanUac::stopMediaLoop()
{
    _running.store(false);
    ESP_LOGI(TAG, "tincan media loop stopped");
}

void TincanUac::setPttState(bool active)
{
    _pttActive.store(active);
    ESP_LOGI(TAG, "PTT state: %s", active ? "TALK (Mic Tx)" : "LISTEN (Spk Rx)");
}
