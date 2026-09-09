# tdeck-max-phone

[![Development Status](https://img.shields.io/badge/status-ACTIVE%20DEVELOPMENT-orange.svg)](#project-status--caveats)
[![Hardware Verification](https://img.shields.io/badge/hardware-CALLS%20VERIFIED%20ON%20REAL%20BOARD-brightgreen.svg)](#project-status--caveats)
[![Bench Test](https://img.shields.io/badge/bench--test-register%20%2B%20outbound%203CX%20call%20PASS-blue.svg)](docs/BENCH_TEST.md)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Target Board](https://img.shields.io/badge/board-LilyGO%20T--Deck%20MAX-purple.svg)](https://github.com/Xinyuan-LilyGO/T-Deck-MAX)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.0-red.svg)](https://docs.espressif.com/projects/esp-idf/)

A SIP phone proof-of-concept for the **LilyGO T-Deck MAX** (ESP32-S3 + E-Paper + TCA8418 keyboard + ES8311 audio codec). It registers as a plain SIP extension to a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) PBX instance on the LAN, which owns all 3CX Call Control API integration -- this device never talks to 3CX directly, has no OAuth/HTTPS client of its own, and needs no 3CX credentials.

Dialing `9<number>` routes a call out through drawbridge's 3CX anchor; an incoming 3CX call rings this device (and any other registered extension) automatically. The full dialpad works (`0`-`9` `*` `#` `+`, ENT dials, ENT from idle redials the last number that connected — [#17](../../issues/17) landed 2026-08-13); during a call the dial keys send DTMF.

The SIP/RTP engine (`TincanUac`, jitter buffer, digest client, DTMF, G.711) lives in the sibling [tincan-core](https://github.com/GlomarGadaffi/tincan-core) repo, shared with the [tincan](https://github.com/GlomarGadaffi/tincan) intercom. Clone it alongside this one (or set `TINCAN_CORE_PATH`) before building.

---

> [!IMPORTANT]
> ### `feat/lvgl-shell` — the LVGL touch/keypad UI (2026-09-09, builds, **not yet flashed**)
>
> This branch replaces the digits-only e-paper pictogram with a real UI: an LVGL 8.3 home
> grid (Phone / Contacts / Settings), a dialer with an on-screen touch pad and recent calls,
> calling / in-call / incoming screens, an NVS phonebook with caller-ID name lookup, and the
> Settings app (Wi-Fi wizard, static IP, timezone, front-light, confirmed power off). All
> ESP-IDF, no Arduino. Wi-Fi credentials now come from the on-device wizard (NVS), not
> `poc_secrets.h`; SIP settings still do. The shell is a snapshot of the sibling
> `tdeck-glopanel` project's hardware-verified e-paper/keypad/touch layer — provenance,
> screen model and re-sync notes are in [docs/UI_SHELL.md](docs/UI_SHELL.md). Verified so
> far: clean build and a QEMU boot to the Wi-Fi wizard. Nothing below this box has been
> re-run on hardware with the new UI yet. Music and Messages are deliberately not on the
> home screen: the SD card is undriven and the SIP engine has no MESSAGE support.

> [!NOTE]
> ### Project Status & Caveats
>
> **The PoC's core claim is proven on real hardware (2026-08-13).** A T-Deck MAX running this
> firmware registered to drawbridge as extension `1002`, dialled a real mobile with the `9`
> trunk prefix, and carried **two-way audio confirmed by the operator on both ends** — with
> no 3CX credential or API call ever touching the device. Second call without a reboot works;
> `*777` echo returns voice. Session logs are in [#3](../../issues/3).
>
> The long silence during bring-up was [#34](../../issues/34): **the I2S data pins are wired
> the reverse of what LilyGO's header names imply.** Not the codec, not the rails, not the
> slot mask. If you are porting audio to this board, read that issue first — it will save you
> several bench sessions.
>
> **Still not exercised on hardware:** an inbound call from a 3CX DN (RING-ALL fork), and a
> hangup initiated by the *far* end (only local BYE has been driven). Busy handling, the
> dial-out/inbound race, and the 1-hour re-REGISTER are likewise unrun. See
> [docs/BENCH_TEST.md](docs/BENCH_TEST.md) for the per-step ledger.
>
> **Off-hardware checks** still run and still matter:
> - **Host unit tests** (`ctest`, no board, no ESP-IDF) assert on generated SIP wire bytes, the digest client (RFC 2617 vectors) and the jitter buffer -- this is what proves response formatting is correct, and it caught a real defect that compiled perfectly. They live with the engine in tincan-core; see [Building & Testing](#building--testing).
> - **QEMU-xtensa** (native ESP-IDF v6.0.1, no WSL, no CI minutes) verifies the firmware builds clean and boots through peripheral init, the I2C scan, the e-paper task, and into Wi-Fi driver bring-up.
>
> **QEMU's ceiling is earlier than it looks.** Emulated time reaches ~754 ms at `phy_init`'s full-calibration fallback and then **stops advancing entirely** -- the emulator wedges there. It does not merely fail to associate. Measured by compiling the Wi-Fi timeout down to 2 s and waiting 200 s of wall clock: emulated time never moved and the timeout never fired. So **nothing downstream of Wi-Fi is reachable in QEMU at any timeout value** -- no registration, no SIP, no RTP, not even the Wi-Fi failure path. See [docs/BENCH_TEST.md](docs/BENCH_TEST.md).
>
> **Known limitations** (tracked as GitHub issues, not silently omitted):
> - **The panel trails your fingers by 3.3 s.** A full refresh measures **3277 ms** and there is no partial refresh, so the dial buffer is always a beat or two behind what you typed. The depth-1 render queue discards superseded frames rather than replaying them, so it catches up rather than lagging cumulatively — but typing a long number is visibly laggy, and there is no input grace window yet on an incoming call ([#16](../../issues/16)).
> - **No acoustic echo cancellation.** Speaker and mic sit centimetres apart on one PCB, so at usable volume the far end hears itself. Mitigated by *ducking* the mic while the far end talks (`POC_DUCK_DB`, ~24 dB, 200 ms hangover) — a deliberate half-duplex compromise, not AEC. You cannot interrupt the far end while ducking is on; set `POC_DUCK_DB` to 0 for true full duplex plus the echo. The `*777` echo service will still howl by design.
> - Jitter buffer (60 ms target / 200 ms ceiling) with comfort-noise fill, but **no packet-loss concealment** proper and no reordering -- a late packet plays in arrival order. Watch `lost=`/`under=` in the in-call census line on a bad link.
> - Digest auth ([#28](../../issues/28)), non-blocking dial-out ([#18](../../issues/18)), DTMF (SIP INFO + in-band, or RFC 2833 via `POC_DTMF_RFC2833`) and the ringer are **implemented but not yet exercised on hardware** -- they landed with the tincan-core extraction and have host-test coverage only so far. See [docs/BENCH_TEST.md](docs/BENCH_TEST.md).
> - E-paper renders `0`-`9` `*` `#` `+` and a simple active/idle pictogram only -- no alphabet font, so status text and alphanumeric caller IDs don't render, and no partial refresh, so every update is a full-screen flash ([#16](../../issues/16)). That flash is normal for this panel type, just visible.
> - LoRa, GPS, 4G/cellular, touch, IMU, and battery-gauge hardware exist on the board and have pin definitions in `board_tdeck_max.h`, but none of it is driven by this firmware (beyond parking the LoRa/SD chip-selects high so they can't corrupt the shared SPI bus). This PoC is Wi-Fi only.

---

## Architecture

```
   tdeck-max-phone                    drawbridge PBX                      3CX
+---------------------+           +-----------------------+          +----------+
| TincanUac (core)    |  SIP/LAN  | RequestsHandler       |  OAuth2  |          |
| ES8311 codec        |<--------->|         +             |<-------->| 3CX PBX  |
| TCA8418 keypad      | RTP G.711 | TelephonyAnchorClient |  WSS/REST|          |
| GDEQ031T10 e-paper  |           +-----------------------+          +----------+
+---------------------+
   no 3CX credentials                owns all 3CX integration
```

- **Outbound**: dial `9<number>` -> plain SIP INVITE to drawbridge -> drawbridge strips the `9` and calls out through its 3CX anchor.
- **Inbound**: drawbridge RING-ALLs every registered extension on an incoming 3CX call (first answer wins) -- no per-extension config needed on drawbridge's side.
- **Media**: 8 kHz G.711 µ-law (PCMU, 20 ms frames) over the ES8311's I2S bus, pumped by a dedicated task paced solely by the blocking I2S read. The codec runs genuinely full-duplex -- confirmed with real audio on hardware, so none of the sibling `tincan` project's push-to-talk compromise is inherited. What *is* compromised is echo: with speaker and mic centimetres apart and no AEC, the mic is ducked while the far end talks (`POC_DUCK_DB`). See [Gain staging](#gain-staging--echo-control).

Full design, including what was corrected from an earlier (never-built) on-device-PBX/direct-3CX plan: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Hardware Pinmap (LilyGO T-Deck MAX)

| Component | Signal | GPIO / Expander Pin | Description |
| :--- | :--- | :--- | :--- |
| **System I2C Bus** | SDA / SCL | **IO13** / **IO14** | Shared bus for Touch, Audio, Haptics, Keyboard, Gyro, XL9555 |
| **System SPI Bus** | SCK / MOSI / MISO | **IO36** / **IO33** / **IO47** | Shared bus for E-Paper, LoRa, SD Card |
| **ES8311 Audio** | MCLK / SCLK / LRCK / **DOUT** / **DIN** | **IO38** / **IO39** / **IO18** / **IO40** / **IO17** | I2S audio bus for speaker & microphone. `DOUT`/`DIN` are named **from the ESP32's point of view** — see the warning below. |
| **3.1" E-Paper** | CS / DC / RST / BUSY / Backlight | **IO34** / **IO35** / **IO09** / **IO37** / **GPIO41** | Front-lit E-Paper display (GDEQ031T10) |
| **Touch** *(unused)* | INT / RST | **IO12** / **XL9555_0_7** | CST328 / CST3530 capacitive touch panel |
| **TCA8418 Keyboard** | INT / LED / RST | **IO15** / **IO42** / **XL9555_1_1** | QWERTY keyboard matrix & keypress interrupt |
| **DRV2605 Motor** *(unused)* | EN | **XL9555_0_5** | Precision haptic vibration motor |
| **A7682E (4G LTE)** *(unused)* | RXD / TXD / RI / ITR / PWR | **IO10** / **IO11** / **IO07** / **IO08** / **XL9555_1_0** | Cellular modem UART & power control |
| **Semtech SX1262** *(unused)* | CS / BUSY / RST / INT | **IO03** / **IO06** / **IO04** / **IO05** | LoRa RF transceiver |
| **SD Card** *(unused)* | CS | **IO48** | MicroSD card slot |
| **u-blox GPS** *(unused)* | RXD / TXD / PPS | **IO02** / **IO16** / **IO01** | GNSS positioning module |
| **BHI260AP Gyro** *(unused)* | INT | **IO21** | 6-axis IMU |

> [!CAUTION]
> **The I2S data pins are wired the reverse of what the vendor header names imply.** LilyGO's
> `TDeckMaxBoard.h` calls GPIO40 `ASDOUT` and GPIO17 `DSDIN`; by ES8311 datasheet naming that
> would make GPIO40 the ESP32's **DIN** and GPIO17 its **DOUT**. On real hardware it is the
> other way round, measured two independent ways ([#34](../../issues/34)). Getting this wrong
> gives you a codec that answers on I2C with every register reading back correct, MCLK/BCLK/WS
> all active on a scope, and **total silence in both directions with a bit-exact-zero mic** —
> a symptom that looks like dead hardware. This firmware therefore names them
> `BOARD_I2S_DOUT` (40, ESP32→codec) and `BOARD_I2S_DIN` (17, codec→ESP32).

*(unused)* = present on the board and defined in `board_tdeck_max.h`, but not driven by this firmware. Exception: the LoRa and SD chip-selects **are** driven HIGH at init, because they share SPI2 with the e-paper and an undriven CS can float low and corrupt its traffic.

---

## XL9555 I/O Expander

The **XL9555 (I2C address `0x20`)** gates power/reset/routing for several peripherals -- see `main/src/xl9555.c` for the driver and `main/include/board_tdeck_max.h` for the bit assignments actually used by this firmware (speaker amp enable, touch reset, 4G power, keyboard reset, audio route select, antenna switch). The audio-route and 4G-power bits exist in hardware and the driver, but nothing in this firmware currently drives cellular audio through them -- Wi-Fi/drawbridge is the only signaling and media path today.

---

## Gain staging & echo control

Every audio level lives in `main/include/poc_config.h`, with the reasoning for each value at
its definition. Read there rather than trusting numbers quoted elsewhere -- these were settled
empirically on the bench and have moved more than once.

| Knob | What it does | Why it needs care |
| :--- | :--- | :--- |
| `POC_MIC_GAIN_DB` | ES8311 mic PGA. **Quantised to 6 dB steps, 0-42** -- only multiples of 6 are meaningful. | The far end reported a quiet mic below 30 dB. This is also the level that makes `*777` howl. |
| `POC_SPK_VOLUME` / `POC_SPK_MAX_DB` | 0-100 user knob, and the dB the curve's top maps to. | `esp_codec_dev`'s stock curve tops out at 0 dB and leaves most of the ES8311's +32 dB range unused. Raising the ceiling is digital gain *ahead* of the DAC, so it clips. If calls sound distorted rather than loud, lower `POC_SPK_MAX_DB` first. |
| `POC_RX_GAIN_DB` | Software gain on decoded RTP only. | The boot beep and PSTN voice arrive ~18 dB apart. Codec volume lifts both; this lifts only the quiet one. |
| `POC_DUCK_DB` / `POC_DUCK_THRESHOLD` / `POC_DUCK_HANGOVER_MS` | Attenuate the mic while the far end is talking. | **Not AEC.** The tradeoff is that you cannot interrupt the far end. Set `POC_DUCK_DB` to 0 for true full duplex plus the echo. |

Real AEC (`esp-sr` / `esp_afe`) is the next audio workstream: it targets 16 kHz, so the capture
path moves to 16 kHz with a 2:1 decimation to the 8 kHz G.711 wire, and it needs a time-aligned
playback reference -- a ring buffer of played samples, since the codec driver exposes no DMA
position. Ducking stays as the fallback.

**`*777` howling is expected.** An echo service deliberately closes
mic → RTP → PBX → RTP → speaker → mic, and on an open speakerphone that loop has gain above
unity. A call to a person does not do this. If it makes bench testing hard to judge, drop
`POC_MIC_GAIN_DB` to 18 or use headphones -- don't leave real calls quiet for it.

---

## Building & Testing

### Prerequisites
- ESP-IDF v6.0 (native install, no WSL required) for firmware
- [tincan-core](https://github.com/GlomarGadaffi/tincan-core) checked out as a sibling directory (`../tincan-core`), or `TINCAN_CORE_PATH` pointing at it -- the build stops with a clear error otherwise
- Any C++17 compiler + CMake for the host tests -- no ESP-IDF, no board
- A real Wi-Fi network and a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) instance reachable on it, for anything past boot (see `main/include/poc_config.h`)

### Host unit tests (fastest loop -- no board, no ESP-IDF)
The SIP engine's tests moved with it to tincan-core. Run them there before
blaming the PBX for anything:
```bash
cd ../tincan-core
cmake -B build-host -S test
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```
`sip_wire_test` parses a realistic inbound INVITE with the real parser and
checks the exact response bytes (this is what caught the malformed-response
bug that compiled perfectly and would only ever have surfaced as "drawbridge
ignores us"); `sip_digest_test` checks the digest client against the RFC 2617
worked example; `playout_buffer_test` covers the jitter buffer.

### Build & flash (real hardware)
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <COM_PORT> flash monitor
```
Boot prints an `init OK :` / `init FAIL :` line per peripheral followed by an
I2C bus scan naming each expected device PRESENT/absent. A failing peripheral
logs and continues rather than panic-rebooting, so the log reports every
failure at once -- read this before anything else. (`CONFIG_TDECK_MAX_HALT_ON_INIT_FAIL=y`
restores fail-fast.)

### Build & boot-test without hardware (QEMU)
QEMU has no peripheral models for this board's chips, so a sim-mode Kconfig
flag (`main/Kconfig.projbuild`, `CONFIG_TDECK_MAX_SIM_MODE`) skips the I2C/SPI
transactions that would otherwise time out:
```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.qemu" set-target esp32s3
idf.py build
idf.py qemu
```
This exercises build correctness, boot, peripheral init ordering, and the
bring-up logging path. It does **not** reach Wi-Fi, SIP, or RTP -- see the
QEMU ceiling note in Project Status above.

Full bench-test procedure (what needs real hardware and why): [docs/BENCH_TEST.md](docs/BENCH_TEST.md).

---

## Roadmap

The telephony path is proven and the phone now dials, rings, sends DTMF and can register to a
locked-down PBX. Next, in dependency order: bench the new paths (digest against drawbridge in
Secure mode, the ringer, INFO-DTMF star codes, jitter buffer on a lossy link), then AEC via
`esp-sr` at 16 kHz, then codec negotiation + G.722 once drawbridge negotiates SDP instead of
forcing G.711. The e-paper fonts and partial refresh ([#16](../../issues/16)) remain the UI gap.

Full plan, including what is deliberately out of scope: **[docs/ROADMAP.md](docs/ROADMAP.md)**.

---

## Docs

- **[docs/ROADMAP.md](docs/ROADMAP.md)** -- where this is going, and what is deliberately not being built
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) -- system design
- [docs/UI_DESIGN.md](docs/UI_DESIGN.md) -- keypad map, e-paper screen layouts, refresh strategy, touch verdict (design, not yet implemented)
- [docs/BENCH_TEST.md](docs/BENCH_TEST.md) -- SIP-phone PoC verification procedure and per-step status
- [docs/HARDWARE_CAVEATS.md](docs/HARDWARE_CAVEATS.md) -- shared bus topology, pin states, pending bench items
- [docs/VALIDATION_PLAN.md](docs/VALIDATION_PLAN.md) -- superseded historical bring-up plan (see its own note)

---

## License

MIT. See [LICENSE](LICENSE). The SIP engine and its vendored parser (`sip_core`, from `pocket-dial`, MIT) live in [tincan-core](https://github.com/GlomarGadaffi/tincan-core), also MIT.
