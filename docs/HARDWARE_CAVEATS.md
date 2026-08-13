# Hardware Caveats & Verification Roadmap (LilyGO T-Deck MAX)

> [!WARNING]
> **Status: UNTESTED / AWAITING HARDWARE VERIFICATION**
> This document outlines the physical hardware design choices, shared bus constraints, and pending bench-test verification items for `tdeck-max-phone`.

> **Note (2026-08):** several items below are now handled in firmware rather
> than being purely manual bench checks, and two assume the cellular/3CX-direct
> architecture that was superseded (see `ARCHITECTURE.md`'s correction note and
> closed issues #4/#5). Per-item status is annotated inline.
>
> **PSRAM:** `sdkconfig.defaults` previously set octal mode, which on the
> ESP32-S3 consumes GPIO 33-38 -- every one of which this board uses for
> MOSI/EPD_CS/EPD_DC/SCK/EPD_BUSY. Corrected to quad before first flash
> (#21), matching LilyGO's own board definition (`"memory_type": "qio_qspi"`).
> If anything on the SPI bus misbehaves, verify `CONFIG_SPIRAM_MODE_QUAD=y`
> before debugging further.

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

**How `xl9555_init()` actually differs from the above, verified against the code:**

- **Touch reset is not pulsed.** P0_7 is simply set HIGH (reset released). A real LOW→HIGH pulse only happens in `xl9555_reset_touch()`, which nothing calls -- the touch panel is unused by this firmware.
- **Keypad reset is pulsed**, but from `tca8418_init()` via `xl9555_reset_keyboard()`, not from `xl9555_init()` itself.
- **P1_0 is asserted, powering the A7682E 4G modem on every boot** -- even though this firmware is Wi-Fi only and never talks to the modem. On a battery-powered device that is continuous drain for no benefit. Deliberately left as-is rather than changed blind, because the power-sequencing consequences on real hardware are unverified; worth measuring during bring-up and probably worth turning off once confirmed safe (#29).
- **The speaker amp (P0_6) comes up enabled.** `app_main()` now explicitly disables it after audio init, since nothing should be playing at boot and an idle-but-powered amp both draws current and can hiss/click on DMA underrun. It is re-enabled per call.

---

## 3. Pending Hardware Bench Tests

- [x] **I2S Audio Quality**: ES8311 at 8 kHz over `IO38` (MCLK), `IO39` (BCLK),
      `IO18` (LRCK), **`IO40` (DOUT, ESP32 → codec)**, **`IO17` (DIN, codec → ESP32)**.
      Verified 2026-08-13: 0 register-check failures, mic peak 1321 / rms 335 with
      0/100 silent frames, `*777` echo returns audio.
      > ⚠ **The data pins are the reverse of what the vendor header names imply.**
      > LilyGO's `TDeckMaxBoard.h` calls GPIO40 `ASDOUT` and GPIO17 `DSDIN`, which by
      > ES8311 datasheet naming would make 40 the ESP32's DIN and 17 its DOUT. On real
      > hardware it is the other way round ([#34](../../issues/34)). Getting this wrong
      > gives a codec that answers on I2C with every register correct, MCLK/BCLK/WS all
      > active on a scope, and total silence in **both** directions with a bit-exact-zero
      > mic. This cost several bench sessions. Pins are now named `BOARD_I2S_DOUT` /
      > `BOARD_I2S_DIN` from the ESP32's point of view so it cannot be misread again.
- [ ] **Acoustic Echo & Feedback**: Measure speakerphone feedback in full-duplex mode on physical hardware; tune software AEC / noise suppression.
      *No AEC or noise suppression exists in this firmware -- this is a measurement task, not a tuning task, until something is written.*
      *Observed 2026-08-13: the `*777` echo service closes an acoustic loop with gain
      above unity and howls progressively. Speaker and mic are centimetres apart on the
      same PCB with no isolation. Expected for an echo service on an open speakerphone,
      not a fault -- a normal call does not do this. Backed off to `POC_MIC_GAIN_DB` 18 dB
      / `POC_SPK_VOLUME` 70; headphones remove it entirely.*
- [ ] ~~**A7682E 4G PPP Connection**~~ -- **superseded** (#5). No cellular/PPP code exists; 3CX integration lives in drawbridge and this device is Wi-Fi only.
- [ ] ~~**Battery & Thermal during 30-minute 3CX VoIP calls**~~ -- the *cellular* framing is superseded (#5); a 30-minute call endurance/thermal test over Wi-Fi remains valid and is tracked in #6.

## 4. Now handled in firmware (previously manual bench items)

- **I2C device presence** -- boot runs a scan of 0x08-0x77 naming each expected device PRESENT/absent. No separate `WireScan` sketch needed (#26, #2).
- **Peripheral init failures** -- log-and-continue rather than `ESP_ERROR_CHECK`, so one NAK no longer panic-reboots without saying which device failed. `CONFIG_TDECK_MAX_HALT_ON_INIT_FAIL=y` restores fail-fast.
- **Shared-SPI CS parking** -- LoRa (`IO3`) and SD (`IO48`) chip-selects are now driven HIGH at init alongside EPD CS, so an undriven CS can't float low and corrupt e-paper traffic. Still worth confirming with a meter, but no longer purely dependent on it.
- **SPI MISO** -- the bus is initialized with `BOARD_SPI_MISO` wired even though the e-paper never drives it, because whoever initializes SPI2 first fixes its pin set for the SX1262 and SD card too.
