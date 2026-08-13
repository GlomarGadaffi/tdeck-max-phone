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
   tdeck-max-phone                    drawbridge PBX                      3CX
+---------------------+           +-----------------------+          +----------+
| tincan_uac.cpp      |  SIP/LAN  | RequestsHandler       |  OAuth2  |          |
| ES8311 codec        |<--------->|         +             |<-------->| 3CX PBX  |
| TCA8418 keypad      | RTP G.711 | TelephonyAnchorClient |  WSS/REST|          |
| GDEQ031T10 e-paper  |           +-----------------------+          +----------+
+---------------------+
   no 3CX credentials                owns all 3CX integration
```

- **Outbound**: dialing `9<number>` from the keypad is a plain SIP INVITE to drawbridge; drawbridge strips the `9` and calls out through its `TelephonyAnchorClient` (`drawbridge/src/SIP/RequestsHandler.cpp`, `onInvite` ~line 923).
- **Inbound**: an incoming 3CX call makes drawbridge RING-ALL every registered extension via an offerless INVITE fork (`RequestsHandler::routeInboundAnchorCall`, ~line 7084) -- first answer wins. No per-extension routing config is needed on drawbridge's side for this device to receive calls.
- **Media**: RTP flows directly between this device and whichever party it's actually talking to -- drawbridge only anchors media for the PSTN/3CX leg, not for LAN-to-LAN calls, and this device isn't a LAN-to-LAN peer in the 3CX PoC path anyway (the other party is drawbridge's `MediaBridge` for the 3CX leg).

## 2. On-device task model

Three tasks (`main/src/app_main.cpp`):

| Task | Core | Prio | Responsibility |
|---|---|---|---|
| `main_task` | any | 1 | SIP control (`uac.poll()`), registration refresh, keypad, UI state machine |
| `audio` | 1 (pinned) | 6 | RTP ↔ I2S pump, paced **solely** by the blocking `i2s_channel_read()` |
| `epaper` | any | 3 | The 2-3 s panel refresh, fed by a depth-1 `xQueueOverwrite` |

Audio and display each originally ran inline on the main loop. That was
wrong in both cases and the symptoms were distinct:

- Audio: one 20 ms frame per iteration *plus* a `vTaskDelay(20ms)` stacked
  on top of an already-blocking I2S read gave ~20 packets/sec outbound
  instead of 50, while the inbound socket backlog grew ~30 packets/sec --
  so one-way latency climbed monotonically for the whole call.
- Display: a synchronous 2-3 s refresh on every state change *and every
  dialled digit* blacked out audio for seconds at a time.

The audio task therefore adds **no** delay of its own -- the blocking I2S
read is the clock. Priority is above the main loop because starving audio is
audible and starving the UI is not; it is pinned to core 1 to keep it off
core 0's Wi-Fi/lwIP work.

**Threading contract:** only `sendAudioFrame()`, `recvAudioFrame()` and
`flushRtpBacklog()` may be called from the audio task. They read the media
endpoint as POD (`_mediaIpBe`/`_mediaPortBe`) published with release/acquire
ordering around an atomic flag, never the `std::string` fields the control
path reassigns -- a torn read there would be a crash, not a glitch.

`TincanUac::registerExt()` and `placeCall()` remain the only blocking calls
(bounded, ~6 s and ~120 s worst case). They block the *control* loop only;
audio is unaffected. See the "known limitation" note on `placeCall()` in
`tincan_uac.hpp` for what that costs during dial-out.

## 3. Audio pipeline

```
Local Mic  ---> ES8311 Codec (I2S, full-duplex) ---> G.711 encode ---> RTP ---> drawbridge/peer
Local Spk  <--- ES8311 Codec (I2S, full-duplex) <--- G.711 decode <--- RTP <--- drawbridge/peer
```

8 kHz mono G.711 µ-law (PCMU, RTP payload type 0), 20 ms frames (160
samples) -- drawbridge does not transcode, so this must match what it
expects.

Full duplex is real and confirmed on hardware (2026-08-13), so this design
doesn't adopt tincan's half-duplex push-to-talk compromise.

Getting there cost most of the bring-up, and the cause is worth recording:
**the I2S data pins are wired the reverse of what LilyGO's header names
imply** (#34). `TDeckMaxBoard.h` calls GPIO40 `ASDOUT` and GPIO17 `DSDIN`,
which by ES8311 datasheet naming would make GPIO40 the ESP32's DIN. It is
the ESP32's DOUT. The symptom was a codec answering on I2C with every
register reading back correct, MCLK/BCLK/WS active on a scope, and total
silence both ways with a bit-exact-zero mic. Pins are now named
`BOARD_I2S_DOUT`/`BOARD_I2S_DIN` from the ESP32's point of view. #27 (mono
mode defaulting to the LEFT slot) was a plausible theory that turned out to
be wrong twice over: `esp_codec_dev` rebuilds the slot config inside
`esp_codec_dev_open()`, so the mask passed to `i2s_channel_init_std_mode()`
is discarded anyway.

**Echo, not duplex, is the real acoustic constraint.** Speaker and mic sit
centimetres apart on one PCB with no isolation and there is no AEC, so at
usable volume the far end hears itself. The mitigation is *ducking*: while
the far end is talking the mic is attenuated (`POC_DUCK_DB`, default −24 dB,
with a 200 ms hangover). That is a deliberate half-duplex compromise at the
*acoustic* layer — the user cannot interrupt the far end — chosen over real
AEC because `esp-sr`/`esp_afe` targets 16 kHz while this path is 8 kHz G.711
end to end and needs a time-aligned playback reference. Setting
`POC_DUCK_DB` to 0 restores true full duplex, with the echo.

Deliberately absent: no jitter buffer, no RTP sequence-number reordering,
and no packet-loss concealment. `recvAudioFrame()` takes one datagram and
returns one frame. Acceptable on a clean LAN, will audibly suffer on a
lossy or bursty link. `flushRtpBacklog()` is called when a call starts so a
pre-answer backlog isn't played out as latency that never recovers.

## 4. What's not implemented

- No cellular/4G PPP netif -- this PoC is Wi-Fi only (`main/src/net_wifi.c`, ported from tincan).
- No direct 3CX integration on this device (see above) -- `TDeckMaxAudioAnchor` is excluded from the build, kept for reference.
- No jitter buffer / RTP reordering / packet-loss concealment (see §3).
- No acoustic echo cancellation -- mic ducking only (see §3).
- Keypad digits 1-9 are unmapped, so `ENT` from idle fires the hardcoded `POC_TEST_DIAL` target rather than opening a dialler (#17). The map itself is now known -- `UI_DESIGN.md` §0.2 recovers it from LilyGO's own reference firmware -- but implementing it is blocked on the press/release polarity fix (#35), and the row/column decode remains unverified on hardware.
- Full alphabet font, real status icons, and e-paper partial refresh are deferred UI polish (#16). The phone also rings **silently**: no ringer or ringback tone is generated.
- No SIP authentication (#28). There is no `Authorization`/`WWW-Authenticate` handling anywhere in `main/`, and `registerExt()` treats any 4xx (including a `401 Unauthorized` challenge) as a flat rejection. This works only because drawbridge's registrar is **open by default**; pointing this firmware at drawbridge's secure/digest mode, or at any conventional PBX, will fail to register rather than retry with credentials.
- No DTMF (RFC 2833 / `telephone-event`) -- inbound SDP advertising payload 101 is parsed but not acted on, so in-call menu navigation on the far end won't work.
