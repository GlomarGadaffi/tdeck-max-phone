#include "tincan_uac.hpp"
#include "poc_config.h"
#include "g711.h"
#include "rtp.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include "esp_log.h"
#include "esp_random.h"

#include <cstdio>
#include <cstring>

// Vendored pocket-dial SIP layer (MIT, ported from tincan/components/sip_core)
// -- used to PARSE requests/responses + SDP. See sip_core/CMakeLists.txt.
#include "SipMessageFactory.hpp"
#include "SipSdpMessage.hpp"
#include "SipMessageTypes.h"
#include "IDGen.hpp"

static const char *TAG = "tincan_uac";

// ── helpers (ported from tincan's sip_uac.cpp) ──────────────────────────────
static int statusCode(std::string_view type)
{
    if (type.rfind("SIP/2.0 ", 0) != 0) return -1;
    int code = 0;
    for (size_t i = 8; i < type.size() && type[i] >= '0' && type[i] <= '9'; ++i)
        code = code * 10 + (type[i] - '0');
    return code;
}

static std::string extractParam(std::string_view header, const char *key)
{
    size_t p = header.find(key);
    if (p == std::string_view::npos) return {};
    p += std::strlen(key);
    size_t e = p;
    while (e < header.size() && header[e] != ';' && header[e] != '>' &&
           header[e] != '\r' && header[e] != '\n' && header[e] != ' ')
        ++e;
    return std::string(header.substr(p, e - p));
}

static std::string ipFromConnection(std::string_view conn)
{
    size_t sp = conn.rfind(' ');
    return (sp == std::string_view::npos) ? std::string(conn)
                                          : std::string(conn.substr(sp + 1));
}

static std::string randBranch() { return "z9hG4bK" + IDGen::GenerateID(10); }

// SipMessage's getters (getVia/getFrom/getTo/getCallID/getCSeq) return the
// RAW header line including the field name ("Call-ID: 9f8e...@host"), not
// just the value. Strips the "Field-Name: " prefix to get the bare value,
// needed anywhere we're reusing a captured value to build a fresh request
// (as opposed to echoing a captured line verbatim in a UAS response, where
// the prefix is wanted).
static std::string headerValue(std::string_view line)
{
    size_t p = line.find(':');
    if (p == std::string_view::npos) return std::string(line);
    p++;
    while (p < line.size() && line[p] == ' ') p++;
    return std::string(line.substr(p));
}

// ── ctor/dtor ────────────────────────────────────────────────────────────--
TincanUac::TincanUac() {}
TincanUac::~TincanUac()
{
    if (_sipSock >= 0) close(_sipSock);
    if (_rtpSock >= 0) close(_rtpSock);
}

bool TincanUac::init(const std::string &localIp, int localSipPort, int localRtpPort,
                      const std::string &serverIp, int serverPort,
                      const std::string &selfExt)
{
    _localIp = localIp;
    _localSipPort = localSipPort;
    _localRtpPort = localRtpPort;
    _serverIp = serverIp;
    _serverPort = serverPort;
    _selfExt = selfExt;
    return openSipSocket() && openRtpSocket();
}

