# Architecture & System Design (`tdeck-max-phone`)

> **Note (2026-08):** this document previously described an on-device
> `pocket-dial` SIP PBX and a direct 3CX Route Point API integration running
> on this board. Neither was ever implemented (no `pocket-dial` source file
> ever existed in this repo), and the implemented PoC uses a different,
> simpler architecture described below. This file is corrected to match
> what's actually built, not what was originally scoped.

## 1. Actual architecture: LAN SIP extension, PBX/3CX externalized

`tdeck-max-phone` is a plain SIP UAC/UAS (`main/src/tincan_uac.cpp`,
`main/include/tincan_uac.hpp`) that registers as a normal extension to a
[drawbridge](https://github.com/GlomarGadaffi/drawbridge) PBX instance
running elsewhere on the LAN. Drawbridge owns *all* 3CX Call Control API
integration (OAuth2, WebSocket call-control events, RTP media anchoring)
-- this device never talks to 3CX directly and has no OAuth/HTTPS client of
its own. `TDeckMaxAudioAnchor.cpp/.hpp` (a stub 3CX Route Point client) is
kept in the tree as reference but excluded from the build.

```
tdeck-max-phone                      drawbridge PBX                    3CX
+----------------+   SIP (LAN)   +----------------------+   OAuth2/WSS/REST   +--------+
| tincan_uac.cpp |<------------->| RequestsHandler +     |<------------------->| 3CX PBX|
| ES8311 codec   |   RTP (G.711) | TelephonyAnchorClient |   Call Control API  +--------+
| TCA8418 keypad |               +----------------------+
| GDEQ031T10 e-paper|
+----------------+
```

- **Outbound**: dialing `9<number>` from the keypad is a plain SIP INVITE to drawbridge; drawbridge strips the `9` and calls out through its `TelephonyAnchorClient` (`drawbridge/src/SIP/RequestsHandler.cpp`, `onInvite` ~line 923).
- **Inbound**: an incoming 3CX call makes drawbridge RING-ALL every registered extension via an offerless INVITE fork (`RequestsHandler::routeInboundAnchorCall`, ~line 7084) -- first answer wins. No per-extension routing config is needed on drawbridge's side for this device to receive calls.
- **Media**: RTP flows directly between this device and whichever party it's actually talking to -- drawbridge only anchors media for the PSTN/3CX leg, not for LAN-to-LAN calls, and this device isn't a LAN-to-LAN peer in the 3CX PoC path anyway (the other party is drawbridge's `MediaBridge` for the 3CX leg).

## 2. On-device task model

There is no dual-core task split. `app_main()` (`main/src/app_main.cpp`) runs
a single loop on the main task: `uac.poll()` (non-blocking SIP message
drain) → keypad poll → per-UI-state dispatch (dial buffer / incoming-call
prompt / in-call full-duplex audio pump) → ~20ms delay (one G.711 frame
period). `TincanUac::registerExt()` and `TincanUac::placeCall()` are the
only blocking calls (bounded, ~6s and ~120s worst case respectively) --
see the "known limitation" note on `placeCall()` in `tincan_uac.hpp` for
what that costs during dial-out.

## 3. Audio pipeline

```
Local Mic  ---> ES8311 Codec (I2S, full-duplex) ---> G.711 encode ---> RTP ---> drawbridge/peer
Local Spk  <--- ES8311 Codec (I2S, full-duplex) <--- G.711 decode <--- RTP <--- drawbridge/peer
```

8 kHz mono G.711 µ-law (PCMU, RTP payload type 0), 20 ms frames (160
samples) -- drawbridge does not transcode, so this must match what it
expects. The ES8311 I2S driver runs the mic and speaker simultaneously out
of the box (confirmed via ESP-IDF log: "the rx channel on I2S0 is switched
from master to slave for full-duplex mode"), so this phone doesn't need
tincan's half-duplex push-to-talk compromise.

## 4. What's not implemented

- No cellular/4G PPP netif -- this PoC is Wi-Fi only (`main/src/net_wifi.c`, ported from tincan).
- No direct 3CX integration on this device (see above) -- `TDeckMaxAudioAnchor` is dead code, kept for reference.
- Full alphabet keypad entry, real status icons, and e-paper partial refresh are deferred UI polish -- see the GitHub issue tracker.
