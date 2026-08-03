# tdeck-max-phone

[![Development Status](https://img.shields.io/badge/status-ACTIVE%20DEVELOPMENT-orange.svg)](#-project-status--caveats)
[![Hardware Verification](https://img.shields.io/badge/hardware-UNTESTED%20%2F%20AWAITING%20VERIFICATION-yellow.svg)](#-project-status--caveats)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Target Board](https://img.shields.io/badge/board-LilyGO%20T--Deck%20MAX-purple.svg)](https://github.com/Xinyuan-LilyGO/T-Deck-MAX)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.1%2B%20%2F%20v6.0-red.svg)](https://docs.espressif.com/projects/esp-idf/)

A Media-Anchored SIP Handset and 3CX Cellular Gateway firmware tailored for the **LilyGO T-Deck MAX** (ESP32-S3 + E-Paper + TCA8418 Keyboard + ES8311 Audio Codec + A7682E 4G LTE + SX1262 LoRa).

Synthesizes the **drawbridge** 3CX Route Point API & Anchored Media engine, the **pocket-dial** self-contained SIP PBX, and the **tincan** full-duplex G.711 RTP audio pipeline into a standalone handheld device.

---

> [!WARNING]
> ### ⚠️ PROJECT STATUS & HARDWARE CAVEATS
> 
> **THIS FIRMWARE IS IN ACTIVE DEVELOPMENT AND IS CURRENTLY UNTESTED ON PHYSICAL HARDWARE.**
> 
> * **Software Scaffolding Complete**: The driver layer (XL9555, ES8311, TCA8418, E-Paper), 3CX Route Point API client (`TelephonyAnchorLogic`), and full-duplex RTP engine (`tincan_uac`) have been written according to official LilyGO schematics and proven `drawbridge`/`pocket-dial` architecture.
> * **Awaiting Hardware Verification**: Bench testing with physical LilyGO T-Deck MAX hardware, 4G LTE SIM cards, and live 3CX server instances is currently underway.
> * **Community Contributions Welcome**: Pull requests, issue reports, hardware trace verifications, and test logs are actively encouraged!

---

## 🏷️ Key Features & Keywords

* **Standalone 3CX SBC & Mobile Extension**: Registers to 3CX via the **Telephony Route Point API** (`/connect/token` OAuth2, `wss://<host>/callcontrol/ws` control plane, chunked `/stream` audio).
* **Full-Duplex VoIP Handset**: Continuous 8 kHz PCM16 G.711 $\mu$-law/A-law audio sampling over ES8311 codec I2S lines.
* **Cellular Data Gateway (4G LTE)**: Uses the onboard **A7682E modem** via PPP over UART for remote WAN connectivity to 3CX servers over cellular networks.
* **XL9555 Hardware Multiplexing**: Software-controlled audio output switching (`IO12` toggles ES8311 vs A7682E audio), speaker power amplifier enable (`IO06`), and LoRa antenna selection (`IO04`).
* **Front-Lit E-Paper UI**: Low-power 3.1" E-Paper display (GDEQ031T10) with controllable front-lighting (`GPIO41`) for night visibility.
* **Physical QWERTY Keypad**: Full DTMF dialing, star-code input (`*60` DND, `777` Echo Test), and text entry via TCA8418 I2C keyboard controller.
* **Off-Grid Radio Voice Bridging**: Built-in hooks for Semtech SX1262 LoRa mesh voice transport.

---

## 📌 Hardware Pinmap (LilyGO T-Deck MAX)

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

## 🎛️ XL9555 Hardware Multiplexing Rules

The **XL9555 (I2C address `0x20`)** manages resource switching on the T-Deck MAX:
- **Audio Output Select (`IO12`)**:
  - `IO12 = 0` (`LOW`): Routes speaker audio to **ES8311 Local Codec** (Handset mode).
  - `IO12 = 1` (`HIGH`): Routes speaker audio to **A7682E 4G Module** (Cellular pass-through).
- **Speaker Amplifier (`IO06`)**: Set `IO06 = 1` (`HIGH`) to enable the speaker power amplifier.
- **LoRa Antenna Switch (`IO04`)**: Set `IO04 = 1` (`HIGH`) for internal antenna, `0` (`LOW`) for external SMA.
- **4G Modem Power (`P1_0`)**: Set `P1_0 = 1` (`HIGH`) to power the A7682E modem.
- **Haptic Motor (`P0_5`)**: Set `P0_5 = 1` (`HIGH`) to enable DRV2605 haptics.

---

## 🏗️ System Architecture

```
                      +---------------------------------------+
                      |         LilyGO T-Deck MAX             |
                      |   ESP32-S3 (16MB Flash, 8MB PSRAM)   |
                      +-------------------+-------------------+
                                          |
                 +------------------------+------------------------+
                 |                                                 |
      +----------v----------+                           +----------v----------+
      |      Core 0         |                           |      Core 1         |
      |   pocket-dial       |                           |      tincan         |
      |  (SIP PBX & Engine) |                           | (Full-Duplex Handset|
      +----------+----------+                           +----------+----------+
                 |                                                 |
                 +-------------------+  +--------------------------+
                                     |  |
                           +---------v--v----------+
                           |  TDeckMaxAudioAnchor  |
                           |   (3CX Route Point)   |
                           +---------+-------------+
                                     |
                +--------------------+--------------------+
                |                                         |
     +----------v----------+                   +----------v----------+
     |   ES8311 Codec      |                   |    A7682E 4G LTE    |
     |  (XL9555 IO12=LOW)  |                   | (XL9555 IO12=HIGH)  |
     +---------------------+                   +---------------------+
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/HARDWARE_CAVEATS.md](docs/HARDWARE_CAVEATS.md) for technical deep-dives.

---

## ⚡ Quick Start: Building & Flashing

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

## 🤝 Contributing & Topic Tags

We welcome pull requests! Areas currently open for testing & refinement:
- Hardware validation of ES8311 I2S DMA buffers on physical T-Deck MAX boards.
- A7682E PPP netif dialing scripts.
- E-Paper partial refresh driver optimization.

`#esp32s3` `#3cx` `#voip` `#sip-phone` `#lilygo-tdeck-max` `#lora` `#cellular-gateway` `#embedded-cpp` `#esp-idf`

---

## 📄 License

Apache License 2.0. See [LICENSE](LICENSE) for details.
