# WiFi Signal Scanner
## Design Specification v1.0 — As-Built

**Author:** T. Philip — <https://github.com/t-philip>
**Date:** 5 August 2026
**Status:** Reconciled against the shipped code. Describes what the firmware actually
does, including one defect found while writing this document and fixed before this
spec's first release, and one known limitation left open on purpose.
**Licence:** AGPL-3.0, same as the code. Relicensed from GPL-3.0 on 2026-08-06 — see §10.

---

## 0. How to read this document

[README.md](../README.md) explains how to *build, flash and use* the scanner. This
explains *why* it works the way it does — the state machine, the two library pitfalls
that cost real debugging time, and what "verified" actually means for a single-file
firmware project with no test harness and no compiler available on the machine that
wrote this document.

There is no pre-implementation brief for this project (unlike `sky-radar`'s
`SKYRADAR_BRIEF.md`) — it grew directly from an earlier hardcoded-network sketch inside
a private monorepo, iterated against real hardware, and was published only once working.
So this document has no brief to reconcile against; instead §7 reconciles every claim in
the README against `src/main.cpp` directly.

---

## 1. Purpose and scope

### 1.1 In scope

A single-purpose ESP32 firmware: show live WiFi signal strength for whichever network
the device is connected to, on its built-in 135×240 ST7789 TFT — an arc icon, a
plain-language strength label, the RSSI in dBm, and a scrolling colour-coded history
graph. Network selection happens entirely in a browser via a captive portal; nothing is
hardcoded and nothing needs recompiling to point the device at a different network.

### 1.2 Out of scope

- **Not a general WiFi diagnostic tool.** It reports RSSI and the raw 802.11 disconnect
  reason for the *current* association attempt only — no packet capture, no channel
  scanning/site-survey mode, no throughput testing.
- **Not multi-network.** One stored network at a time, in the ESP32's own WiFi NVS.
  Switching networks means deliberately forgetting the current one (§3.4).
- **Not remotely configurable.** The only interfaces are the physical BOOT button and
  the one-time setup portal. There is no ongoing web server, API, or OTA update path
  once a network is joined.

### 1.3 Design priorities, in order

1. **Never lose the diagnostic channel.** Opening a serial monitor on the target
   (ideaspark) board resets it via the CH340's DTR/RTS lines, so the TFT panel is the
   *only* diagnostic surface that doesn't destroy what it's observing. Every design
   decision below that looks unusual exists to keep useful information on that panel.
2. **A weak or lost signal is the measurement, not a fault.** The firmware must never
   reboot, reset its history, or otherwise treat "bad signal" as an error condition —
   that is precisely the thing a user opened this tool to see.
3. **Setup must recover from being wrong.** A wrong password, an out-of-range network,
   or a security-mode mismatch must be diagnosable from the panel alone, not require
   re-flashing or guessing.

---

## 2. Architecture

Single compilation unit, `src/main.cpp`, ~450 lines. No web server code of its own —
the captive portal is entirely owned by the `WiFiManager` library.

```
 power-on
    │
    ▼
 setup()
    │  WiFi.onEvent() registered (captures raw 802.11 disconnect reason)
    │  wm.setConfigPortalBlocking(false)
    │  wm.autoConnect(ssid, password) ─── stored creds in NVS? ──yes──► connects synchronously
    │           │                                                        (up to CONNECT_TIMEOUT_S)
    │           no creds, or connect failed
    │           ▼
    │     mode = Setup                                    mode = Monitor
    │     portal AP raised, non-blocking                        │
    │           │                                                │
    └───────────┴──────────────────────┬─────────────────────────┘
                                        ▼
                                     loop()
                     ┌──────────────────┴──────────────────┐
                     │ mode == Setup                        │ mode == Monitor
                     │  wm.process() services the portal     │  reconnect backstop every 30s
                     │  drawSetupStatus(): hint + raw 802.11  │  RSSI → level → icon/label/graph
                     │  reason, redrawn only on change        │  never reboots on disconnect
                     │  transitions to Monitor once            │
                     │  WL_CONNECTED && portal closed          │
                     └───────────────────────────────────────┘
                                        │
                     handleResetButton() runs every iteration, both modes:
                     BOOT held 3s → wm.resetSettings() → ESP.restart() → back to Setup
```

Two `Mode` values (`Setup`, `Monitor`) are the entire state machine. Everything else —
the portal's own internal state, `WiFiManager`'s connection retries, the ESP32 core's
`WiFi.setAutoReconnect` — is owned by libraries and only observed here.

