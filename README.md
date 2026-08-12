# tdeck-max-phone

[![Development Status](https://img.shields.io/badge/status-ACTIVE%20DEVELOPMENT-orange.svg)](#project-status--caveats)
[![Hardware Verification](https://img.shields.io/badge/hardware-UNTESTED%20%2F%20AWAITING%20VERIFICATION-yellow.svg)](#project-status--caveats)
[![Bench Test](https://img.shields.io/badge/bench--test-see%20docs-blue.svg)](docs/BENCH_TEST.md)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Target Board](https://img.shields.io/badge/board-LilyGO%20T--Deck%20MAX-purple.svg)](https://github.com/Xinyuan-LilyGO/T-Deck-MAX)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.0-red.svg)](https://docs.espressif.com/projects/esp-idf/)

A SIP phone proof-of-concept for the **LilyGO T-Deck MAX** (ESP32-S3 + E-Paper + TCA8418 keyboard + ES8311 audio codec). It registers as a plain SIP extension to a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) PBX instance on the LAN, which owns all 3CX Call Control API integration -- this device never talks to 3CX directly, has no OAuth/HTTPS client of its own, and needs no 3CX credentials.

Dialing `9<number>` routes a call out through drawbridge's 3CX anchor; an incoming 3CX call rings this device (and any other registered extension) automatically. Note that keypad digit entry is not fully mapped yet ([#17](../../issues/17)), so outbound dialing currently needs a hardcoded number -- inbound and answer/hangup do not.

---

> [!WARNING]
> ### Project Status & Caveats
>
> **UNTESTED ON PHYSICAL HARDWARE.** No call has ever been placed or received by this firmware on a real board.
>
> What *has* been verified without hardware, and exactly how far it goes:
> - **Host unit tests** (`ctest`, no board, no ESP-IDF) assert on generated SIP wire bytes -- this is what proves response formatting is correct, and it caught a real defect that compiled perfectly. See [Building & Testing](#building--testing).
> - **QEMU-xtensa** (native ESP-IDF v6.0.1, no WSL, no CI minutes) verifies the firmware builds clean and boots through peripheral init, the I2C scan, the e-paper task, and into Wi-Fi driver bring-up.
>
> **QEMU's ceiling is earlier than it looks.** Emulated time reaches ~754 ms at `phy_init`'s full-calibration fallback and then **stops advancing entirely** -- the emulator wedges there. It does not merely fail to associate. Measured by compiling the Wi-Fi timeout down to 2 s and waiting 200 s of wall clock: emulated time never moved and the timeout never fired. So **nothing downstream of Wi-Fi is reachable in QEMU at any timeout value** -- no registration, no SIP, no RTP, not even the Wi-Fi failure path. See [docs/BENCH_TEST.md](docs/BENCH_TEST.md).
>
> **Known limitations** (tracked as GitHub issues, not silently omitted):
> - Only `0`, DEL, and ENT are mapped on the 4x10 QWERTY matrix -- digits 1-9 sit behind an unconfirmed ALT/SYM shift layer, so dialing an arbitrary number doesn't work yet. The row/column decode itself is inherited from LilyGO's example and also unverified. Build with `CONFIG_TDECK_MAX_KEYPAD_DEBUG=y` to derive the real map on the bench ([#17](../../issues/17)). Answering/rejecting/hanging up needs no digit entry.
> - I2S mono mode defaults to the **left** slot; if the ES8311 mic lands on the right, the mic reads silence even with the codec correctly configured ([#27](../../issues/27)). Untested either way -- check it first if audio is one-directional.
> - `TincanUac::placeCall()` blocks while dialing out (bounded, ~120 s worst case); a genuinely new inbound call arriving in that window gets no SIP response until it resolves ([#18](../../issues/18)).
> - No jitter buffer, no RTP sequence/reordering handling, and no packet-loss concealment -- one datagram in, one frame out. Fine on a clean LAN, will audibly suffer on a lossy or bursty link.
> - E-paper renders digits and a simple active/idle pictogram only -- no alphabet font, no partial refresh, so every update is a full-screen flash ([#16](../../issues/16)). That flash is normal for this panel type, just visible.
> - LoRa, GPS, 4G/cellular, touch, IMU, and battery-gauge hardware exist on the board and have pin definitions in `board_tdeck_max.h`, but none of it is driven by this firmware (beyond parking the LoRa/SD chip-selects high so they can't corrupt the shared SPI bus). This PoC is Wi-Fi only.

---

## Architecture

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

- **Outbound**: dial `9<number>` -> plain SIP INVITE to drawbridge -> drawbridge strips the `9` and calls out through its 3CX anchor.
- **Inbound**: drawbridge RING-ALLs every registered extension on an incoming 3CX call (first answer wins) -- no per-extension config needed on drawbridge's side.
- **Media**: 8 kHz G.711 µ-law (PCMU, 20 ms frames) over the ES8311's I2S bus, pumped by a dedicated task paced solely by the blocking I2S read. The driver reports switching to full-duplex mode at init, so no half-duplex push-to-talk compromise is needed (unlike the sibling `tincan` project) -- though simultaneous mic+speaker has not been confirmed with actual audio on hardware, and [#27](../../issues/27) is an open risk to the mic path specifically.

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

*(unused)* = present on the board and defined in `board_tdeck_max.h`, but not driven by this firmware. Exception: the LoRa and SD chip-selects **are** driven HIGH at init, because they share SPI2 with the e-paper and an undriven CS can float low and corrupt its traffic.

---

## XL9555 I/O Expander

The **XL9555 (I2C address `0x20`)** gates power/reset/routing for several peripherals -- see `main/src/xl9555.c` for the driver and `main/include/board_tdeck_max.h` for the bit assignments actually used by this firmware (speaker amp enable, touch reset, 4G power, keyboard reset, audio route select, antenna switch). The audio-route and 4G-power bits exist in hardware and the driver, but nothing in this firmware currently drives cellular audio through them -- Wi-Fi/drawbridge is the only signaling and media path today.

---

## Building & Testing

### Prerequisites
- ESP-IDF v6.0 (native install, no WSL required) for firmware
- Any C++17 compiler + CMake for the host tests -- no ESP-IDF, no board
- A real Wi-Fi network and a [drawbridge](https://github.com/GlomarGadaffi/drawbridge) instance reachable on it, for anything past boot (see `main/include/poc_config.h`)

### Host unit tests (fastest loop -- no board, no ESP-IDF)
`components/sip_core` is portable by construction, so the SIP layer's
generated bytes can be asserted on directly. Run this before blaming the PBX
for anything:
```bash
cmake -B build-host -S test
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```
`test/sip_wire_test.cpp` parses a realistic inbound INVITE with the real
parser and checks the exact response bytes -- no doubled header field names,
exactly one of each required header, response re-parses cleanly, BYE Call-ID
matching works. This is what caught the malformed-response bug that compiled
perfectly and would only ever have surfaced as "drawbridge ignores us".

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

## Docs

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) -- system design
- [docs/BENCH_TEST.md](docs/BENCH_TEST.md) -- SIP-phone PoC verification procedure
- [docs/HARDWARE_CAVEATS.md](docs/HARDWARE_CAVEATS.md) -- shared bus topology, pin states, pending bench items
- [docs/VALIDATION_PLAN.md](docs/VALIDATION_PLAN.md) -- broader hardware bring-up plan (partly aspirational, see its own note)

---

## License

MIT. See [LICENSE](LICENSE) -- this project combines original work with a vendored SIP parser (`components/sip_core`, ported via the sibling `tincan` project from `pocket-dial`, also MIT).
