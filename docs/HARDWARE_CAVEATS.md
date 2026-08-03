# Hardware Caveats & Verification Roadmap (LilyGO T-Deck MAX)

> [!WARNING]
> **Status: UNTESTED / AWAITING HARDWARE VERIFICATION**
> This document outlines the physical hardware design choices, shared bus constraints, and pending bench-test verification items for `tdeck-max-phone`.

---

## 1. Shared Bus Topology & Constraints

The LilyGO T-Deck MAX features high peripheral density on the ESP32-S3. Several peripherals share common I2C and SPI buses:

### Shared I2C Bus (`SDA IO13`, `SCL IO14`)
Shared by 6 distinct slave devices:
1. **XL9555 I/O Expander** (`0x20` / `0x21`)
2. **ES8311 Audio Codec** (`0x18`)
3. **TCA8418 Keyboard Controller** (`0x34`)
4. **DRV2605 Haptic Motor Driver** (`0x5A`)
5. **CST328 / CST3530 Touch Controller** (`0x1A`)
6. **Bosch BHI260AP Gyro / IMU** (`0x28`)

* **Verification Item**: I2C bus arbitration during simultaneous keypad interrupts (`IO15`) and ES8311 register writes. Ensure I2C clock speed is set to $400\text{ kHz}$ with proper pull-up resistors.

### Shared SPI Bus (`SCK IO36`, `MOSI IO33`, `MISO IO47`)
Shared by 3 SPI slave devices:
1. **3.1" E-Paper Display** (`CS IO34`)
2. **Semtech SX1262 LoRa** (`CS IO03`)
3. **MicroSD Card** (`CS IO48`)

* **Verification Item**: Ensure SPI CS pins (`IO34`, `IO03`, `IO48`) are kept `HIGH` during boot to prevent bus contention during SD card or E-Paper initialization.

---

## 2. XL9555 Expander Pin States & Power-On Sequence

The **XL9555 (I2C `0x20`)** must be initialized early in `app_main.cpp` before audio or cellular peripherals can be accessed:

```c
// Recommended Boot Sequence
1. Bring up System I2C Bus (IO13 SDA, IO14 SCL)
2. Initialize XL9555 Expander:
   - Set Port 0 & Port 1 pin directions to OUTPUT (0x00)
   - Set P1_0 HIGH (Power ON A7682E 4G Modem)
   - Set P0_6 HIGH (Enable Speaker Power Amp)
   - Set P1_2 LOW  (Route Audio to ES8311 Codec)
   - Pulse P1_1 LOW -> HIGH (Reset TCA8418 Keypad)
   - Pulse P0_7 LOW -> HIGH (Reset Touch Panel)
```

---

## 3. Pending Hardware Bench Tests

- [ ] **I2S Audio Quality**: Verify ES8311 codec 8 kHz mono sampling over `IO38` (MCLK), `IO39` (BCLK), `IO18` (LRCK), `IO40` (Mic DIN), `IO17` (Spk DOUT).
- [ ] **Acoustic Echo & Feedback**: Measure speakerphone feedback in full-duplex mode on physical hardware; tune software AEC / noise suppression.
- [ ] **A7682E 4G PPP Connection**: Test `esp_modem` PPP dialing scripts (`ATD*99#`) to verify LTE data throughput for 3CX WebSocket and RTP media streams.
- [ ] **Battery & Thermal Management**: Monitor battery drain on SY6970 / BQ27220 during continuous 30-minute 3CX VoIP calls.
