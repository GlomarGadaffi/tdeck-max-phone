# Bench Test: SIP Phone PoC (register → call through 3CX via drawbridge)

Covers what's actually implemented today: Wi-Fi bring-up, a real TCA8418
keypad driver, a real GDEQ031T10 e-paper driver, and a SIP UAC/UAS
(`tincan_uac.cpp`) that registers this device as a plain LAN extension to a
[drawbridge](https://github.com/GlomarGadaffi/drawbridge) PBX instance,
which owns all 3CX Call Control API integration. See `ARCHITECTURE.md` for
the full design and `poc_config.h` for the constants referenced below.

## What's verifiable without hardware (already done, see repo history)

No hardware-in-the-loop CI exists in this repo. Everything that doesn't
need real I2C/SPI peripherals or real 802.11 RF was verified locally via a
native ESP-IDF v6.0.1 + QEMU-xtensa setup (no WSL needed on Windows):

```
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.qemu" set-target esp32s3
idf.py build
idf.py qemu
```

This confirmed: the firmware builds clean (zero warnings) in both the real-
hardware config and the QEMU sim-mode config (`CONFIG_TDECK_MAX_SIM_MODE`,
see `main/Kconfig.projbuild`), and boots cleanly through XL9555/ES8311/
TCA8418/e-paper init and into Wi-Fi driver + PHY init before blocking on
802.11 association -- which QEMU cannot emulate at all, and is the honest
ceiling of what's verifiable without a board or a from-scratch RF simulator.
Everything below this line needs real hardware.

## Prerequisites

- A LilyGO T-Deck MAX flashed with this firmware (`sdkconfig.defaults`, no QEMU fragment).
- `main/include/poc_config.h` edited with real Wi-Fi credentials and a real drawbridge server IP (currently `CHANGE_ME` placeholders).
- A drawbridge instance reachable on the same LAN (host build is fastest for iteration: `./build/SipServer --ip <LAN-IP> --port 5060 --web 8080`), with a real 3CX tenant configured via its SSH sysop TUI ("PBX Config" tab) -- see drawbridge's `docs/EXTENSION_SETUP.md`.
- A desktop softphone on the same LAN, registered as a second extension, for steps 2 and 4.

## Procedure

1. **Flash and register.** Flash the device, watch the serial log for `registered as <ext>`, and confirm the extension appears in drawbridge's Extensions list (open registrar -- no pre-created extension needed).
2. **Inbound call.** Call this device's extension from the desktop softphone. Expect: e-paper shows the caller's extension and "Incoming Call". Press ENT on the keypad to answer -- expect two-way audio and the screen to show "In Call". Press ENT or DEL to hang up -- expect BYE round-trip and the screen to return to "Idle" **without a reset** (this PoC explicitly does not inherit tincan's single-call-per-boot limitation).
3. **Outbound call through 3CX.** Dial `9<number>` from the keypad and press ENT. **Known limitation:** the real T-Deck MAX keypad only has `0`, DEL, and ENT mapped today (#8) -- digits 1-9 sit behind an unconfirmed ALT/SYM shift layer, so a real phone number generally can't be typed yet. Until that's mapped, exercise this path by temporarily hardcoding a test number in `app_main.cpp`'s dial call, or wait for the keymap follow-up. Once dialed: expect drawbridge's anchor-call activity to fire and the call to connect through 3CX with two-way audio.
4. **Inbound call from 3CX.** Have someone call the 3CX DN drawbridge is configured to anchor. Expect this device to ring (RING-ALL fork, first answer wins) alongside any other registered extension -- answerable from the keypad exactly like step 2.
5. **Second call, no reboot.** Repeat step 2 or 3 a second time without power-cycling the device, to confirm no single-call-per-boot regression.
6. **Busy handling.** While already in a call (from step 2 or 3), have a third party call this device. Expect a 486 Busy Here (`tincan_uac.cpp`'s `handleInboundInvite` early-reject branch) -- the caller should hear busy/rejection, not silence or a hang.
7. **Race: dial-out vs. inbound.** While mid-dial (after pressing ENT in step 3, before it resolves), have someone call this device. **Expected failure, not a bug to chase during this bench test:** `placeCall()` blocks the loop and drains no other traffic while waiting, so this inbound call gets no SIP response at all until the outbound attempt resolves -- documented in `tincan_uac.hpp`. The caller's UAC will eventually time out. Confirming this behavior (rather than a crash or hang) is the point of this step.

## What "done" looks like

Steps 1, 2, 4, 5, and 6 passing constitutes proof that the PoC's core claim
holds: a phone on this hardware can register to drawbridge and both place
and receive calls routed through 3CX, entirely through a keypad + e-paper
UI, with no 3CX credentials or API logic ever touching this device. Step 3
is currently gated on the keypad digit-entry follow-up (#8); step 7 is a
known, documented limitation, not a blocker.
