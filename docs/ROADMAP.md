# Roadmap

Where `tdeck-max-phone` is, what comes next, and what is deliberately not being built.

Status as of **2026-08-13**, after the `hw-bringup-2026-08-12` bench sessions.

---

## Where we are

**The PoC's premise is proven.** A T-Deck MAX running this firmware registered to drawbridge
as extension `1002`, dialled a real mobile with the `9` trunk prefix, and carried two-way
audio confirmed by the operator on both ends — with no 3CX credential or API call on the
device. Evidence and serial captures are in [#3](../../issues/3).

The original question this project existed to answer ("can this board be a 3CX phone with all
the OAuth/WebSocket complexity living off-device?") is answered: **yes.**

What that leaves is a device that makes calls but is not yet a *phone*. It cannot dial a
number it wasn't compiled with, and it rings silently.

| Layer | State |
| :--- | :--- |
| Wi-Fi, SIP registration, re-REGISTER, OPTIONS keepalive | Working on hardware |
| Outbound call via drawbridge → 3CX, two-way G.711 audio | Working on hardware |
| Local hangup, second call without reboot | Working on hardware |
| ES8311 codec, gain staging, mic ducking | Working on hardware, tuned empirically |
| Power rails, XL9555 bring-up, SY6970 power-off | Working on hardware |
| E-paper render (full refresh, digits only) | Working on hardware |
| Keypad | Initialises; only `0` / DEL / ENT mapped; **cannot dial** |
| Inbound from a 3CX DN, far-end BYE | **Never exercised** |

---

## Near-term: make it a usable phone

This is the critical path, in dependency order. The design work is already done —
[UI_DESIGN.md](UI_DESIGN.md) specifies the key map, screen layouts and refresh strategy, and
§9 of that document is the source of the `P<n>` items below.

1. **Merge `hw-bringup-2026-08-12` into `master`.** Ten commits of bring-up fixes — the I2S
   pin correction, the XL9555 rail sequence, gain staging, the SIP retransmit fix and the
   power-off path — currently live only on the branch.

2. **P1 — Keypad press/release polarity ([#35](../../issues/35)). Blocking.**
   `tca8418_keypad.cpp` treats bit 7 as *press*; the Adafruit driver in the vendor tree and
   LilyGO's own factory firmware both document it as *release*. Simple taps hide this, which
   is why the phone appears to work today — but every hold and every modifier built on top
   would fail, in exactly the way the factory firmware's shift layer failed. Five minutes on
   the bench with `CONFIG_TDECK_MAX_KEYPAD_DEBUG=y` settles it. Nothing else in the UI plan
   is safe to build until it is.

3. **P2 — Event-based keypad API.** `tca8418_get_key()` returns a `char` and drops release
   events, so long-press and hold-to-clear are unrepresentable. Needs a scancode + edge API.

4. **[#17](../../issues/17) — Digit entry.** No longer a research problem: UI_DESIGN §0.2
   recovers a full telephone keypad from two LilyGO reference files that agree — `1`-`9` on
   `W E R / S D F / Z X C`, `0` on its own key, `*` and `#` on `A` and `Q`. Implement the
   per-state map and the dial buffer, then delete `POC_TEST_DIAL`.

5. **P3 / P6 — Fonts and a ringer ([#16](../../issues/16)).** An 8x16 ASCII font for status
   text, `*`/`#`/`+` added to the digit font, and ring/ringback tone generation. A phone that
   rings silently is the worst remaining defect after digit entry.

6. **P4 — Partial e-paper refresh ([#16](../../issues/16)).** Full-width Y-bands only, plus
   the ghost-debt counter. Every update is currently a full-screen flash.

7. **Finish the bench ledger.** Inbound from a 3CX DN, far-end BYE, busy handling, and the
   1-hour re-REGISTER have never been run. See [BENCH_TEST.md](BENCH_TEST.md).

---

## Also open, not on the critical path

| | |
| :--- | :--- |
| [#18](../../issues/18) | `placeCall()` blocks the control loop for up to ~120 s. Also the only thing preventing a **cancellable** outgoing call (UI_DESIGN §2.3), so the UI work may pull it forward. |
| [#28](../../issues/28) | No SIP digest auth — a `401` is treated as flat rejection. Works only against drawbridge's open registrar; any conventional PBX will refuse this firmware. |
| [#29](../../issues/29) | The A7682E 4G modem is powered at every boot and never used. Pure battery drain. |
| [#30](../../issues/30) | Legacy `driver/i2c.h` is deprecated in ESP-IDF v6 — migrate to `driver/i2c_master.h`. |
| [#6](../../issues/6) | 30-minute call endurance and thermal testing over Wi-Fi. |
| P5, P8, P9 | Runtime volume/mute API; render-generation acknowledgement; persist last dialled number. |
| U11 | Boot log prints `CST328 touch`; the part is actually a CST3530. Cosmetic while touch is unused. |

---

## Deliberately out of scope

Listed so nobody re-derives the reasoning:

- **Acoustic echo cancellation.** `esp-sr`/`esp_afe` targets 16 kHz while this path is 8 kHz
  G.711 end to end, needs a time-aligned playback reference through the DMA and RTP chain, and
  wants RAM and CPU alongside SIP, RTP and the e-paper task. Mic ducking (`POC_DUCK_DB`) is
  the twenty-line answer instead.
- **Jitter buffer, RTP reordering, packet-loss concealment.** One datagram in, one frame out.
  Acceptable on a clean LAN; would need building before this is usable over anything worse.
- **Touch.** Rejected on the merits in [UI_DESIGN.md](UI_DESIGN.md) §8 — a panel that needs
  ~2 s to acknowledge a tap cannot host a touch UI. Would only reopen if partial refresh
  measures comfortably under ~200 ms *and* a feature like call history actually wants it.
- **Alpha input.** Nothing downstream consumes letters: `placeCall()` builds
  `sip:<ext>@server` and drawbridge extensions are numeric.
- **On-device 3CX integration.** The whole point of the architecture is that drawbridge owns
  OAuth2/WSS/REST. See [ARCHITECTURE.md](ARCHITECTURE.md) §1.
- **Cellular / PPP.** Superseded by the drawbridge topology ([#5](../../issues/5), closed).
- **LoRa, GPS, IMU, haptics, battery gauge.** Present on the board, no drivers, not needed to
  be a phone.
- **DTMF (RFC 2833 / SIP INFO).** Not implemented, so far-end IVR menus cannot be navigated.
  Worth tracking before in-call digit keys are designed around (UI_DESIGN §11 Q3).

---

## Open questions for the operator

From [UI_DESIGN.md](UI_DESIGN.md) §11 — these need a human with the board in hand, not more code
reading:

1. Are the number/symbol legends physically printed on the keycaps? If not, the idle-screen
   cheat-sheet becomes load-bearing.
2. Is a mid-call full-screen flash acceptable? Decides whether a call-duration timer is worth
   building.
3. Is DTMF needed?
4. Hang up on a single `DEL` press, or a 400 ms hold?
