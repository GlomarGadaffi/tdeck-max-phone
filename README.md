# tdeck-max-phone

[![Development Status](https://img.shields.io/badge/status-ACTIVE%20DEVELOPMENT-orange.svg)](#project-status--caveats)
[![Hardware Verification](https://img.shields.io/badge/hardware-UNTESTED%20%2F%20AWAITING%20VERIFICATION-yellow.svg)](#project-status--caveats)
[![Bench Test](https://img.shields.io/badge/bench--test-see%20docs-blue.svg)](docs/BENCH_TEST.md)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Target Board](https://img.shields.io/badge/board-LilyGO%20T--Deck%20MAX-purple.svg)](https://github.com/Xinyuan-LilyGO/T-Deck-MAX)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.0-red.svg)](https://docs.espressif.com/projects/esp-idf/)

A SIP phone proof-of-concept for the **LilyGO T-Deck MAX** (ESP32-S3 + E-Paper + TCA8418 keyboard + ES8311 audio codec). It registers as a plain SIP extension to a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) PBX instance on the LAN, which owns all 3CX Call Control API integration -- this device never talks to 3CX directly, has no OAuth/HTTPS client of its own, and needs no 3CX credentials.

Dialing `9<number>` from the keypad routes a call out through drawbridge's 3CX anchor; an incoming 3CX call rings this device (and any other registered extension) automatically.

---

> [!WARNING]
> ### Project Status & Caveats
>
> **UNTESTED ON PHYSICAL HARDWARE.** Everything that can be verified without a board has been -- native ESP-IDF v6.0.1 + QEMU-xtensa (no WSL, no CI minutes) confirms the firmware builds clean and boots correctly through hardware init, Wi-Fi driver bring-up, and PHY init before hitting the one thing QEMU genuinely can't emulate: real 802.11 association. Everything past that point (actual SIP registration, actual calls) needs real hardware or a reachable network. See [docs/BENCH_TEST.md](docs/BENCH_TEST.md).
>
> **Known limitations** (tracked as GitHub issues, not silently omitted):
> - The real T-Deck MAX keypad is a 4x10 QWERTY matrix, not a numeric keypad. Only `0`, DEL, and ENT are mapped today -- digits 1-9 sit behind an unconfirmed ALT/SYM shift layer, so dialing an arbitrary number from the keypad doesn't fully work yet. Answering/rejecting/hanging up needs no digit entry and works today.
> - `TincanUac::placeCall()` blocks while dialing out (bounded, ~120s worst case); a genuinely new inbound call arriving during that window gets no SIP response until it resolves.
> - E-paper UI renders digits and a simple active/idle pictogram only -- no full alphabet font, no partial refresh (every render is a full-screen flash, which is normal/expected for this panel type, just visible).
> - LoRa, GPS, 4G/cellular, touch, IMU, and battery-gauge hardware exist on the board and have pin definitions in `board_tdeck_max.h`, but none of it is wired into this firmware. This PoC is Wi-Fi only.

---

## Architecture

```
tdeck-max-phone                      drawbridge PBX                    3CX
+----------------+   SIP (LAN)   +----------------------+   OAuth2/WSS/REST   +--------+
| tincan_uac.cpp |<------------->| RequestsHandler +     |<------------------->| 3CX PBX|
| ES8311 codec   |   RTP (G.711) | TelephonyAnchorClient |   Call Control API  +--------+
| TCA8418 keypad |               +----------------------+
| GDEQ031T10 e-paper|
+----------------+
```

- **Outbound**: dial `9<number>` -> plain SIP INVITE to drawbridge -> drawbridge strips the `9` and calls out through its 3CX anchor.
- **Inbound**: drawbridge RING-ALLs every registered extension on an incoming 3CX call (first answer wins) -- no per-extension config needed on drawbridge's side.
- **Media**: full-duplex 8kHz G.711 µ-law (PCMU) over the ES8311 codec's I2S bus, confirmed to run mic+speaker simultaneously without a half-duplex compromise.

Full design, including what was corrected from an earlier (never-built) on-device-PBX/direct-3CX plan: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Hardware Pinmap (LilyGO T-Deck MAX)

| Component | Signal | GPIO / Expander Pin | Description |
| :--- | :--- | :--- | :--- |
| **System I2C Bus** | SDA / SCL | **IO13** / **IO14** | Shared bus for Touch, Audio, Haptics, Keyboard, Gyro, XL9555 |
| **System SPI Bus** | SCK / MOSI / MISO | **IO36** / **IO33** / **IO47** | Shared bus for E-Paper, LoRa, SD Card |
| **ES8311 Audio** | MCLK / SCLK / ASDOUT / LRCK / DSDIN | **IO38** / **IO39** / **IO40** / **IO18** / **IO17** | I2S audio bus for speaker & microphone |
| **3.1" E-Paper** | CS / DC / RST / BUSY / Backlight | **IO34** / **IO35** / **IO09** / **IO37** / **GPIO41** | Front-lit E-Paper display (GDEQ031T10) |
| **Touch** *(unused)* | INT / RST | **IO12** / **XL9555_0_7** | CST328 / CST3530 capacitive touch panel |
| **TCA8418 Keyboard** | INT / LED / RST | **IO15** / **IO42** / **XL9555_1_1** | QWERTY keyboard matrix & keypress interrupt |
| **DRV2605 Motor** *(unused)* | EN | **XL9555_0_5** | Precision haptic vibration motor |
| **A7682E (4G LTE)** *(unused)* | RXD / TXD / RI / ITR / PWR | **IO10** / **IO11** / **IO07** / **IO08** / **XL9555_1_0** | Cellular modem UART & power control |
| **Semtech SX1262** *(unused)* | CS / BUSY / RST / INT | **IO03** / **IO06** / **IO04** / **IO05** | LoRa RF transceiver |
| **SD Card** *(unused)* | CS | **IO48** | MicroSD card slot |
| **u-blox GPS** *(unused)* | RXD / TXD / PPS | **IO02** / **IO16** / **IO01** | GNSS positioning module |
| **BHI260AP Gyro** *(unused)* | INT | **IO21** | 6-axis IMU |

*(unused)* = present on the board, defined in `board_tdeck_max.h`, not wired into this firmware.

---

## XL9555 I/O Expander

The **XL9555 (I2C address `0x20`)** gates power/reset/routing for several peripherals -- see `main/src/xl9555.c` for the driver and `main/include/board_tdeck_max.h` for the bit assignments actually used by this firmware (speaker amp enable, touch reset, 4G power, keyboard reset, audio route select, antenna switch). The audio-route and 4G-power bits exist in hardware and the driver, but nothing in this firmware currently drives cellular audio through them -- Wi-Fi/drawbridge is the only signaling and media path today.

---

## Building & Testing

### Prerequisites
- ESP-IDF v6.0 (native install, no WSL required -- see below for the QEMU boot-testing setup)
- A real Wi-Fi network and a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) instance reachable on it, for anything past boot (see `main/include/poc_config.h`)

### Build & flash (real hardware)
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <COM_PORT> flash monitor
```

### Build & boot-test without hardware (QEMU)
No I2C/SPI peripheral models exist for this board's chips under QEMU, so a
sim-mode Kconfig flag skips those transactions (`main/Kconfig.projbuild`,
`CONFIG_TDECK_MAX_SIM_MODE`) -- everything else (Wi-Fi driver bring-up, the
SIP/RTP/G.711 stack, app orchestration) still runs for real:
```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.qemu" set-target esp32s3
idf.py build
idf.py qemu
```

Full bench-test procedure (what needs real hardware and why): [docs/BENCH_TEST.md](docs/BENCH_TEST.md).

---

## Docs

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) -- system design
- [docs/BENCH_TEST.md](docs/BENCH_TEST.md) -- SIP-phone PoC verification procedure
- [docs/HARDWARE_CAVEATS.md](docs/HARDWARE_CAVEATS.md) -- shared bus topology, pin states, pending bench items
- [docs/VALIDATION_PLAN.md](docs/VALIDATION_PLAN.md) -- broader hardware bring-up plan (partly aspirational, see its own note)

---

## License

MIT. See [LICENSE](LICENSE) -- this project combines original work with a vendored SIP parser (`components/sip_core`, ported via the sibling `tincan` project from `pocket-dial`, also MIT).