bool TincanUac::openSipSocket()
{
    if (_sipSock >= 0) return true;
    _sipSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_sipSock < 0) { ESP_LOGE(TAG, "SIP socket() failed"); return false; }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(_localSipPort);
    if (bind(_sipSock, (sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "SIP bind() failed");
        return false;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 }; // 500 ms poll for blocking calls
    setsockopt(_sipSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

bool TincanUac::openRtpSocket()
{
    if (_rtpSock >= 0) return true;
    _rtpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_rtpSock < 0) { ESP_LOGE(TAG, "RTP socket() failed"); return false; }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(_localRtpPort);
    if (bind(_rtpSock, (sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "RTP bind() failed");
        return false;
    }
    return true;
}

void TincanUac::resetDialog()
{
    _callId.clear();
    _fromTag.clear();
    _remoteTag.clear();
    _peerExt.clear();
    _inVia.clear();
    _inFrom.clear();
    _inTo.clear();
    _inCallId.clear();
    _inCseqInvite.clear();
    _ourToTag.clear();
    _remoteRtpIp.clear();
    _remoteRtpPort = 0;
}

void TincanUac::sendToServer(const std::string &msg)
{
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = inet_addr(_serverIp.c_str());
    srv.sin_port = htons(_serverPort);
    sendto(_sipSock, msg.data(), msg.size(), 0, (sockaddr *)&srv, sizeof(srv));
}

// ── message builders ────────────────────────────────────────────────────--
std::string TincanUac::buildRegister() const
{
    char msg[768];
    std::snprintf(msg, sizeof(msg),
        "REGISTER sip:%s:%d SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: reg-%s@%s\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Contact: <sip:%s@%s:%d;transport=UDP>\r\n"
        "Expires: %d\r\n"
        "Content-Length: 0\r\n\r\n",
        _serverIp.c_str(), _serverPort,
        _localIp.c_str(), _localSipPort, randBranch().c_str(),
        _selfExt.c_str(), _serverIp.c_str(), IDGen::GenerateID(8).c_str(),
        _selfExt.c_str(), _serverIp.c_str(),
        IDGen::GenerateID(16).c_str(), _localIp.c_str(),
        _selfExt.c_str(), _localIp.c_str(), _localSipPort,
        POC_SIP_REG_EXPIRES);
    return std::string(msg);
}

std::string TincanUac::buildInvite(const std::string &calleeExt) const
{
    char sdp[512];
    int sdpLen = std::snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=%s 0 0 IN IP4 %s\r\n"
        "s=tdeck-max-phone\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %d RTP/AVP %d\r\n"
        "a=rtpmap:%d PCMU/8000\r\n"
        "a=sendrecv\r\n",
        _selfExt.c_str(), _localIp.c_str(), _localIp.c_str(),
        _localRtpPort, POC_RTP_PAYLOAD_PCMU, POC_RTP_PAYLOAD_PCMU);

    char msg[1280];
    std::snprintf(msg, sizeof(msg),
        "INVITE sip:%s@%s:%d SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 1 INVITE\r\n"
        "Contact: <sip:%s@%s:%d;transport=UDP>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n%s",
        calleeExt.c_str(), _serverIp.c_str(), _serverPort,
        _localIp.c_str(), _localSipPort, randBranch().c_str(),
        _selfExt.c_str(), _serverIp.c_str(), _fromTag.c_str(),
        calleeExt.c_str(), _serverIp.c_str(),
        _callId.c_str(),
        _selfExt.c_str(), _localIp.c_str(), _localSipPort,
        sdpLen, sdp);
    return std::string(msg);
}

std::string TincanUac::buildAck() const
{
    char msg[768];
    std::snprintf(msg, sizeof(msg),
        "ACK sip:%s@%s:%d SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 1 ACK\r\n"
        "Content-Length: 0\r\n\r\n",
        _peerExt.c_str(), _serverIp.c_str(), _serverPort,
        _localIp.c_str(), _localSipPort, randBranch().c_str(),
        _selfExt.c_str(), _serverIp.c_str(), _fromTag.c_str(),
        _peerExt.c_str(), _serverIp.c_str(), _remoteTag.c_str(),
        _callId.c_str());
    return std::string(msg);
}

std::string TincanUac::buildBye() const
{
    char msg[768];
    std::snprintf(msg, sizeof(msg),
        "BYE sip:%s@%s:%d SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 2 BYE\r\n"
        "Content-Length: 0\r\n\r\n",
        _peerExt.c_str(), _serverIp.c_str(), _serverPort,
        _localIp.c_str(), _localSipPort, randBranch().c_str(),
        _selfExt.c_str(), _serverIp.c_str(), _fromTag.c_str(),
        _peerExt.c_str(), _serverIp.c_str(), _remoteTag.c_str(),
        _callId.c_str());
    return std::string(msg);
}

std::string TincanUac::buildCancel() const
{
    char msg[768];
    std::snprintf(msg, sizeof(msg),
        "CANCEL sip:%s@%s:%d SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 1 CANCEL\r\n"
        "Content-Length: 0\r\n\r\n",
        _peerExt.c_str(), _serverIp.c_str(), _serverPort,
        _localIp.c_str(), _localSipPort, randBranch().c_str(),
        _selfExt.c_str(), _serverIp.c_str(), _fromTag.c_str(),
        _peerExt.c_str(), _serverIp.c_str(),
        _callId.c_str());
    return std::string(msg);
}

// UAS response to an inbound INVITE, echoing the request's Via/From/To/
// Call-ID/CSeq per RFC 3261 (we only add our own tag to To).
std::string TincanUac::buildInviteResponse(int code, const char *reason, bool withSdp) const
{
    char sdp[512];
    int sdpLen = 0;
    if (withSdp) {
        sdpLen = std::snprintf(sdp, sizeof(sdp),
            "v=0\r\n"
            "o=%s 0 0 IN IP4 %s\r\n"
            "s=tdeck-max-phone\r\n"
            "c=IN IP4 %s\r\n"
            "t=0 0\r\n"
            "m=audio %d RTP/AVP %d\r\n"
            "a=rtpmap:%d PCMU/8000\r\n"
            "a=sendrecv\r\n",
            _selfExt.c_str(), _localIp.c_str(), _localIp.c_str(),
            _localRtpPort, POC_RTP_PAYLOAD_PCMU, POC_RTP_PAYLOAD_PCMU);
    }

    // SipMessage's getVia()/getFrom()/getTo()/getCallID()/getCSeq() return
    // the RAW header line INCLUDING the field name (e.g. getVia() ->
    // "Via: SIP/2.0/UDP ..."), not just the value -- confirmed by reading
    // SipMessage.cpp's parse loop, which stores the whole matched line
    // (`_via = line;` etc). _inVia/_inFrom/_inTo/_inCallId/_inCseqInvite
    // were captured straight from those getters in handleInboundInvite(),
    // so they already carry their own field names. No "Via: "/"From: "/
    // etc. literal prefix belongs in these format strings, or the field
    // name doubles on the wire (a real bug this had until caught in
    // review: drawbridge/any real UAC discards a response with
    // "Via: Via: ...").
    char msg[1280];
    if (withSdp) {
        std::snprintf(msg, sizeof(msg),
            "SIP/2.0 %d %s\r\n"
            "%s\r\n"
            "%s\r\n"
            "%s;tag=%s\r\n"
            "%s\r\n"
            "%s\r\n"
            "Contact: <sip:%s@%s:%d;transport=UDP>\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %d\r\n"
            "\r\n%s",
            code, reason, _inVia.c_str(), _inFrom.c_str(), _inTo.c_str(), _ourToTag.c_str(),
            _inCallId.c_str(), _inCseqInvite.c_str(),
            _selfExt.c_str(), _localIp.c_str(), _localSipPort,
            sdpLen, sdp);
    } else {
        std::snprintf(msg, sizeof(msg),
            "SIP/2.0 %d %s\r\n"
            "%s\r\n"
            "%s\r\n"
            "%s;tag=%s\r\n"
            "%s\r\n"
            "%s\r\n"
            "Content-Length: 0\r\n\r\n",
            code, reason, _inVia.c_str(), _inFrom.c_str(), _inTo.c_str(), _ourToTag.c_str(),
            _inCallId.c_str(), _inCseqInvite.c_str());
    }
    return std::string(msg);
}

// Generic 200 OK for an in-dialog request we didn't originate (BYE/CANCEL),
// echoing its own Via/From/To/Call-ID/CSeq verbatim -- those already carry
// both parties' tags since it's in-dialog, so no tag needs adding here.
// Same "getters already include the field name" caveat as
// buildInviteResponse() above applies to via/from/to/callId/cseq here.
std::string TincanUac::buildGenericResponse(const std::string &via, const std::string &from,
                                             const std::string &to, const std::string &callId,
                                             const std::string &cseq, int code, const char *reason) const
{
    char msg[768];
    std::snprintf(msg, sizeof(msg),
        "SIP/2.0 %d %s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "Content-Length: 0\r\n\r\n",
        code, reason, via.c_str(), from.c_str(), to.c_str(), callId.c_str(), cseq.c_str());
    return std::string(msg);
}

// ── REGISTER ─────────────────────────────────────────────────────────────--
bool TincanUac::registerExt()
{
    const std::string reg = buildRegister();
    ESP_LOGI(TAG, "REGISTER %s -> %s:%d", _selfExt.c_str(), _serverIp.c_str(), _serverPort);

    char rbuf[2048];
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (attempt % 2 == 0) sendToServer(reg);

        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        int n = recvfrom(_sipSock, rbuf, sizeof(rbuf) - 1, 0, (sockaddr *)&src, &sl);
        if (n <= 0) continue;
        rbuf[n] = '\0';

        SipMessageFactory factory;
        auto parsed = factory.createMessage(std::string(rbuf, n), src);
        if (!parsed.has_value()) continue;
        int code = statusCode(parsed.value()->getType());
        if (code == 200) { ESP_LOGI(TAG, "registered as %s", _selfExt.c_str()); return true; }
        if (code >= 400) { ESP_LOGE(TAG, "REGISTER rejected (%d)", code); return false; }
    }
    ESP_LOGW(TAG, "no REGISTER response from %s:%d", _serverIp.c_str(), _serverPort);
    return false;
}

