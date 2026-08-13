# Hardware & Software Validation Plan (Levels 0 – 5)

This document provides a systematic 6-stage validation protocol (Levels 0 through 5) for bringing up, bench-testing, and verifying `tdeck-max-phone` on physical **LilyGO T-Deck MAX** hardware.

> [!IMPORTANT]
> **Superseded (2026-08-13). Kept as a historical record — do not plan from this file.**
>
> This plan predates the implemented SIP-phone PoC and describes a broader,
> partly-aspirational scope (on-device `pocket-dial` PBX, direct 3CX
> integration, cellular/LoRa/battery field testing) that doesn't match what
> was actually built -- see `ARCHITECTURE.md`'s correction note.
>
> Read these instead:
> - **[ROADMAP.md](ROADMAP.md)** -- what is actually next, and what is deliberately out of scope.
> - **[BENCH_TEST.md](BENCH_TEST.md)** -- the real verification procedure, with a per-step status ledger.
>
> Where the levels below still map to reality: Levels 0-2 are largely satisfied
> (see issues #1, #2, #3), with the caveat that Level 0's `ctest` target was
> never the `TelephonyAnchorLogic` one described here -- the real host suite is
> `test/sip_wire_test.cpp`, and `TelephonyAnchorLogic` is excluded from the
> build. Level 2's "Audio Path Multiplexer" and partial-refresh items are out
> of scope for this firmware. Levels 3-5 were superseded outright: Level 3's
> on-device PBX never existed, and Levels 4-5 assume the cellular/LoRa
> architecture that #5 retired.
>
> Two items here also carry a trap that cost real bench time: the Level 2 pin
> list below names `Mic DIN IO40` / `Spk DOUT IO17`, which is **backwards**.
> See #34 and `HARDWARE_CAVEATS.md`.

---

## 🛠️ Validation Level Summary

```
Level 0: Firmware Build & Host Logic Unit Tests
  └── Level 1: Low-Level Bus & Power Management Bring-Up (I2C / SPI / XL9555)
        └── Level 2: Audio Codec & Peripheral Hardware Validation (ES8311 / Keypad / Screen)
              └── Level 3: Local PBX & Handset Loopback Calling (pocket-dial + tincan @ 127.0.0.1)
                    └── Level 4: A7682E Cellular PPP Data & 3CX Route Point API Integration
                          └── Level 5: LoRa Mesh Radio Bridging, Thermal & Battery Field Testing
```

---

## Level 0: Firmware Build & Host Logic Unit Tests

**Objective**: Verify toolchain stability, zero-warning compilation, static code hygiene, and unit test pass rates before touching physical hardware.

- [ ] **IDF Toolchain Verification**:
  ```bash
  idf.py set-target esp32s3
  idf.py build
  ```
  *Pass Criteria*: Clean compilation with zero build errors under ESP-IDF v5.1+, v5.2, and v6.0.

- [ ] **Pure Logic Unit Tests (`TelephonyAnchorLogic`)**:
  - Run host unit tests (`ctest --test-dir build/tests`) for JWT base64url decoding, expiration calculation, and 3CX URL builders (`tokenUrl`, `controlWsUrl`, `participantActionUrl`).
  *Pass Criteria*: 100% test pass rate on host platform.

---

## Level 1: Low-Level Bus & Power Management Bring-Up

**Objective**: Verify power rails, shared bus arbitration, and I2C/SPI slave detection on the T-Deck MAX.

- [ ] **I2C Bus Scan (`SDA IO13`, `SCL IO14`)**:
  - Flash `WireScan` utility and verify detection of all 6 onboard slave addresses:
    - `0x20` / `0x21`: XL9555 I/O Expander
    - `0x18`: ES8311 Audio Codec
    - `0x34`: TCA8418 Keypad Controller
    - `0x5A`: DRV2605 Haptic Motor Driver
    - `0x1A`: CST328 / CST3530 Touch Panel
    - `0x28`: Bosch BHI260AP Gyro / IMU
  *Pass Criteria*: All 6 addresses respond on I2C Bus 0 without bus lockups.

- [ ] **XL9555 Expander Pin Registers**:
  - Verify initialization sequence in `xl9555.c`:
    - `P1_0 = 1`: Powers ON the A7682E 4G modem.
    - `IO06 = 1`: Enables the speaker power amplifier.
    - `P1_1`: Pulses keyboard reset line.
    - `P0_7`: Pulses touch reset line.
  *Pass Criteria*: Measured GPIO voltage levels match expected register states.

- [ ] **Shared SPI Bus (`SCK IO36`, `MOSI IO33`, `MISO IO47`)**:
  - Verify E-Paper CS (`IO34`), LoRa CS (`IO03`), and SD Card CS (`IO48`) remain `HIGH` during boot to prevent bus contention.

---

## Level 2: Audio Codec & Peripheral Hardware Validation

**Objective**: Verify local audio playback/capture, hardware multiplexing, keypad input, and E-Paper display rendering.

- [ ] **ES8311 I2S Audio Loopback**:
  - Initialize I2S0 (`MCLK IO38`, `BCLK IO39`, `LRCK IO18`, `Mic DIN IO40`, `Spk DOUT IO17`) at 8 kHz 16-bit mono.
  - Capture microphone samples and pipe directly to speaker output.
  *Pass Criteria*: Clear audio playback through speaker with minimal delay and no DMA buffer underrun clicks.

- [ ] **Audio Path Multiplexer (`XL9555 IO12`)**:
  - Toggle `IO12`: Verify `IO12 = 0` routes speaker audio to **ES8311 Codec**, and `IO12 = 1` routes audio to **A7682E 4G Module**.
  *Pass Criteria*: Audio path toggles cleanly without hardware short-circuits.

- [ ] **TCA8418 Keypad & Haptics**:
  - Scan QWERTY keypresses; verify keycode interrupt on `IO15` and trigger DRV2605 haptic click on `P0_5`.
  *Pass Criteria*: Keypresses log accurately in serial console with immediate haptic response.

- [ ] **3.1" Front-Lit E-Paper Display**:
  - Verify partial refresh rendering of status text and backlight control (`GPIO41`).
  *Pass Criteria*: Sharp display redraws under ambient and dark conditions.

---

## Level 3: Local PBX & Handset Loopback Calling

**Objective**: Verify local `pocket-dial` PBX routing and `tincan` full-duplex handset audio over loopback network interface (`127.0.0.1`).

- [ ] **SIP Registrar & Auto-Registration**:
  - Start `pocket-dial` PBX on Core 0.
  - Auto-register `tincan` UAC on Core 1 as Extension `100` (`127.0.0.1:5060`).
  *Pass Criteria*: Extension `100` registers successfully within 1 second of boot.

- [ ] **Virtual Extension `777` Echo Test**:
  - Dial `777` from physical TCA8418 keypad.
  - Speak into local microphone; verify echo playback from PBX.
  *Pass Criteria*: Stable full-duplex audio stream with jitter buffer smoothing via `PlayoutBuffer`.

- [ ] **Star-Code Features**:
  - Test `*60` (DND On), `*80` (DND Off), and `999` (All-Page Broadcast).

---

## Level 4: Cellular Modem PPP Data & 3CX Route Point API

**Objective**: Establish cellular data connection and validate end-to-end 3CX call control over LTE.

- [ ] **A7682E LTE PPP Data Session**:
  - Power on A7682E via XL9555 `P1_0`.
  - Send AT commands over `UART1` (`IO10`/`IO11`) and initiate PPP data connection (`ATD*99#`).
  *Pass Criteria*: ESP32-S3 LWIP stack acquires WAN IP from cellular carrier.

- [ ] **3CX OAuth2 & WebSocket Connection**:
  - Send POST `/connect/token` to 3CX server; verify JWT access token acquisition.
  - Connect WebSocket `wss://<3cx-host>/callcontrol/ws` with Bearer token.
  *Pass Criteria*: WebSocket receives real-time JSON `Upset` and `Remove` call control events.

- [ ] **External 3CX Call Origination & Streaming**:
  - Place external call from T-Deck MAX keyboard to an outside number.
  - Verify chunked HTTP POST/GET media stream (`/callcontrol/{dn}/participants/{id}/stream`).
  *Pass Criteria*: Two-way voice call established through 3CX PBX over 4G LTE with warm TLS session resumption.

---

## Level 5: LoRa Mesh Radio, Thermal & Battery Field Testing

**Objective**: Validate off-grid radio voice bridging, long-term stability, thermal limits, and battery endurance.

- [ ] **Semtech SX1262 LoRa Initialization**:
  - Verify LoRa SPI communication (`CS IO03`, `BUSY IO06`, `RST IO04`, `INT IO05`).
  - Toggle LoRa antenna switch (`XL9555 IO04 = 1` internal, `0` external SMA).
  *Pass Criteria*: Clean LoRa packet TX/RX over sub-GHz radio frequencies.

- [ ] **30-Minute Call Endurance & Thermal Test**:
  - Run continuous full-duplex VoIP call over 4G LTE for 30 minutes.
  - Measure ESP32-S3 and A7682E module temperatures.
  *Pass Criteria*: Thermal stability maintained without CPU throttling or memory leaks.

- [ ] **Battery Fuel Gauge & Power Draw**:
  - Monitor battery discharge voltage via BQ27220 / SY6970 during standby and active calling.
  *Pass Criteria*: Accurate fuel gauge percentage on E-Paper status bar.
