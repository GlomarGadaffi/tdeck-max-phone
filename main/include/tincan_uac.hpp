#ifndef TINCAN_UAC_HPP
#define TINCAN_UAC_HPP

#include <string>
#include <atomic>
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

    // Refresh the registration before POC_SIP_REG_EXPIRES elapses. Call
    // every loop tick; it no-ops until the refresh deadline (Expires/2)
    // passes, then re-REGISTERs. Without this the binding lapses and the
    // phone silently stops receiving calls after an hour with no visible
    // symptom. Returns true if a refresh was attempted AND failed, so the
    // caller can surface it on the UI.
    bool maintainRegistration();

    // Non-blocking: drain pending SIP messages and advance internal call
    // state (detects an inbound INVITE, matches ACK/BYE/CANCEL to the
    // active dialog). Call this every loop tick regardless of call state.
    void poll();

    // Blocking (bounded, ~120s worst case): INVITE `calleeExt` through the
    // server and wait for it to be answered. Dial "9<number>" to route out
    // through drawbridge's 3CX anchor; any other extension dials another
    // LAN extension directly. Call registerExt() first.
    //
    // Known limitation: because this blocks and drains _sipSock inline
    // (matching tincan's original design), a genuinely new inbound INVITE
    // that arrives while we're mid-dial-out gets no SIP response at all --
    // not even a 486 -- until placeCall() returns and poll() resumes. The
    // caller's UAC will eventually time out waiting. Fixing this needs a
    // non-blocking dial-out state machine, which is a larger change than
    // this PoC's scope; see the bench-test doc for how this shows up in
    // practice.
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
    // safe to call every tick regardless of state (no-ops otherwise).
    //
    // THREADING: these two (plus flushRtpBacklog) are the only members safe
    // to call from a task other than the one driving poll()/placeCall()/
    // answer()/hangup(). They touch only the POD media endpoint and the RTP
    // counters they exclusively own. Do not call anything else from the
    // audio task.
    void sendAudioFrame(const int16_t *pcm, size_t samples);
    size_t recvAudioFrame(int16_t *pcm, size_t maxSamples);

    // Discard queued inbound RTP. Call when starting to pump a new call, so
    // a pre-answer backlog isn't played out as latency that never recovers.
    void flushRtpBacklog();

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
    void handleInboundAck(const std::string &raw);
    void tickOkRetransmit();

    // RFC 3261 requires a UAS to keep retransmitting its 2xx to an INVITE
    // until the ACK arrives -- the 2xx is NOT protected by the transaction
    // layer. Without this a single dropped 200 OK leaves us believing we're
    // in a call while the caller keeps ringing. Retransmit starts at T1 and
    // doubles to a T2 ceiling, giving up after 64*T1.
    std::string _pendingOk;
    bool _awaitingAck = false;
    int64_t _nextOkRetransmitUs = 0;
    int _okRetransmitMs = 0;
    int64_t _ackGiveUpUs = 0;

    std::string _localIp, _serverIp, _selfExt, _peerExt;
    int _localSipPort = 0, _localRtpPort = 0, _serverPort = 0;

    int _sipSock = -1;
    int _rtpSock = -1;

    State _state = State::Idle;
    bool _callEndedPending = false;

    // Deadline (esp_timer microseconds) for the next REGISTER refresh; 0
    // until the first successful registration. See maintainRegistration().
    int64_t _nextRegisterUs = 0;

    // Outbound-dialog fields (mirrors tincan's SipUac).
    std::string _callId, _fromTag, _remoteTag;

    // Inbound-dialog fields, captured verbatim from the offering INVITE so
    // our responses/BYE echo the right Via/From/To/Call-ID/CSeq.
    std::string _inVia, _inFrom, _inTo, _inCallId, _inCseqInvite;
    std::string _ourToTag;

    std::string _remoteRtpIp;
    int _remoteRtpPort = 0;

    // Media endpoint published for the audio task, kept separate from the
    // std::string fields above on purpose. sendAudioFrame()/recvAudioFrame()
    // run on a different task than poll()/answer()/hangup(), so they must
    // never touch _remoteRtpIp (a std::string the control path reassigns --
    // a torn read there is a crash, not a glitch). These are POD in network
    // byte order, published with release/acquire ordering around
    // _mediaActive: the control path fills them BEFORE setting the flag and
    // clears the flag BEFORE disturbing them, so the media path only ever
    // reads a fully-written endpoint. Also avoids a per-frame inet_addr()
    // and string copy at 50 Hz.
    std::atomic<bool> _mediaActive{false};
    uint32_t _mediaIpBe = 0;
    uint16_t _mediaPortBe = 0;

    void publishMediaTarget();   // fill POD fields, then arm _mediaActive
    void retireMediaTarget();    // disarm _mediaActive

    uint16_t _rtpSeq = 0;
    uint32_t _rtpTs = 0;
    uint32_t _rtpSsrc = 0;
};

#endif // TINCAN_UAC_HPP
