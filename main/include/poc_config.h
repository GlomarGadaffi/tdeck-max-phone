#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  PoC configuration.
//  (Kept as #defines for spike simplicity, matching tincan's poc_config.h.)
//
//  Topology: this device registers as a plain SIP extension to a drawbridge
//  PBX instance on the LAN. Drawbridge owns all 3CX Call Control API
//  integration (OAuth2/WS/REST) — this device never talks to 3CX directly.
//  Outbound: dialing 9<number> from the keypad routes out through 3CX.
//  Inbound: drawbridge RING-ALLs registered extensions on an incoming 3CX
//  call; this device just needs to be registered to receive it.
//
//  ── DO NOT PUT REAL CREDENTIALS IN THIS FILE ──
//  This file is tracked in git (and this repo is public). Copy
//  poc_secrets.h.example to poc_secrets.h and put real values there --
//  poc_secrets.h is gitignored, and anything it defines wins over the
//  placeholder defaults below. Nothing else needs changing.
// ─────────────────────────────────────────────────────────────────────────

// Pull in local, untracked overrides if the developer created them. Guarded
// by __has_include so a fresh clone with no poc_secrets.h still compiles.
#if defined(__has_include)
#  if __has_include("poc_secrets.h")
#    include "poc_secrets.h"
#    define POC_SECRETS_PRESENT 1
#  endif
#endif

#ifndef POC_SECRETS_PRESENT
#  define POC_SECRETS_PRESENT 0
#endif

// ── Site-specific settings (override these in poc_secrets.h) ─────────────

// Wi-Fi STA credentials.
#ifndef POC_WIFI_SSID
#define POC_WIFI_SSID        "CHANGE_ME"
#endif
#ifndef POC_WIFI_PASS
#define POC_WIFI_PASS        "CHANGE_ME"
#endif

// drawbridge SIP server (registrar/PBX) running on the LAN.
#ifndef POC_SIP_SERVER_IP
#define POC_SIP_SERVER_IP    "CHANGE_ME"
#endif
#ifndef POC_SIP_SERVER_PORT
#define POC_SIP_SERVER_PORT  5060
#endif

// Our identity. drawbridge's registrar is open by default (self-registering,
// no pre-created extensions needed) — pick any free extension number.
#ifndef POC_SIP_EXT_SELF
#define POC_SIP_EXT_SELF     "1002"
#endif
#ifndef POC_SIP_REG_EXPIRES
#define POC_SIP_REG_EXPIRES  3600
#endif

// Bench-test speed dial. TEMPORARY WORKAROUND for #17: the keypad can only
// produce '0', DEL and ENT today, so a target containing '*' or digits 1-9
// cannot be typed. When this is defined, pressing ENT from the Idle screen
// dials it immediately instead of opening an empty dial buffer.
//
//   "777"    drawbridge's own SDP loopback echo -- local, no 3CX leg.
//            Use this FIRST: it isolates mic/speaker/RTP from the trunk.
//   "9*777"  3CX's echo test via the anchor (drawbridge strips the leading
//            '9' and sends *777 onward). Use this SECOND, once local audio
//            is proven.
//
// Comment out to restore normal dial-buffer behaviour.
#ifndef POC_TEST_DIAL
#define POC_TEST_DIAL        "777"
#endif

// ── Fixed protocol/hardware settings (not site-specific) ─────────────────

// Local ports.
#ifndef POC_SIP_LOCAL_PORT
#define POC_SIP_LOCAL_PORT   5060
#endif
#ifndef POC_RTP_LOCAL_PORT
#define POC_RTP_LOCAL_PORT   4000             // even port; RTCP would be +1
#endif

// Audio: G.711 µ-law (PCMU, payload type 0), 8 kHz mono, 20 ms frames —
// drawbridge does not transcode, so this must match what it expects.
#define POC_SAMPLE_RATE_HZ   8000
#define POC_FRAME_SAMPLES    160              // 8000 Hz * 0.020 s
#define POC_RTP_PAYLOAD_PCMU 0

// Gain staging. Speaker and mic sit a few centimetres apart on the same PCB
// with no acoustic isolation, so this is a loudspeaking phone with a wide
// open echo path and no AEC. Loop gain above unity makes the *777 echo test
// regenerate into a howl that builds on itself -- expected for an echo
// service, not a fault, but it also means the defaults want headroom.
//
// The mic PGA was briefly 30 dB during bring-up while the ADC was reading
// bit-exact zero (see #34) and that turned out to be a wiring fault, not a
// level problem. 30 dB is far hotter than this mic needs: it put ordinary
// room noise at roughly -13 dBFS peak.
//
// ES8311 mic PGA is quantised to 6 dB steps (0/6/12/.../42), so only
// multiples of 6 are meaningful here.
// 30 dB: the far end reported a quiet mic at 24 dB on a real 3CX call.
//
// This is the same level that made the *777 echo test regenerate into a
// howl, and that is fine -- *777 is an echo service, so it deliberately
// closes mic -> RTP -> PBX -> RTP -> speaker -> mic. A normal call does not,
// because the far end is not echoing you back. If you go back to bench-
// testing against *777, drop this to 18 or use headphones rather than
// leaving it low for real calls. 36 dB is the next notch if still quiet.
#ifndef POC_MIC_GAIN_DB
#define POC_MIC_GAIN_DB      30.0f            // 0..42 in 6 dB steps
#endif
#ifndef POC_SPK_VOLUME
#define POC_SPK_VOLUME       80               // 0..100
#endif
