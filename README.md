# tdeck-max-phone

A Media-Anchored SIP Phone and 3CX Cellular Gateway firmware tailored for the **LilyGO T-Deck MAX** (ESP32-S3 + E-Paper + TCA8418 Keyboard + ES8311 Audio + A7682E 4G LTE + SX1262 LoRa).

Built on the architecture of **drawbridge** (commercial 3CX Route Point & SSH TUI engine) and **pocket-dial** (SIP PBX engine), combined with **tincan** full-duplex handset audio.

---

## Architecture Lineage & Heritage (drawbridge + pocket-dial + tincan)

This project synthesizes three core open-source / embedded voice projects:

1. **drawbridge (`src/SIP/TelephonyAnchorClient.cpp` & `TelephonyAnchorLogic.hpp`)**:
   - Production 3CX / Telephony Route Point API client (`/connect/token` OAuth, `wss://<host>/callcontrol/ws` control plane, `/participants/{id}/stream` HTTP audio).
   - Multi-call concurrent `CallSlot` pool with warm TLS session handles for fast ECDHE resumption.
   - Non-blocking FreeRTOS WebSocket worker queue to prevent WS task stalls.
   - `littlessh` ANSI Sysop Terminal TUI (`Tui.cpp`) configuration surface.

2. **pocket-dial (`src/SIP/RequestsHandler.cpp` & `MediaBridge.cpp`)**:
   - Self-contained SIP PBX registrar, call state machines, and `MediaBridge` RTP shuttling.
   - Call Park orbits (`700`-`709`), paging zones (`980`-`989`), BLF/presence, ring groups, call forwarding, and star-codes.
   - Zero-heap allocation in packet hot path.

3. **tincan (`main/audio_io.c` & `g711.c`)**:
   - Embedded G.711 $\mu$-law/A-law audio encoding/decoding and RTP packetization for ESP32-S3.

---

## Hardware Pinmap (LilyGO T-Deck MAX)

| Component | Signal | GPIO / Expander Pin | Description |
| :--- | :--- | :--- | :--- |
| **System I2C Bus** | SDA / SCL | **IO13** / **IO14** | Shared bus for Touch, Audio, Haptics, Keyboard, Gyro, XL9555 |
| **System SPI Bus** | SCK / MOSI / MISO | **IO36** / **IO33** / **IO47** | Shared bus for E-Paper, LoRa, SD Card |
| **ES8311 Audio** | MCLK / SCLK / ASDOUT / LRCK / DSDIN | **IO38** / **IO39** / **IO40** / **IO18** / **IO17** | I2S audio bus for speaker & microphone |
| **3.1" E-Paper** | CS / DC / RST / BUSY / Backlight | **IO34** / **IO35** / **IO09** / **IO37** / **GPIO41** | Front-lit E-Paper display (GDEQ031T10) |
| **Touch** | INT / RST | **IO12** / **XL9555_0_7** | CST328 / CST3530 capacitive touch panel |
| **TCA8418 Keyboard** | INT / LED / RST | **IO15** / **IO42** / **XL9555_1_1** | QWERTY keyboard matrix & keypress interrupt |
| **DRV2605 Motor** | EN | **XL9555_0_5** | Precision haptic vibration motor |
| **A7682E (4G LTE)** | RXD / TXD / RI / ITR / PWR | **IO10** / **IO11** / **IO07** / **IO08** / **XL9555_1_0** | Cellular modem UART & power control |
| **Semtech SX1262** | CS / BUSY / RST / INT | **IO03** / **IO06** / **IO04** / **IO05** | LoRa RF transceiver |
| **SD Card** | CS | **IO48** | MicroSD card slot |
| **u-blox GPS** | RXD / TXD / PPS | **IO02** / **IO16** / **IO01** | GNSS positioning module |
| **BHI260AP Gyro** | INT | **IO21** | 6-axis IMU |

---

## XL9555 Hardware Multiplexing Rules

The **XL9555 (I2C `0x20`)** controls resource switching on the T-Deck MAX:
- **Audio Output Selection (`IO12`)**:
  - Set **`IO12` to LOW (`0`)**: Routes audio to **ES8311 Codec** (Local mic/speaker).
  - Set **`IO12` to HIGH (`1`)**: Routes audio to **A7682E 4G Cellular Module**.
- **Speaker Amplifier (`IO06`)**: Set **`IO06` to HIGH (`1`)** to enable speaker power amp.
- **LoRa Antenna Switch (`IO04`)**: Set **`IO04` to HIGH (`1`)** for internal antenna, **LOW (`0`)** for external.
- **DRV2605 Haptic Motor (`P0_5`)**: Set **`P0_5` to HIGH (`1`)** to enable haptic feedback.
- **Modem Power (`P1_0`)**: Set **`P1_0` to HIGH (`1`)** to power the A7682E LTE modem.

---

## 3CX Integration Protocol (from `drawbridge`)

```
T-Deck MAX (ESP32-S3)                          3CX Server / Softswitch
       |                                                 |
       |----- POST /connect/token (OAuth Client ID/Secret) ->|
       |<---- 200 OK (JWT Access Token & Expiration) ------|
       |                                                 |
       |----- WSS /callcontrol/ws (Bearer Token Header) --->|
       |<---- 101 Switching Protocols -------------------|
       |<---- JSON Event: Upset (Dialing/Connected) ------|
       |                                                 |
       |----- POST /callcontrol/{dn}/participants/stream ->|
       |<---- 200 OK Chunked Audio Stream (PCM16 8kHz) ---|
```

---

## Building and Flashing

### Prerequisites
- ESP-IDF v5.1 or later (up to v6.0)

### Setup & Build
```bash
# Set ESP-IDF target to ESP32-S3
idf.py set-target esp32s3

# Build firmware
idf.py build

# Flash & open serial monitor
idf.py -p <COM_PORT> flash monitor
```

---

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.
