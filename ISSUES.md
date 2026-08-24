# Issue Tracking & Architectural Roadmap

This document is the active issue tracker for **tdeck-max-phone**. Entries below came out of a
code-review sweep of `main/src/net_wifi.c` while evaluating it for reuse in a downstream project
(`glopanel`). Each was verified against the files in this repo; where a claim turned out **not** to
be a defect it is recorded as a portability note rather than deleted, so the distinction is durable.

---

## Active Issues & Backlog Roadmap

### 🟡 Issue #1: `wifi_is_connected()` never returns false after the first successful connect
* **Status**: ⏳ Open
* **Labels**: `bug`, `wifi`, `state`
* **Severity**: Medium — silently wrong; breaks any UI that displays link state

#### Description
`main/src/net_wifi.c:35` sets `WIFI_GOT_IP_BIT` on `IP_EVENT_STA_GOT_IP`, and
`wifi_is_connected()` (`:77-81`) reports exactly that bit:

```c
return (xEventGroupGetBits(s_wifi_evt) & WIFI_GOT_IP_BIT) != 0;
```

The `WIFI_EVENT_STA_DISCONNECTED` branch (`:28-30`) logs and re-calls `esp_wifi_connect()`, but
**never clears the bit**. Once the device has connected a single time, `wifi_is_connected()` returns
`true` permanently — including while the AP is powered off and the station is retrying.

Anything that renders a Wi-Fi-up/down indicator, or gates work on link state, is therefore
displaying a latch rather than the truth. The failure is invisible in normal operation and only
appears when the network drops, which is precisely when the indicator matters.

**Fix:** clear the bit in the disconnect branch:

```c
} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT);
    ESP_LOGW(TAG, "disconnected; retrying");
    esp_wifi_connect();
}
```

**In-tree precedent for the correct behaviour:** `mockingbird-scrivener/main/main.c:73` clears its
equivalent state (`s_ip[0] = '\0'`) on disconnect.

---

### 🟡 Issue #2: Unconditional `ESP_ERROR_CHECK` on `esp_netif_init()` / `esp_event_loop_create_default()`
* **Status**: ⏳ Open
* **Labels**: `bug`, `robustness`, `wifi`, `latent`
* **Severity**: Medium — latent; aborts if the call order ever changes

#### Description
`main/src/net_wifi.c:43-44`:

```c
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
```

Both return `ESP_ERR_INVALID_STATE` if something already created them, and `ESP_ERROR_CHECK` turns
that into an abort. This is safe in the current call order (nothing else initialises them first) but
breaks the moment `wifi_sta_connect()` is called twice, or any other subsystem initialises netif or
the default event loop before it — a re-entrant "leave and return" screen, for example.

**Fix:** tolerate `ESP_ERR_INVALID_STATE` on both. There is a working reference for the idempotent
form in a sibling repo: `jc3248-display-driver/main/demo_wifi_battery.c:59-77`
(`ensure_process_globals()`), which handles NVS, netif and the event loop across repeated entries.

---

### 🔵 Issue #3: No backoff on reconnect — an AP outage produces a reconnect storm
* **Status**: ⏳ Open / Backlog
* **Labels**: `robustness`, `wifi`, `long-running`
* **Severity**: Low — acceptable for a demo, not for an always-on device

#### Description
`main/src/net_wifi.c:28-30` calls `esp_wifi_connect()` immediately on every
`WIFI_EVENT_STA_DISCONNECTED`, directly from the event-loop task, with no delay. Through a router
reboot this becomes a tight retry loop for as long as the AP is down.

**Fix:** add a backoff (a timer, or an incrementing delay), or set
`wifi_config.sta.failure_retry_cnt`.

---

### 🔵 Issue #4: `s_ip` is written from the event-loop task and read unsynchronised
* **Status**: ⏳ Open / Backlog
* **Labels**: `concurrency`, `wifi`, `latent`
* **Severity**: Low

#### Description
`main/src/net_wifi.c:22` declares `static char s_ip[16]`, written by `esp_ip4addr_ntoa()` at `:33`
from the event-loop task and returned by `wifi_local_ip()` (`:83`) to any caller with no
synchronisation. A caller that displays the string while a reconnect rewrites it can observe a torn
value. Low impact — worst case is a briefly garbled IP on screen — but it is a genuine data race.

**Fix:** copy under a mutex, or have `wifi_local_ip()` take a caller-supplied buffer and fill it
from an `esp_netif_get_ip_info()` call in the caller's own context.

---

## Portability notes (not defects in this repo)

- **`net_wifi.c` does not call `nvs_flash_init()`, and does not need to.** `esp_wifi_init()`
  requires NVS for calibration data, and `wifi_sta_connect()` never initialises it — but
  `main/src/app_main.cpp:542` does (`try_init("nvs_flash", nvs_flash_init())`), well before the
  `wifi_sta_connect()` call at `:587`. **The order is correct as it stands.** This is recorded only
  because it makes `net_wifi.c` non-self-contained: lifting the file into another project without
  also calling `nvs_flash_init()` first will fail inside `esp_wifi_init()`.

- **The bounded connect wait is deliberate and correct** (`net_wifi.c:62-73`). The 30 s timeout with
  an explanatory comment — rather than `portMAX_DELAY` — is the right call, and the driver keeps
  retrying in the background afterwards. Consumers should therefore **not** wrap `wifi_sta_connect()`
  in `ESP_ERROR_CHECK`: a timeout is a "show no-network state" condition, not a fatal one. Consumers
  driving a display should also render something *before* calling it, since 30 s of blocking on a
  wrong PSK is otherwise indistinguishable from a hung boot.
