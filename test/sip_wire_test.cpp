// Host-side wire-format test for tincan_uac's UAS response builders.
//
// Purpose: Blocker 1 from the pre-hardware review was a wire-format bug --
// code that compiles perfectly but puts malformed bytes on the network.
// A firmware build proves nothing about it. This harness compiles the REAL
// vendored sip_core parser on the host, feeds it a realistic INVITE, pulls
// the fields out exactly as TincanUac::handleInboundInvite() does, and runs
// them through byte-identical copies of the fixed format strings from
// TincanUac::buildInviteResponse() / buildGenericResponse().
//
// It asserts on the resulting bytes: no doubled field names, and every
// header RFC 3261 requires in a response is present exactly once.
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <vector>

#include "SipMessageFactory.hpp"
#include "SipSdpMessage.hpp"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        std::cout << "  FAIL: " << what << "\n";
    } else {
        std::cout << "  ok:   " << what << "\n";
    }
}

// Mirrors the headerValue() helper added to tincan_uac.cpp.
static std::string headerValue(std::string_view line)
{
    size_t p = line.find(':');
    if (p == std::string_view::npos) return std::string(line);
    p++;
    while (p < line.size() && line[p] == ' ') p++;
    return std::string(line.substr(p));
}

// Mirrors the displayName() helper added to tincan_uac.cpp for #48.
static std::string displayName(std::string_view header)
{
    size_t q1 = header.find('"');
    if (q1 == std::string_view::npos) return {};
    size_t q2 = header.find('"', q1 + 1);
    if (q2 == std::string_view::npos || q2 == q1 + 1) return {};
    return std::string(header.substr(q1 + 1, q2 - q1 - 1));
}

// Mirrors tincan_uac.cpp's ipFromConnection(): last token of "IN IP4 x.x.x.x".
static std::string ipFromConnection(std::string_view conn)
{
    size_t sp = conn.rfind(' ');
    return (sp == std::string_view::npos) ? std::string(conn)
                                          : std::string(conn.substr(sp + 1));
}

// Counts non-overlapping occurrences of `needle` in `hay`.
static int countOf(const std::string &hay, const std::string &needle)
{
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        n++;
    return n;
}

// A realistic inbound INVITE, of the shape drawbridge forks to registered
// extensions: quoted display name, ;rport on Via, multi-codec SDP, and a
// c= line on a different IP than the signalling source.
static const char *kInboundInvite =
    "INVITE sip:1002@192.168.1.50:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK8a7f6e5d;rport\r\n"
    "Max-Forwards: 70\r\n"
    "From: \"Front Desk\" <sip:1001@192.168.1.10>;tag=abc123\r\n"
    "To: <sip:1002@192.168.1.10>\r\n"
    "Call-ID: 9f8e7d6c5b4a@192.168.1.10\r\n"
    "CSeq: 1 INVITE\r\n"
    "Contact: <sip:1001@192.168.1.10:5060;transport=UDP>\r\n"
    "Content-Type: application/sdp\r\n"
    "Content-Length: 210\r\n"
    "\r\n"
    "v=0\r\n"
    "o=- 0 0 IN IP4 192.168.1.10\r\n"
    "s=drawbridge\r\n"
    "c=IN IP4 192.168.1.99\r\n"
    "t=0 0\r\n"
    "m=audio 40000 RTP/AVP 0 8 101\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=sendrecv\r\n";

