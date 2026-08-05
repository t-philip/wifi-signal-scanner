# WiFi Signal Scanner (ESP32)

A live WiFi signal-strength meter on an ESP32 with a built-in ST7789 TFT. Shows a
WiFi arc icon, a plain-language strength label (`LOST` → `EXCELLENT`), the RSSI in
dBm, and a scrolling colour-coded history graph so you can see the signal change
as you walk around.

You pick the network **in a browser**, not in code. Nothing needs recompiling to
point it at a different WiFi.

---

## What it does

- **Live RSSI** with a colour-coded strength label and a scrolling history graph
- **Browser-based setup** — the device raises its own access point, you pick your
  network from a scanned list and type the password
- **Credentials persist** in NVS across power cuts *and* firmware reflashes
- **Hold BOOT for 3 s** to forget the network and start over, with an on-screen
  countdown that aborts if you let go
- **Never reboots on signal loss** — a lost signal is the measurement, not a fault

## Hardware

| Item | Notes |
|---|---|
| ESP32 dev board with built-in ST7789 TFT | 135×240. Developed on an ideaspark board. |
| USB cable | For flashing and power. Runs from any USB supply afterwards. |

**If you get a blank or white screen, the pin mapping is wrong for your board.**
This is the most common first-build failure. The defines at the top of
`src/main.cpp` are for the ideaspark board:

```
MOSI 23   SCLK 18   CS 15   DC 2   RST 4   BL 32
```

The widely-copied LilyGO T-Display pinout (MOSI 19, CS 5, DC 16, RST 23, BL 4) is
**different** and will not work here. Check your board's own documentation.

## Build and flash

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/t-philip/wifi-signal-scanner.git
cd wifi-signal-scanner
pio run --target upload
```

The platform version is pinned in `platformio.ini`. That's deliberate — an
unpinned `espressif32` silently drifts to whatever is newest, which means a clone
months from now builds against something never tested.

## First run

1. Power the board. It shows a **WiFi SETUP** screen with an access point name and
   an 8-character password (unique to your board, derived from its MAC).
2. Join that access point from a phone or laptop.
3. Open **`http://192.168.4.1`**. Don't wait for a captive-portal popup to appear
   on its own — that's unreliable on modern phones. Type the address.
4. Pick your network from the list, enter its password, save.

It then connects and switches to the signal display. On every later boot it
reconnects automatically — you'll see `Connecting to SAVED AP` if you have a
serial monitor attached.

The setup portal stays open indefinitely until a network is chosen. There's no
timeout, because a signal scanner with no network selected has nothing useful to
do; pulling the power is the natural way to cancel.

## Changing network

**Hold the BOOT button for 3 seconds.** A countdown appears; release before it
finishes to cancel. When it completes, the saved network is forgotten and the
setup portal reopens.

## Troubleshooting

The setup screen shows a **"Last attempt"** area with two lines: a plain-language
hint, and the raw 802.11 disconnect reason underneath it.

**Read the raw reason, not the hint.** Arduino's `wl_status_t` collapses several
unrelated failures into `WL_STATION_WRONG_PASSWORD`, which sends you chasing a
password problem that may not exist. The raw reason distinguishes them:

| Raw reason | Actually means |
|---|---|
| `MIC_FAILURE`, `4WAY_TIMEOUT` | Genuinely wrong password — or a PMF/handshake mismatch |
| `AKMP_INVALID`, `IE_INVALID` | Security mismatch, typically WPA2 vs WPA3/SAE |
| `ASSOC_TOOMANY` | The access point is full |
| `NO_AP_FOUND` | Wrong band, hidden SSID, or out of range |
| `AUTH_FAIL` | Rejected at authentication — check MAC filtering |

**On a WPA/WPA2-mixed or WPA-only network**, the Arduino core refuses to associate
with anything advertising weaker than WPA2, and reports it as a wrong password.
There's a commented note in `src/main.cpp` showing the one-line change
(`WiFi.setMinSecurity`) if that's your situation. It's off by default because it
lowers the security floor.

## One finding worth knowing

If you're building anything with WiFiManager, this one cost real debugging time:

**Do not call `WiFi.mode(WIFI_STA)` before `wm.autoConnect()`.** `autoConnect()`
brings the radio up itself via `WiFi_enableSTA`. Setting the mode beforehand,
while Arduino's WiFi persistence is still enabled, writes WiFi state to NVS behind
WiFiManager's back — something the library explicitly guards against internally.

The symptom is badly misleading: connections fail with
`WL_STATION_WRONG_PASSWORD` on a network whose password is completely correct.
That sent the investigation after the password, the router's access-control list,
and the platform version in turn, before the actual cause turned out to be a
single line of setup ordering.

## Design

[docs/DESIGN_SPEC.md](docs/DESIGN_SPEC.md) explains how this is built and why — the
Setup/Monitor state machine, why the portal is non-blocking, the raw-802.11-reason
diagnostic, the AP-password threat model, and an honest verification status: this
firmware has no test harness and no CI, so "verified" always means "flashed and
observed," never "test suite passed."

---

Licensed under [GPL-3.0](LICENSE). Please credit **t-philip** if you use or share this.

Built and maintained by [t-philip](https://github.com/t-philip).