// ── outbound INVITE (via server) ────────────────────────────────────────--
bool TincanUac::placeCall(const std::string &calleeExt)
{
    if (_state != State::Idle) { ESP_LOGW(TAG, "placeCall() while not idle"); return false; }

    resetDialog();
    _peerExt = calleeExt;
    // _callId holds the bare Call-ID VALUE ("id@host"), not a raw header
    // line -- consistent with what answer() stores for inbound-established
    // calls (see its own comment) so BYE-matching and rebuilding in-dialog
    // requests both work the same way regardless of call direction.
    _callId = IDGen::GenerateID(16) + "@" + _localIp;
    _fromTag = IDGen::GenerateID(8);
    _state = State::Calling;

    const std::string invite = buildInvite(calleeExt);
    ESP_LOGI(TAG, "INVITE %s via %s:%d", calleeExt.c_str(), _serverIp.c_str(), _serverPort);

    char rbuf[2048];
    // Wait up to ~120s for the callee to answer. Retransmit only in the
    // first few seconds (UDP-loss guard); once ringing, just wait.
    for (int attempt = 0; attempt < 240; ++attempt) {
        if (attempt < 6 && attempt % 2 == 0) sendToServer(invite);

        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        int n = recvfrom(_sipSock, rbuf, sizeof(rbuf) - 1, 0, (sockaddr *)&src, &sl);
        if (n <= 0) continue;
        rbuf[n] = '\0';

        SipMessageFactory factory;
        auto parsed = factory.createMessage(std::string(rbuf, n), src);
        if (!parsed.has_value()) continue;
        auto msg = parsed.value();

        int code = statusCode(msg->getType());
        ESP_LOGI(TAG, "<- %.*s", (int)msg->getType().size(), msg->getType().data());

        if (code == 100 || code == 180 || code == 183) continue;
        if (code == 200) {
            _remoteTag = extractParam(msg->getTo(), ";tag=");
            if (msg->hasSdp()) {
                auto *sdp = static_cast<SipSdpMessage *>(msg.get());
                _remoteRtpPort = sdp->getRtpPort();
                _remoteRtpIp = ipFromConnection(sdp->getConnectionInformation());
            }
            sendToServer(buildAck());
            _rtpSeq = (uint16_t)esp_random();
            _rtpTs = esp_random();
            _rtpSsrc = esp_random();
            _state = State::InCall;
            ESP_LOGI(TAG, "200 OK -> ACK; media to %s:%d", _remoteRtpIp.c_str(), _remoteRtpPort);
            return _remoteRtpPort > 0 && !_remoteRtpIp.empty();
        }
        if (code >= 400) {
            ESP_LOGW(TAG, "call to %s rejected (%d)", calleeExt.c_str(), code);
            _state = State::Idle;
            return false;
        }
    }
    ESP_LOGW(TAG, "no final response to INVITE (is %s registered?)", calleeExt.c_str());
    _state = State::Idle;
    return false;
}

