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