---

## 3. Key design decisions

### 3.1 The portal is non-blocking, for three stated reasons

From the source comment, verified against the code:

1. **The portal stays open until a network is chosen.** No timeout, because a scanner
   with no network selected has nothing useful to do — confirmed: `setConfigPortalTimeout`
   is never called, so it defaults to 0 (no timeout).
2. **`loop()` keeps running while the portal is up**, so the panel can report why the
   last attempt failed (§3.3) — the diagnostic channel that priority 1 in §1.3 depends on.
3. **The ESP32 core keeps retrying a stored network in the background** during setup, so
   a router that was merely slow or briefly absent recovers without the user doing
   anything.

### 3.2 `WiFi.mode(WIFI_STA)` is deliberately never called

The single most expensive lesson behind this codebase, called out with its own
paragraph in `src/main.cpp` and repeated in the README's "One finding worth knowing"
section:

`wm.autoConnect()` brings the radio up itself via `WiFi_enableSTA`. Calling
`WiFi.mode(WIFI_STA)` beforehand, while Arduino's own WiFi persistence is still
enabled, writes WiFi state to NVS **behind WiFiManager's back** — something the library
explicitly guards against internally. The failure mode is `WL_STATION_WRONG_PASSWORD`
on a network whose password is completely correct, which sends debugging down the wrong
path entirely (password, then AP access-control list, then platform version) before the
real cause — one line of setup ordering — is found. This firmware matches the working
`spotify-nowplaying` bring-up exactly (same core, same library, same network) rather
than re-deriving the ordering from scratch.

### 3.3 Raw 802.11 disconnect reason, alongside the Arduino status