// ── inbound handling ─────────────────────────────────────────────────────--
void TincanUac::handleInboundInvite(const std::string &raw)
{
    if (_state != State::Idle) {
        // Already on a call -- decline politely rather than silently drop.
        sockaddr_in dummy{};
        SipMessageFactory factory;
        auto parsed = factory.createMessage(raw, dummy);
        if (!parsed.has_value()) return;
        auto msg = parsed.value();
        std::string resp = buildGenericResponse(std::string(msg->getVia()), std::string(msg->getFrom()),
                                                  std::string(msg->getTo()) + ";tag=" + IDGen::GenerateID(8),
                                                  std::string(msg->getCallID()), std::string(msg->getCSeq()),
                                                  486, "Busy Here");
        sendToServer(resp);
        return;
    }

    sockaddr_in dummy{};
    SipMessageFactory factory;
    auto parsed = factory.createMessage(raw, dummy);
    if (!parsed.has_value()) return;
    auto msg = parsed.value();

    resetDialog();
    _inVia = std::string(msg->getVia());
    _inFrom = std::string(msg->getFrom());
    _inTo = std::string(msg->getTo());
    _inCallId = std::string(msg->getCallID());
    _inCseqInvite = std::string(msg->getCSeq());
    _peerExt = std::string(msg->getFromNumber());
    _ourToTag = IDGen::GenerateID(8);

    if (msg->hasSdp()) {
        auto *sdp = static_cast<SipSdpMessage *>(msg.get());
        _remoteRtpPort = sdp->getRtpPort();
        _remoteRtpIp = ipFromConnection(sdp->getConnectionInformation());
    }

    _state = State::Ringing;
    ESP_LOGI(TAG, "<- INVITE from %s (caller %s)", _peerExt.c_str(), _inFrom.c_str());
    sendToServer(buildInviteResponse(180, "Ringing", false));
}

