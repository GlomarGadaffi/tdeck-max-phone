# Architecture & System Design (`tdeck-max-phone`)

This document details the system design, dual-core task affinity, 3CX integration, and audio pipeline for `tdeck-max-phone`.

---

## 1. Dual-Core Task Allocation (ESP32-S3)

The ESP32-S3 dual-core LX7 processor is allocated as follows:

```
                      +---------------------------------------+
                      |         ESP32-S3 Dual-Core            |
                      +-------------------+-------------------+
                                          |
                 +------------------------+------------------------+
                 |                                                 |
      +----------v----------+                           +----------v----------+
      |        Core 0       |                           |        Core 1       |
      | Network & PBX Engine|                           | Handset Audio & UI  |
      +----------+----------+                           +----------+----------+
                 |                                                 |
  • pocket-dial SIP Server & Registrar           • tincan Full-Duplex Audio Loop
  • 3CX TelephonyAnchorClient (WSS & HTTPS)      • ES8311 I2S DMA Sampling (8 kHz)
  • LWIP Sockets & Cellular PPP Netif            • TCA8418 Keypad Scanning & Interrupts
  • FreeRTOS WS Work Queue Drainers              • E-Paper Display Partial Redraws
```

---

## 2. 3CX Route Point API Integration

`tdeck-max-phone` uses the **Telephony Route Point API** (ported from `drawbridge`):

1. **OAuth Token**: Obtains access token via `POST https://<3cx-host>/connect/token`.
2. **WebSocket Control**: Connects to `wss://<3cx-host>/callcontrol/ws` for real-time JSON events (`Upset` for participant state, `Remove` for teardown).
3. **Audio Streaming**: Performs HTTP chunked `POST` (mic TX) and `GET` (speaker RX) to `/callcontrol/{dn}/participants/{id}/stream`.
4. **TLS Session Resumption**: Reuses mbedTLS session handles across calls to achieve $\sim 100\text{ ms}$ resumed handshakes.

---

## 3. Audio Pipeline Topology

```
Local Mic  ---> ES8311 Codec (I2S) ---> tincan UAC ---> Local PBX (pocket-dial) ---> 3CX Anchor Client ---> 4G PPP ---> 3CX Server
Local Spk  <--- ES8311 Codec (I2S) <--- tincan UAC <--- Local PBX (pocket-dial) <--- 3CX Anchor Client <--- 4G PPP <--- 3CX Server
```