Arduino's `wl_status_t` is lossy by design: AKMP/cipher mismatches, an AP at its
association limit, and a genuinely wrong PSK all collapse into the single value
`WL_STATION_WRONG_PASSWORD`. `onWiFiEvent()` captures the underlying `reason` field from
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED` into `lastDisconnectReason`, and
`disconnectReasonName()` maps the codes worth distinguishing (`MIC_FAILURE` and
`4WAY_TIMEOUT` for a genuinely wrong PSK; `AKMP_INVALID`/`IE_INVALID` for a WPA2-vs-WPA3
mismatch; `ASSOC_TOOMANY` for a full AP; `NO_AP_FOUND` for out of range or hidden SSID).
Both the plain-language hint and the raw reason are shown together — deliberately, since
the hint can be wrong without the underlying diagnosis being lost.

### 3.4 BOOT button is edge-triggered, not level-triggered

GPIO0 (the BOOT button) is wired into the ideaspark board's auto-reset circuit alongside
the CH340 USB bridge's DTR/RTS lines, so with USB attached it can read LOW with nobody
pressing anything. `handleResetButton()` requires an observed HIGH → LOW transition
before it will start counting a hold; a pin that has never been seen HIGH (stuck low
from boot, or held low by the USB bridge) can never trigger the 3-second countdown that
forgets the stored network. This is the one place in the firmware where getting the
detail wrong has a destructive consequence — an accidental credential wipe — so it gets
the most defensive treatment in the file.

### 3.5 No reboot on disconnect, ever

Consistent with priority 2 in §1.3: `loop()`'s Monitor-mode path has no reboot logic at
all. A lost or degraded signal is displayed (`LOST`, red, RSSI clamped) and logged into
the scrolling history — rebooting would erase that history at exactly the moment it
becomes interesting.

### 3.6 A 30-second manual reconnect backstop, alongside `WiFi.setAutoReconnect(true)`

`loop()` calls `WiFi.reconnect()` on a 30-second timer whenever `WiFi.status() !=
WL_CONNECTED`, in addition to the ESP32 core's own automatic reconnection
(`WiFi.setAutoReconnect(true)`, set once in `setup()`). The source comment explains the
30-second interval is deliberately long: forcing `WiFi.reconnect()` on a short timer
tears down whatever attempt the core's own backoff already has in flight, resetting
that backoff and slowing recovery rather than helping — this is exactly the "over-eager
reconnect" bug fixed earlier in this project's private history (`7a7ff62`).

**Audited and left as-is.** I considered removing the manual backstop as redundant with
`setAutoReconnect`, but there is no visibility from the sketch into whether the core's
own reconnect logic is mid-attempt at any given moment, and the interval is already
tuned long enough that collisions should be rare. Given `7a7ff62` shows reconnect
logic in this exact file has regressed once before, and there is no hardware available
from the machine that wrote this document to verify a change here, this was judged not
worth the risk for a change with no reported symptom behind it. Recorded as a
considered-and-rejected fix, not an oversight.

### 3.7 Platform and library versions are pinned

`platformio.ini` pins `platform = espressif32@^6.5.0` with a comment explaining why: an
earlier unpinned build silently drifted to 7.0.1, meaning a clone months later would
build against something never tested on real hardware. The caret restricts PlatformIO
to `6.5.0 ≤ version < 7.0.0` — not a floor, a ceiling. `WiFiManager @ ^2.0.17` follows
the same convention.

---

## 4. Security model

### 4.1 Portal AP password

`apPassword()` derives an 8-hex-character password (32 bits) from the lower 32 bits of
the chip's eFuse MAC address — stable per device, printed on the panel, and **not** one
default shared across every build of this firmware. WiFiManager's minimum AP password
length is 8 characters; the function always returns exactly 8, so it's never rejected.

**Honestly stated limit:** this is proximity security, not cryptographic security. The
password is derived from a value (the chip's MAC) that is, in principle, inferable by
anyone who can already see the device's WiFi traffic — though at setup time, before any
network is joined, there is no traffic to observe it from. The realistic threat model is
"someone in the same room during the setup window," which the on-screen password already
assumes.

### 4.2 The stored password is never shown

`drawSetupStatus()` shows the stored SSID in full, but for the password shows only its
**length** and whether it contains an ampersand — enough to prove or disprove truncation
somewhere in the browser → form → `WebServer` → NVS chain, without ever putting the
secret itself on a screen anyone else in the room can read.

### 4.3 Known limitation, not fixed here: every device broadcasts the same portal SSID

`AP_SSID` is the fixed string `"WiFiScanner-Setup"`, identical across every board
running this firmware. Two boards in range simultaneously (plausible for anyone who owns
more than one, as this project's author does — see `wifi-chatroom` in the same private
monorepo) are indistinguishable in a WiFi scan; only the *password* differs per device,
so joining the wrong one just fails to authenticate rather than misconfiguring anything,
but it is a real point of confusion during setup.

**A fix is straightforward but deliberately not made in this pass:** suffix the SSID
with a few hex characters derived from a portion of the MAC **disjoint** from the one
`apPassword()` already uses (its upper 16 bits, since the password uses the lower 32) —
sharing bits between the publicly-broadcast SSID and the password would leak part of the
password to anyone in WiFi range, not just anyone who can read the screen, which would be
a regression, not an improvement. This touches the exact WiFi bring-up path that
regressed once before (§3.2), and there is no hardware here to verify it against. Left
for a session where it can be flashed and tested. Tracked as future work (§8).

---

## 5. Display design

135×240 portrait TFT (`setRotation(2)`). `drawCentered()` centres text and truncates
rather than overrunning the panel — the default GFX font advances 6px per character, so
at text size *n* anything over `135 / (6·n)` characters would run off the 135px width;
the guard lives once in the helper rather than being recomputed at every call site.

RSSI-to-label thresholds (`getSignalLevel`), unchanged by this document's fix:

| RSSI | Label | Colour |
|---|---|---|
| ≤ -95 dBm, or ≥ 0 (invalid) | LOST | red |
| ≤ -85 dBm | V. WEAK | orange |
| ≤ -75 dBm | WEAK | yellow |
| ≤ -67 dBm | GOOD | cyan |
| ≤ -58 dBm | STRONG | light green |
| better | EXCELLENT | green |

---

## 6. Verification status

**Stated honestly**, per this document's own §1.3 priority on recoverable, diagnosable
behaviour applied to the document itself.

### 6.1 Verified on real hardware (prior sessions, before this document)

Per the private-history commit `bf20d59` ("verified working on hardware; edge-trigger the
BOOT button") and the README's own claims: the non-blocking portal, the BOOT-button
forget flow, WPA2-floor diagnosis, and normal signal-monitoring operation were confirmed
on an ideaspark ESP32 board. This predates and is unaffected by the fix in §7.

### 6.2 Verified by this document: code review, not hardware

**No PlatformIO installation exists on the machine that wrote this document**, so the
fix in §7 could not be compile-checked, let alone flashed. What was done instead:

- The full file was re-read after every edit for syntax correctness (balanced braces,
  consistent types, correct use of `constrain()` and `map()` — both standard Arduino
  macros already used elsewhere in the file).
- Every call site of the changed constants (`RSSI_MIN`, the new `RSSI_NONE`) and the
  changed variable (`currentRSSI`) was traced through `getSignalLevel()`, the dBm text
  path, and the graph-draw loop to confirm the sentinel is handled consistently
  everywhere it appears.
- The change is additive and narrowly scoped: one new constant, four call sites,
  no change to WiFi bring-up, portal handling, or the reset-button logic.

**This is reasoned, not demonstrated.** It has not been flashed to a board. Recommended
before trusting it in the field: flash it, and check the history graph draws a
continuous flat line (not a gap) while sitting at the edge of range with a real, weak
but connected signal.

### 6.3 Not covered at all

No test harness exists for this project — firmware on a single-purpose microcontroller
has no meaningful way to unit-test against real radio behaviour from this machine. There
is no CI. Verification for a change to this codebase will always mean "flashed and
observed," never "test suite passed."

---

## 7. README claims checked against the code

Every claim in `README.md` was checked against `src/main.cpp` while writing this
document. All reconciled:

| README claim | Code location | Verdict |
|---|---|---|
| Credentials persist across power cuts and reflashes | WiFiManager → ESP32 WiFi NVS, a separate flash region from the app partition `pio run --target upload` writes | Accurate |
| Portal never times out | `setConfigPortalTimeout` never called (default 0) | Accurate |
| WPA/WPA2-mixed networks report as "wrong password" | Arduino core's `_minSecurity = WIFI_AUTH_WPA2_PSK` floor; commented-out `setMinSecurity` escape hatch present, off by default | Accurate |
| `WiFi.mode(WIFI_STA)` must never be called first | Confirmed absent from `setup()`; §3.2 | Accurate |
| Never reboots on signal loss | Confirmed — no reboot path exists in Monitor mode | Accurate |
| Hold BOOT 3s to forget, countdown aborts on release | `handleResetButton()`, §3.4 | Accurate |

No divergence found.

---

## 8. Gap found while writing this document — and fixed

Writing this specification surfaced one defect, in `src/main.cpp`. It is fixed as of
commit `692c5f9` in the private history this firmware is developed in, applied to this
repository in the same release as this document.

### 8.1 The history graph showed a gap during a real, sustained weak signal

`RSSI_MIN` (-100) was used for two unrelated purposes at once: the graph's axis floor,
and the sentinel value written into `history[]` when the device is disconnected. A
genuinely connected reading at or below -95 dBm — correctly labelled `LOST` in red, but
still a real measurement — was numerically indistinguishable from "no reading, not
connected." The graph-draw loop skipped drawing between two such points, so a device
sitting at the edge of range for an extended period showed a **blank gap** in its own
history, rather than the flat line at the bottom of the axis that the situation actually
called for. The dBm readout had a smaller version of the same problem: it printed
"`-100 dBm`" while disconnected, a plausible-looking but fabricated number.

**Fix.** A separate sentinel, `RSSI_NONE` (-999), is now written only when
`WiFi.status() != WL_CONNECTED`. Real readings — including ones at or beyond the axis
bounds — are always plotted, clamped into `[RSSI_MIN, RSSI_MAX]` via `constrain()`
before being passed to `map()`. The dBm text now shows `"-- dBm"` specifically when
disconnected, rather than a number that looks real but isn't. A boundary segment (one
real point next to one `RSSI_NONE` point) still draws, clamped to the bottom of the
axis — the same visual the old code produced for a genuine disconnect transition, so
that part of the behaviour is unchanged.

*Verified:* by code review only, per §6.2. Not yet flashed.

---

## 9. Possible future work

1. **§4.3** — suffix the portal SSID with MAC-derived characters disjoint from the
   password, so two boards in range are distinguishable without weakening either.
2. **Flash and confirm §8's fix** on real hardware — the graph should now show a
   continuous flat line, not a gap, when held at the edge of range.
3. A site-survey / channel-scan mode is a natural extension but is explicitly out of
   scope (§1.2) for this build.

---

## 10. Licence and provenance

Published under **AGPL-3.0** alongside the code it describes.

**Licence history.** This repository was published under GPL-3.0 until 2026-08-06, when
it was relicensed to AGPL-3.0. The change adds §13 (Remote Network Interaction): anyone
who modifies this code and makes it available to users over a network must offer those
users the corresponding source. Copyright is held solely by T. Philip, so the change was
made without third-party consent. Releases already published under GPL-3.0 remain
available under GPL-3.0 to anyone who received them — that grant is not revocable.

Written and built by **T. Philip** — <https://github.com/t-philip>.
Repository: <https://github.com/t-philip/wifi-signal-scanner>.