void TincanUac::handleInboundBye(const std::string &raw)
{
    sockaddr_in dummy{};
    SipMessageFactory factory;
    auto parsed = factory.createMessage(raw, dummy);
    if (!parsed.has_value()) return;
    auto msg = parsed.value();

    // Always ack the BYE, whether or not it matches a call we think is
    // active -- an unmatched BYE is still owed a 200 OK by the transaction
    // layer contract, and silently dropping it (as tincan's PoC did) just
    // makes the peer retransmit.
    sendToServer(buildGenericResponse(std::string(msg->getVia()), std::string(msg->getFrom()),
                                       std::string(msg->getTo()), std::string(msg->getCallID()),
                                       std::string(msg->getCSeq()), 200, "OK"));

    // getCallID() returns the raw header line ("Call-ID: id@host"); _callId
    // holds just the bare value, so strip the prefix before comparing.
    if (_state == State::InCall && headerValue(msg->getCallID()) == _callId) {
        ESP_LOGI(TAG, "<- BYE (peer hung up)");
        resetDialog();
        _state = State::Idle;
        _callEndedPending = true;
    }
}

void TincanUac::handleInboundCancel(const std::string &raw)
{
    sockaddr_in dummy{};
    SipMessageFactory factory;
    auto parsed = factory.createMessage(raw, dummy);
    if (!parsed.has_value()) return;
    auto msg = parsed.value();

    sendToServer(buildGenericResponse(std::string(msg->getVia()), std::string(msg->getFrom()),
                                       std::string(msg->getTo()), std::string(msg->getCallID()),
                                       std::string(msg->getCSeq()), 200, "OK"));

    if (_state == State::Ringing && std::string(msg->getCallID()) == _inCallId) {
        ESP_LOGI(TAG, "<- CANCEL (caller gave up)");
        sendToServer(buildInviteResponse(487, "Request Terminated", false));
        resetDialog();
        _state = State::Idle;
        _callEndedPending = true;
    }
}