int main()
{
    sockaddr_in src{};
    SipMessageFactory factory;
    auto parsed = factory.createMessage(std::string(kInboundInvite), src);
    if (!parsed.has_value()) {
        std::cout << "FATAL: parser rejected the INVITE\n";
        return 2;
    }
    auto msg = parsed.value();

    // --- Part 1: confirm the premise the fix rests on -------------------
    // The review claimed sip_core's getters return the raw header line
    // INCLUDING the field name. Verify that directly rather than trusting it.
    std::cout << "[premise] getters return field name + value:\n";
    std::string via    = std::string(msg->getVia());
    std::string from   = std::string(msg->getFrom());
    std::string to     = std::string(msg->getTo());
    std::string callId = std::string(msg->getCallID());
    std::string cseq   = std::string(msg->getCSeq());

    check(via.rfind("Via:", 0) == 0,        "getVia() includes \"Via:\" prefix");
    check(from.rfind("From:", 0) == 0,      "getFrom() includes \"From:\" prefix");
    check(to.rfind("To:", 0) == 0,          "getTo() includes \"To:\" prefix");
    check(callId.rfind("Call-ID:", 0) == 0, "getCallID() includes \"Call-ID:\" prefix");
    check(cseq.rfind("CSeq:", 0) == 0,      "getCSeq() includes \"CSeq:\" prefix");

    // --- Part 2: the fixed buildInviteResponse() format string ----------
    // Byte-identical to tincan_uac.cpp's (no literal "Via: "/"From: "/etc).
    const std::string ourToTag = "Ab3xY9zQ";
    const std::string selfExt  = "1002";
    const std::string localIp  = "192.168.1.50";
    const int localSipPort     = 5060;
    const int localRtpPort     = 4000;

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
        selfExt.c_str(), localIp.c_str(), localIp.c_str(),
        localRtpPort, 0, 0);

    char msg200[1280];
    std::snprintf(msg200, sizeof(msg200),
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
        200, "OK", via.c_str(), from.c_str(), to.c_str(), ourToTag.c_str(),
        callId.c_str(), cseq.c_str(),
        selfExt.c_str(), localIp.c_str(), localSipPort,
        sdpLen, sdp);
    std::string ok200(msg200);

    std::cout << "\n[200 OK] generated bytes:\n----\n" << ok200 << "----\n";
    std::cout << "[200 OK] assertions:\n";
    check(countOf(ok200, "Via: Via:") == 0,           "no doubled \"Via: Via:\"");
    check(countOf(ok200, "From: From:") == 0,         "no doubled \"From: From:\"");
    check(countOf(ok200, "To: To:") == 0,             "no doubled \"To: To:\"");
    check(countOf(ok200, "Call-ID: Call-ID:") == 0,   "no doubled \"Call-ID: Call-ID:\"");
    check(countOf(ok200, "CSeq: CSeq:") == 0,         "no doubled \"CSeq: CSeq:\"");
    check(countOf(ok200, "\r\nVia:") == 1,            "exactly one Via header");
    check(countOf(ok200, "\r\nFrom:") == 1,           "exactly one From header");
    check(countOf(ok200, "\r\nTo:") == 1,             "exactly one To header");
    check(countOf(ok200, "\r\nCall-ID:") == 1,        "exactly one Call-ID header");
    check(countOf(ok200, "\r\nCSeq:") == 1,           "exactly one CSeq header");
    check(ok200.rfind("SIP/2.0 200 OK\r\n", 0) == 0,  "status line is first");
    check(ok200.find(";tag=" + ourToTag) != std::string::npos, "our To-tag present");
    check(ok200.find("branch=z9hG4bK8a7f6e5d") != std::string::npos, "Via branch echoed");
    check(ok200.find("tag=abc123") != std::string::npos, "caller's From-tag echoed");

    // The response must be re-parseable by the same parser -- the real
    // proof it's well-formed, not just non-doubled.
    sockaddr_in src2{};
    SipMessageFactory f2;
    auto reparsed = f2.createMessage(ok200, src2);
    check(reparsed.has_value(), "200 OK re-parses cleanly");
    if (reparsed.has_value()) {
        auto r = reparsed.value();
        check(headerValue(r->getCallID()) == "9f8e7d6c5b4a@192.168.1.10",
              "re-parsed Call-ID matches the request's");
        check(std::string(r->getType()).rfind("SIP/2.0 200", 0) == 0,
              "re-parsed type is a 200 response");
        check(r->hasSdp(), "re-parsed 200 OK carries SDP");
    }

    // --- Part 3: buildGenericResponse() (BYE/CANCEL ack path) -----------
    char msgBye[768];
    std::snprintf(msgBye, sizeof(msgBye),
        "SIP/2.0 %d %s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "%s\r\n"
        "Content-Length: 0\r\n\r\n",
        200, "OK", via.c_str(), from.c_str(), to.c_str(), callId.c_str(), cseq.c_str());
    std::string byeOk(msgBye);

    std::cout << "\n[BYE 200 OK] assertions:\n";
    check(countOf(byeOk, "Via: Via:") == 0,         "no doubled Via");
    check(countOf(byeOk, "Call-ID: Call-ID:") == 0, "no doubled Call-ID");
    check(countOf(byeOk, "\r\nCSeq:") == 1,         "exactly one CSeq header");
    sockaddr_in src3{};
    SipMessageFactory f3;
    check(f3.createMessage(byeOk, src3).has_value(), "BYE 200 OK re-parses cleanly");

    // --- Part 4: the Call-ID normalization bug found beyond the review ---
    // _callId must hold the BARE value so handleInboundBye()'s match works.
    // Before the fix, answer() stored the raw header line while placeCall()
    // stored a bare id, so the comparison never matched in either direction
    // and the phone never noticed a peer hangup.
    std::cout << "\n[Call-ID normalization] assertions:\n";
    std::string bare = headerValue(callId);
    check(bare == "9f8e7d6c5b4a@192.168.1.10", "headerValue() strips the \"Call-ID: \" prefix");
    check(bare.find("Call-ID") == std::string::npos, "bare value carries no field name");
    // Simulate an inbound BYE for this dialog and confirm the match succeeds.
    std::string byeReq =
        "BYE sip:1002@192.168.1.50:5060 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bKdeadbeef\r\n"
        "From: \"Front Desk\" <sip:1001@192.168.1.10>;tag=abc123\r\n"
        "To: <sip:1002@192.168.1.10>;tag=" + ourToTag + "\r\n"
        "Call-ID: 9f8e7d6c5b4a@192.168.1.10\r\n"
        "CSeq: 2 BYE\r\n"
        "Content-Length: 0\r\n\r\n";
    sockaddr_in src4{};
    SipMessageFactory f4;
    auto byeParsed = f4.createMessage(byeReq, src4);
    check(byeParsed.has_value(), "inbound BYE parses");
    if (byeParsed.has_value()) {
        // _callId (bare, as placeCall()/answer() now store it) vs the BYE's.
        check(headerValue(byeParsed.value()->getCallID()) == bare,
              "BYE Call-ID matches stored _callId (hangup IS detected)");
        // And demonstrate the old broken comparison would have failed.
        check(!(std::string(byeParsed.value()->getCallID()) == bare),
              "raw-vs-bare compare would NOT have matched (the old bug)");
    }

    // --- Delayed-offer inbound (#48) ------------------------------------
    // drawbridge's inbound ring-all forks an INVITE with NO SDP -- it can't
    // advertise a media-bridge port before the bridge binds -- takes our offer
    // from the 200 OK, and puts its answer in the ACK. The firmware used to
    // learn the RTP endpoint only from the INVITE, so on this path it armed
    // the audio task with an unset endpoint and the far end heard silence.
    //
    // These assertions pin the sip_core behaviour that fix depends on. They
    // are here rather than in a tincan_uac test because that file needs
    // ESP-IDF headers; the parser invariants are what could silently regress.
    std::cout << "\n[delayed offer] assertions:\n";

    // Byte-shape of RequestsHandler::buildInboundInviteFork(): the PSTN caller
    // is ONLY in the display name, and the From/To URI user is OUR OWN ext.
    const char *kDelayedInvite =
        "INVITE sip:1002@192.168.1.50:5060 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bKfeedface\r\n"
        "From: \"3125551234\" <sip:1002@192.168.1.10:5060>;tag=inb99\r\n"
        "To: <sip:1002@192.168.1.10>\r\n"
        "Call-ID: inbound-anchor-1@192.168.1.10\r\n"
        "CSeq: 1 INVITE\r\n"
        "Max-Forwards: 70\r\n"
        "Contact: <sip:1002@192.168.1.10:5060;transport=UDP>\r\n"
        "Content-Length: 0\r\n\r\n";

    sockaddr_in src5{};
    SipMessageFactory f5;
    auto dParsed = f5.createMessage(std::string(kDelayedInvite), src5);
    check(dParsed.has_value(), "delayed-offer INVITE parses");
    if (dParsed.has_value()) {
        auto d = dParsed.value();
        check(!d->hasSdp(), "delayed-offer INVITE reports hasSdp() == false");
        // This is the whole bug: learnRemoteMedia() must return false here so
        // answer() does NOT publish a media target it doesn't have yet.
        check(std::string(d->getFromNumber()) == "1002",
              "From URI user is our OWN ext (why caller ID needed a fallback)");
        check(displayName(d->getFrom()) == "3125551234",
              "display name carries the real PSTN caller");
    }

    // The ACK that actually carries the answer. The fix static_casts to
    // SipSdpMessage after checking hasSdp(); that is only sound because the
    // factory types by body content, not by method -- assert it for an ACK.
    const std::string kAckWithSdp =
        "ACK sip:1002@192.168.1.50:5060 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bKfeedface\r\n"
        "From: \"3125551234\" <sip:1002@192.168.1.10:5060>;tag=inb99\r\n"
        "To: <sip:1002@192.168.1.10>;tag=" + ourToTag + "\r\n"
        "Call-ID: inbound-anchor-1@192.168.1.10\r\n"
        "CSeq: 1 ACK\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: 129\r\n"
        "\r\n"
        "v=0\r\n"
        "o=- 0 0 IN IP4 192.168.1.10\r\n"
        "s=drawbridge\r\n"
        "c=IN IP4 192.168.1.77\r\n"
        "t=0 0\r\n"
        "m=audio 41234 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";

    sockaddr_in src6{};
    SipMessageFactory f6;
    auto aParsed = f6.createMessage(kAckWithSdp, src6);
    check(aParsed.has_value(), "ACK carrying SDP parses");
    if (aParsed.has_value()) {
        auto a = aParsed.value();
        check(a->hasSdp(), "ACK reports hasSdp() == true (factory types by body, not method)");
        auto *asdp = dynamic_cast<SipSdpMessage *>(a.get());
        check(asdp != nullptr, "ACK really IS a SipSdpMessage (the static_cast is sound)");
        if (asdp) {
            check(asdp->getRtpPort() == 41234, "ACK's SDP yields the far-end RTP port");
            check(ipFromConnection(asdp->getConnectionInformation()) == "192.168.1.77",
                  "ACK's c= line yields the far-end RTP IP");
        }
        check(headerValue(a->getCallID()) == "inbound-anchor-1@192.168.1.10",
              "ACK Call-ID matches the dialog (handleInboundAck accepts it)");
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
