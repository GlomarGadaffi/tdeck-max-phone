#ifndef TINCAN_UAC_HPP
#define TINCAN_UAC_HPP

#include <string>
#include <cstdint>
#include <cstddef>

// SIP UAC/UAS for this phone. Registers as a plain LAN extension to a
// drawbridge PBX instance, which owns all 3CX Call Control API integration
// -- this class never talks to 3CX directly, and never needs to.
//
// Ported from tincan's SipUac (main/sip_uac.h/.cpp in the sibling tincan
// repo), which only supports placing outbound calls. This class adds
// inbound-call handling (drawbridge RING-ALLs registered extensions on an
// incoming 3CX call -- see RequestsHandler::routeInboundAnchorCall in
// drawbridge), owns the RTP socket and G.711 encode/decode internally
// (tincan's app_main.cpp did that inline), and supports placing more than
// one call per boot -- tincan's own PoC idles forever after a single call
// and needs a hardware reset to call again; this does not.
class TincanUac
{
public:
    TincanUac();
    ~TincanUac();

    // Bind sockets. Must be called once, after Wi-Fi is up, before anything else.
    bool init(const std::string &localIp, int localSipPort, int localRtpPort,
              const std::string &serverIp, int serverPort,
              const std::string &selfExt);

    // Blocking (bounded, ~6s worst case): REGISTER our extension. Returns
    // true on 200 OK. drawbridge's registrar is open by default -- no
    // pre-created extension or credentials are needed.
    bool registerExt();

    // Non-blocking: drain pending SIP messages and advance internal call
    // state (detects an inbound INVITE, matches ACK/BYE/CANCEL to the
    // active dialog). Call this every loop tick regardless of call state.
    void poll();

    // Blocking (bounded, ~120s worst case): INVITE `calleeExt` through the
    // server and wait for it to be answered. Dial "9<number>" to route out
    // through drawbridge's 3CX anchor; any other extension dials another
    // LAN extension directly. Call registerExt() first.
    bool placeCall(const std::string &calleeExt);

    // True once poll() has seen an unsolicited INVITE and no call is
    // already active. Stays true until answer()/reject() is called or the
    // caller sends CANCEL.
    bool hasIncomingCall() const { return _state == State::Ringing; }
    std::string incomingCallerId() const { return _peerExt; }

    // Accept/decline the pending inbound offer (see hasIncomingCall()).
    bool answer();
    void reject();

    bool inCall() const { return _state == State::InCall; }

    // One-shot: true exactly once, the first poll() after an active call
    // ends (peer BYE, or our own hangup() completing). Callers should
    // treat this as the "return to Idle" signal.
    bool callEnded();

    // End an active or ringing-out/ringing-in call (sends BYE if InCall,
    // CANCEL if we're the one still waiting on our own outbound INVITE,
    // or a decline if an inbound offer is pending).
    void hangup();

    // Media -- only meaningful while inCall(). Both are non-blocking and
    // safe to call every loop tick regardless of state (no-ops otherwise).
    void sendAudioFrame(const int16_t *pcm, size_t samples);
    size_t recvAudioFrame(int16_t *pcm, size_t maxSamples);

private:
    enum class State { Idle, Calling, Ringing, InCall, Ending };

    bool openSipSocket();
    bool openRtpSocket();
    void resetDialog();
    void sendToServer(const std::string &msg);

    std::string buildRegister() const;
    std::string buildInvite(const std::string &calleeExt) const;
    std::string buildAck() const;
    std::string buildBye() const;
    std::string buildCancel() const;
    std::string buildInviteResponse(int code, const char *reason, bool withSdp) const;
    std::string buildGenericResponse(const std::string &via, const std::string &from,
                                      const std::string &to, const std::string &callId,
                                      const std::string &cseq, int code, const char *reason) const;

    void handleInboundInvite(const std::string &raw);
    void handleInboundBye(const std::string &raw);
    void handleInboundCancel(const std::string &raw);

    std::string _localIp, _serverIp, _selfExt, _peerExt;
    int _localSipPort = 0, _localRtpPort = 0, _serverPort = 0;

    int _sipSock = -1;
    int _rtpSock = -1;

    State _state = State::Idle;
    bool _callEndedPending = false;

    // Outbound-dialog fields (mirrors tincan's SipUac).
    std::string _callId, _fromTag, _remoteTag;

    // Inbound-dialog fields, captured verbatim from the offering INVITE so
    // our responses/BYE echo the right Via/From/To/Call-ID/CSeq.
    std::string _inVia, _inFrom, _inTo, _inCallId, _inCseqInvite;
    std::string _ourToTag;

    std::string _remoteRtpIp;
    int _remoteRtpPort = 0;

    uint16_t _rtpSeq = 0;
    uint32_t _rtpTs = 0;
    uint32_t _rtpSsrc = 0;
};

#endif // TINCAN_UAC_HPP
