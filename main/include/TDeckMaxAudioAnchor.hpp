#ifndef TDECK_MAX_AUDIO_ANCHOR_HPP
#define TDECK_MAX_AUDIO_ANCHOR_HPP

#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>

// ── AnchorClient Abstract Interface ─────────────────────────────────────────
class AnchorClient
{
public:
	struct CallEvent
	{
		enum Type { Ringing, Answered, Dropped, Dtmf, Incoming } type;
		std::string participantId;
		std::string dtmfDigit;
		std::string callerId;
	};
	using EventCallback = std::function<void(const CallEvent&)>;
	using AudioRxCallback = std::function<void(const std::string& participantId, const int16_t* pcmSamples, size_t count)>;

	virtual ~AnchorClient() = default;

	virtual bool init(const std::string& baseUrl, const std::string& clientId,
	                  const std::string& clientSecret, const std::string& sourceDn) = 0;
	virtual bool start() = 0;
	virtual void stop() = 0;
	virtual bool isConnected() const = 0;
	virtual bool makeCall(const std::string& destination, std::string* ownLegOut = nullptr) = 0;
	virtual bool answerCall(const std::string& participantId) = 0;
	virtual bool dropCall(const std::string& participantId) = 0;
	virtual void setEventCallback(EventCallback cb) = 0;
	virtual bool writeAudio(const std::string& participantId, const int16_t* pcmSamples, size_t count) = 0;
	virtual void registerAudioRxCallback(AudioRxCallback cb) = 0;
	virtual void tick() = 0;
};

// ── 3CX / Telephony Route Point Client logic for T-Deck MAX ────────────────
namespace threecx {
	inline std::string tokenUrl(const std::string& baseUrl) { return baseUrl + "/connect/token"; }
	inline std::string participantsUrl(const std::string& baseUrl, const std::string& dn) { return baseUrl + "/callcontrol/" + dn + "/participants"; }
	inline std::string devicesUrl(const std::string& baseUrl, const std::string& dn) { return baseUrl + "/callcontrol/" + dn + "/devices"; }
	inline std::string controlWsUrl(const std::string& baseUrl) {
		if (baseUrl.rfind("https://", 0) == 0) return "wss://" + baseUrl.substr(8) + "/callcontrol/ws";
		if (baseUrl.rfind("http://", 0) == 0) return "ws://" + baseUrl.substr(7) + "/callcontrol/ws";
		return "wss://" + baseUrl + "/callcontrol/ws";
	}
	inline std::string participantActionUrl(const std::string& baseUrl, const std::string& dn, const std::string& participantId, const std::string& action) {
		std::string u = baseUrl + "/callcontrol/" + dn + "/participants/" + participantId;
		if (!action.empty()) u += "/" + action;
		return u;
	}
}

// Concrete T-Deck MAX Anchor Client incorporating 3CX Route Point WS & ES8311 / 4G audio
class TDeckMaxAudioAnchor : public AnchorClient
{
public:
	TDeckMaxAudioAnchor();
	~TDeckMaxAudioAnchor() override;

	bool init(const std::string& baseUrl, const std::string& clientId,
	          const std::string& clientSecret, const std::string& sourceDn) override;
	bool start() override;
	void stop() override;
	bool isConnected() const override;
	bool makeCall(const std::string& destination, std::string* ownLegOut = nullptr) override;
	bool answerCall(const std::string& participantId) override;
	bool dropCall(const std::string& participantId) override;
	void setEventCallback(EventCallback cb) override;
	bool writeAudio(const std::string& participantId, const int16_t* pcmSamples, size_t count) override;
	void registerAudioRxCallback(AudioRxCallback cb) override;
	void tick() override;

	// Audio hardware route toggle via XL9555 IO12
	// false = ES8311 Local Codec (Microphone & Speaker)
	// true  = A7682E 4G LTE Audio Path
	void setAudioRoute(bool use4GModem);

	// XL9555 Hardware Control Methods (derived from LilyGO T-Deck-MAX spec)
	void setSpeakerAmplifier(bool enable); // XL9555 IO06
	void setLoraAntennaInternal(bool internal); // XL9555 IO04

private:
	std::string _baseUrl;
	std::string _clientId;
	std::string _clientSecret;
	std::string _sourceDn;

	std::atomic<bool> _connected{false};
	std::atomic<bool> _use4GModem{false};

	EventCallback _eventCb;
	AudioRxCallback _audioRxCb;
};

#endif // TDECK_MAX_AUDIO_ANCHOR_HPP