void TincanUac::poll()
{
    char rbuf[2048];
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    int n = recvfrom(_sipSock, rbuf, sizeof(rbuf) - 1, MSG_DONTWAIT, (sockaddr *)&src, &sl);
    if (n <= 0) return;
    rbuf[n] = '\0';
    std::string raw(rbuf, n);

    // registerExt()/placeCall() drain their own responses inline and never
    // run concurrently with poll() on this single-threaded loop, so we only
    // need to handle unsolicited requests here.
    if (raw.rfind("INVITE ", 0) == 0) handleInboundInvite(raw);
    else if (raw.rfind("BYE ", 0) == 0) handleInboundBye(raw);
    else if (raw.rfind("CANCEL ", 0) == 0) handleInboundCancel(raw);
    // ACK and any other in-dialog chatter: nothing else to do for this PoC.
}

bool TincanUac::answer()
{
    if (_state != State::Ringing) { ESP_LOGW(TAG, "answer() with no ringing call"); return false; }

    sendToServer(buildInviteResponse(200, "OK", true));

    // Adopt the inbound dialog as the active one, in the same fields
    // placeCall() uses, so buildBye()/buildAck() work uniformly regardless
    // of call direction. _inCallId is the raw header line ("Call-ID:
    // id@host") captured for echoing verbatim in the INVITE response above
    // -- strip the prefix here so _callId consistently holds just the bare
    // value, matching what placeCall() stores and what buildBye()/
    // handleInboundBye()'s matching now expect regardless of call direction.
    _callId = headerValue(_inCallId);
    _fromTag = _ourToTag;
    _remoteTag = extractParam(_inFrom, ";tag=");

    _rtpSeq = (uint16_t)esp_random();
    _rtpTs = esp_random();
    _rtpSsrc = esp_random();
    _state = State::InCall;
    ESP_LOGI(TAG, "answered; media to %s:%d", _remoteRtpIp.c_str(), _remoteRtpPort);
    return true;
}

void TincanUac::reject()
{
    if (_state != State::Ringing) return;
    sendToServer(buildInviteResponse(486, "Busy Here", false));
    resetDialog();
    _state = State::Idle;
}

bool TincanUac::callEnded()
{
    bool v = _callEndedPending;
    _callEndedPending = false;
    return v;
}

void TincanUac::hangup()
{
    switch (_state) {
        case State::InCall:
            sendToServer(buildBye());
            ESP_LOGI(TAG, "BYE sent");
            break;
        case State::Calling:
            sendToServer(buildCancel());
            ESP_LOGI(TAG, "CANCEL sent (gave up waiting for answer)");
            break;
        case State::Ringing:
            reject();
            return; // reject() already resets state without setting callEnded
        default:
            return;
    }
    resetDialog();
    _state = State::Idle;
}

// ── media ────────────────────────────────────────────────────────────────--
void TincanUac::sendAudioFrame(const int16_t *pcm, size_t samples)
{
    if (_state != State::InCall || samples == 0) return;
    uint8_t pkt[RTP_HEADER_LEN + POC_FRAME_SAMPLES];
    if (samples > POC_FRAME_SAMPLES) samples = POC_FRAME_SAMPLES;

    g711_ulaw_encode_buf(pcm, pkt + RTP_HEADER_LEN, samples);
    rtp_write_header(pkt, POC_RTP_PAYLOAD_PCMU, 0, _rtpSeq, _rtpTs, _rtpSsrc);
    _rtpSeq++;
    _rtpTs += samples;

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr(_remoteRtpIp.c_str());
    dst.sin_port = htons(_remoteRtpPort);
    sendto(_rtpSock, pkt, RTP_HEADER_LEN + samples, 0, (const sockaddr *)&dst, sizeof(dst));
}

size_t TincanUac::recvAudioFrame(int16_t *pcm, size_t maxSamples)
{
    if (_state != State::InCall) return 0;
    uint8_t rx[RTP_HEADER_LEN + POC_FRAME_SAMPLES * 2];
    int n = recvfrom(_rtpSock, rx, sizeof(rx), MSG_DONTWAIT, nullptr, nullptr);
    if (n <= (int)RTP_HEADER_LEN) return 0;

    size_t plen = n - RTP_HEADER_LEN;
    if (plen > maxSamples) plen = maxSamples;
    g711_ulaw_decode_buf(rx + RTP_HEADER_LEN, pcm, plen);
    return plen;
}
