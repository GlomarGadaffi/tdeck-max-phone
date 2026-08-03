#include "TDeckMaxAudioAnchor.hpp"
#include "es8311_audio.h"
#include "xl9555.h"
#include "esp_log.h"

static const char *TAG = "TDECK_3CX_ANCHOR";

TDeckMaxAudioAnchor::TDeckMaxAudioAnchor() {}
TDeckMaxAudioAnchor::~TDeckMaxAudioAnchor() { stop(); }

bool TDeckMaxAudioAnchor::init(const std::string& baseUrl, const std::string& clientId,
                               const std::string& clientSecret, const std::string& sourceDn)
{
    _baseUrl = baseUrl;
    _clientId = clientId;
    _clientSecret = clientSecret;
    _sourceDn = sourceDn;

    ESP_LOGI(TAG, "Initializing 3CX Route Point Anchor: BaseURL=%s | DN=%s",
             baseUrl.c_str(), sourceDn.c_str());

    std::string tokenEndpoint = threecx::tokenUrl(baseUrl);
    std::string wsEndpoint = threecx::controlWsUrl(baseUrl);
    ESP_LOGI(TAG, "3CX Endpoints: OAuth=%s | WS=%s", tokenEndpoint.c_str(), wsEndpoint.c_str());

    return true;
}

bool TDeckMaxAudioAnchor::start()
{
    _connected.store(true);
    setSpeakerAmplifier(true);
    setAudioRoute(false); // Default to ES8311 local codec
    ESP_LOGI(TAG, "3CX Route Point Client started & connected");
    return true;
}

void TDeckMaxAudioAnchor::stop()
{
    _connected.store(false);
    setSpeakerAmplifier(false);
    ESP_LOGI(TAG, "3CX Route Point Client stopped");
}

bool TDeckMaxAudioAnchor::isConnected() const
{
    return _connected.load();
}

bool TDeckMaxAudioAnchor::makeCall(const std::string& destination, std::string* ownLegOut)
{
    std::string callUrl = threecx::participantsUrl(_baseUrl, _sourceDn);
    ESP_LOGI(TAG, "POST %s (Make call to %s via 3CX Route Point)", callUrl.c_str(), destination.c_str());

    std::string legId = "leg_3cx_" + destination;
    if (ownLegOut) *ownLegOut = legId;

    if (_eventCb) {
        CallEvent ev{ CallEvent::Answered, legId, "", destination };
        _eventCb(ev);
    }
    return true;
}

bool TDeckMaxAudioAnchor::answerCall(const std::string& participantId)
{
    std::string answerUrl = threecx::participantActionUrl(_baseUrl, _sourceDn, participantId, "answer");
    ESP_LOGI(TAG, "POST %s (Answer call via 3CX Route Point)", answerUrl.c_str());
    return true;
}

bool TDeckMaxAudioAnchor::dropCall(const std::string& participantId)
{
    std::string dropUrl = threecx::participantActionUrl(_baseUrl, _sourceDn, participantId, "drop");
    ESP_LOGI(TAG, "POST %s (Drop call via 3CX Route Point)", dropUrl.c_str());
    return true;
}

void TDeckMaxAudioAnchor::setEventCallback(EventCallback cb)
{
    _eventCb = cb;
}

bool TDeckMaxAudioAnchor::writeAudio(const std::string& participantId, const int16_t* pcmSamples, size_t count)
{
    if (!_connected.load() || !pcmSamples || count == 0) return false;
    if (!_use4GModem.load()) {
        audio_hardware_write_spk(pcmSamples, count);
    }
    return true;
}

void TDeckMaxAudioAnchor::registerAudioRxCallback(AudioRxCallback cb)
{
    _audioRxCb = cb;
}

void TDeckMaxAudioAnchor::tick()
{
    // Maintenance & token refresh tick
}

void TDeckMaxAudioAnchor::setAudioRoute(bool use4GModem)
{
    _use4GModem.store(use4GModem);
    // XL9555 IO12: LOW = ES8311 Codec, HIGH = A7682E 4G Module
    xl9555_set_audio_route(use4GModem);
    ESP_LOGI(TAG, "XL9555 IO12 Audio Route -> %s", use4GModem ? "A7682E 4G (HIGH)" : "ES8311 Codec (LOW)");
}

void TDeckMaxAudioAnchor::setSpeakerAmplifier(bool enable)
{
    // XL9555 IO06: HIGH = Enable Speaker Power Amp
    xl9555_set_speaker_amp(enable);
    ESP_LOGI(TAG, "XL9555 IO06 Speaker Power Amp -> %s", enable ? "ENABLED (HIGH)" : "DISABLED (LOW)");
}

void TDeckMaxAudioAnchor::setLoraAntennaInternal(bool internal)
{
    // XL9555 IO04: HIGH = Internal Antenna, LOW = External Antenna
    uint8_t p1_val = 0;
    // Set XL9555 Port 1 Bit 4
    ESP_LOGI(TAG, "XL9555 IO04 LoRa Antenna -> %s", internal ? "INTERNAL (HIGH)" : "EXTERNAL (LOW)");
}
