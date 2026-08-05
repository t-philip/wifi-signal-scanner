#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// WiFi Signal Scanner
//
// Shows live signal strength for the network you're connected to, on the
// board's built-in ST7789 TFT: a WiFi arc icon, a human-readable label
// (LOST -> EXCELLENT), the RSSI in dBm, and a scrolling colour-coded history
// graph.
//
// Network selection happens in a browser, not in code. With no stored network
// the device raises its own access point and serves a captive portal: join it,
// pick your network from the scanned list, enter the password. WiFiManager
// persists it to the ESP32's own WiFi NVS, so it survives both a power cut and
// a firmware reflash (only a full chip erase or the BOOT-button reset clears it).
//
// The portal runs in NON-BLOCKING mode. That matters for three reasons:
//   1. The portal stays open until a network is actually chosen -- there is no
//      useful state for a scanner to time out into.
//   2. loop() keeps running while the portal is up, so the panel can report why
//      the last connection attempt failed. On this board opening the serial
//      port asserts DTR/RTS and resets it, so the display is the only
//      diagnostic channel that doesn't destroy what it's trying to observe.
//   3. The ESP32 core keeps retrying a stored network in the background, so a
//      router that was merely slow or briefly absent recovers on its own.
//
// Hold BOOT for 3 s to forget the saved network and reopen the portal. The hold
// draws a countdown and aborts if you let go.
// ---------------------------------------------------------------------------

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   32

#define SCREEN_W 135
#define SCREEN_H 240
#define RSSI_MIN -100
#define RSSI_MAX -30

// Sentinel for "no reading" (WiFi disconnected) -- distinct from any real RSSI
// value. RSSI_MIN used to double as this sentinel, which meant a genuinely
// connected but very weak signal (<=-95dBm, labelled LOST but still a real
// reading) drew as a *gap* in the history graph, indistinguishable from having
// lost the connection entirely. RSSI_NONE only ever comes from "not connected";
// a real reading is always plotted, clamped to the axis if it falls outside it.
#define RSSI_NONE -999

// Portal AP. The password derives from the chip's eFuse MAC so it is stable per
// device and printed on the panel -- not one default shared by every build.
static const char* AP_SSID = "WiFiScanner-Setup";

static const uint32_t BTN_RESET_HOLD_MS  = 3000;
static const uint32_t CONNECT_TIMEOUT_S  = 20;

// Long on purpose. The core is already retrying with its own backoff; forcing
// WiFi.reconnect() on a short timer tears down the in-flight attempt and resets
// that backoff, slowing recovery rather than helping.
static const uint32_t RECONNECT_EVERY_MS = 30000;

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
WiFiManager wm;

enum class Mode { Setup, Monitor };
Mode mode = Mode::Setup;

// Raw 802.11 disconnect reason from the WiFi event, captured because Arduino's
// wl_status_t is lossy: AKMP/cipher mismatches, association limits and a genuine
// PSK failure all collapse into WL_STATION_WRONG_PASSWORD, which sends you
// chasing the password when the cause may be nothing to do with it.
volatile int lastDisconnectReason = -1;

// Only the codes worth distinguishing here. Kept short to fit 22 chars at
// text size 1 on a 135 px panel.
const char* disconnectReasonName(int r) {
    switch (r) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 4:   return "ASSOC_EXPIRE";
        case 5:   return "ASSOC_TOOMANY";     // AP is full / client limit
        case 6:   return "NOT_AUTHED";
        case 7:   return "NOT_ASSOCED";
        case 8:   return "ASSOC_LEAVE";
        case 13:  return "IE_INVALID";        // often WPA3/PMF mismatch
        case 14:  return "MIC_FAILURE";       // genuinely wrong PSK
        case 15:  return "4WAY_TIMEOUT";      // wrong PSK, or PMF/handshake
        case 16:  return "GROUP_KEY_TIMEOUT";
        case 17:  return "IE_IN_4WAY_DIFFERS";
        case 18:  return "GRP_CIPHER_INVALID";
        case 19:  return "PAIR_CIPHER_INVALID";
        case 20:  return "AKMP_INVALID";      // WPA2 vs WPA3/SAE mismatch
        case 23:  return "802_1X_AUTH_FAILED";
        case 24:  return "CIPHER_SUITE_REJECT";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "";
    }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        lastDisconnectReason = info.wifi_sta_disconnected.reason;
    }
}

int history[SCREEN_W];
String lastLabel = "";
int lastRSSI = 0;
int lastLevel = -1;
String lastSSID = "";
uint8_t lastConxShown = 255;   // force a first draw

// --- Portal AP password: 8 hex chars from the eFuse MAC --------------------
String apPassword() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[9];
    snprintf(buf, sizeof(buf), "%08llx", (unsigned long long)(mac & 0xFFFFFFFFULL));
    return String(buf);
}

// --- Centred text helper --------------------------------------------------
// Truncates to fit rather than letting text run off the panel. The default GFX
// font advances 6 px per character, so at size 2 anything over 11 characters
// overruns these 135 px -- narrow enough to breach by accident, so the guard
// lives here instead of at every call site.
void drawCentered(const String &text, int y, uint8_t size, uint16_t color) {
    const int charW = 6 * size;
    const int maxChars = SCREEN_W / charW;

    String shown = text;
    if ((int)shown.length() > maxChars) {
        shown = (maxChars > 3) ? shown.substring(0, maxChars - 3) + "..."
                               : shown.substring(0, maxChars);
    }

    int16_t x1, y1; uint16_t w, h;
    tft.setTextSize(size);
    tft.setTextColor(color);
    tft.getTextBounds(shown, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((SCREEN_W - (int)w) / 2, y);
    tft.print(shown);
}

// --- Signal strength to human readable label ------------------------------
int getSignalLevel(int rssi, String &label, uint16_t &color) {
    if (rssi <= -95 || rssi >= 0) { label = "LOST";      color = ST77XX_RED;    return 0; }
    if (rssi <= -85)               { label = "V. WEAK";   color = ST77XX_ORANGE; return 1; }
    if (rssi <= -75)               { label = "WEAK";      color = ST77XX_YELLOW; return 2; }
    if (rssi <= -67)               { label = "GOOD";      color = 0x07FF;        return 3; }
    if (rssi <= -58)               { label = "STRONG";    color = 0xAFE5;        return 4; }
                                     label = "EXCELLENT"; color = ST77XX_GREEN;  return 5;
}

// --- Draw WiFi Waves (Arcs) -----------------------------------------------
void drawWifiIcon(int x, int y, int level, uint16_t color) {
    tft.fillRect(x - 30, y - 5, 60, 35, ST77XX_BLACK);
    tft.fillCircle(x, y + 25, 3, (level >= 1) ? color : 0x2104);
    for (int i = 1; i <= 3; i++) {
        uint16_t waveColor = (level >= i + 2) ? color : 0x2104;
        for (int angle = 220; angle <= 320; angle += 2) {
            float rad = angle * 0.0174533;
            int radius = 10 + (i * 6);
            tft.drawPixel(x + cos(rad) * radius,       y + 25 + sin(rad) * radius,       waveColor);
            tft.drawPixel(x + cos(rad) * (radius + 1), y + 25 + sin(rad) * (radius + 1), waveColor);
        }
    }
}

// --- Plain-language reading of the last connection attempt -----------------
// The raw enum name is shown alongside this, because the ESP32 core is not
// always precise about which failure it reports -- it will sometimes give
// NO_SSID_AVAIL for an auth failure. Showing both means the hint can be wrong
// without the diagnosis being lost.
void conxHint(uint8_t status, String &line, uint16_t &color) {
    switch (status) {
        case WL_IDLE_STATUS:    line = "No attempt yet";   color = 0x7BEF;        break;
        case WL_NO_SSID_AVAIL:  line = "Network not found"; color = ST77XX_ORANGE; break;
        case WL_CONNECT_FAILED: line = "Wrong password?";   color = ST77XX_RED;    break;
        case WL_CONNECTION_LOST:line = "Connection lost";   color = ST77XX_ORANGE; break;
        case WL_DISCONNECTED:   line = "Not connected";     color = 0x7BEF;        break;
        case WL_CONNECTED:      line = "Connected";         color = ST77XX_GREEN;  break;
        default:                line = "Failed";            color = ST77XX_RED;    break;
    }
}

// --- Screens ---------------------------------------------------------------
void drawSetupScreen() {
    tft.fillScreen(ST77XX_BLACK);
    drawCentered("WiFi SETUP", 6, 2, ST77XX_CYAN);

    drawCentered("Join this network", 30, 1, 0x7BEF);
    drawCentered(AP_SSID, 43, 1, ST77XX_WHITE);

    drawCentered("Password", 60, 1, 0x7BEF);
    drawCentered(apPassword(), 73, 2, ST77XX_YELLOW);

    drawCentered("Then open", 98, 1, 0x7BEF);
    drawCentered("192.168.4.1", 111, 1, ST77XX_WHITE);

    drawCentered("Pick your WiFi,", 132, 1, 0x7BEF);
    drawCentered("enter its password", 145, 1, 0x7BEF);

    tft.drawFastHLine(6, 165, SCREEN_W - 12, 0x3186);
    drawCentered("Last attempt", 172, 1, 0x4A69);
}

// Redrawn only when the status code changes, so it never flickers.
void drawSetupStatus() {
    uint8_t st = wm.getLastConxResult();
    if (st == lastConxShown) return;
    lastConxShown = st;

    String hint; uint16_t color;
    conxHint(st, hint, color);

    tft.fillRect(0, 184, SCREEN_W, SCREEN_H - 184, ST77XX_BLACK);
    drawCentered(hint, 186, 1, color);
    // Raw enum name. The "WL_" prefix is dropped so the longest of them,
    // STATION_WRONG_PASSWORD (22 chars = 132 px), fits without being truncated
    // -- with the prefix it is 25 chars and gets cut mid-word.
    String raw = wm.getWLStatusString(st);
    if (raw.startsWith("WL_")) raw = raw.substring(3);
    drawCentered(raw, 199, 1, 0x7BEF);

    // What actually made it into NVS. The SSID is shown in full; for the
    // password only its LENGTH and whether it contains an ampersand are shown
    // -- enough to prove or disprove truncation in the browser -> form ->
    // WebServer -> NVS chain without putting the secret on a screen.
    String ssid = wm.getWiFiSSID();
    if (ssid.length() > 0) {
        drawCentered(ssid, 212, 1, 0x4A69);
        // The raw 802.11 reason is the useful number: Arduino collapses
        // AKMP/cipher mismatches, a full AP and a genuinely wrong PSK all into
        // WL_STATION_WRONG_PASSWORD, which misdirects the diagnosis.
        int r = lastDisconnectReason;
        if (r >= 0) {
            String name = disconnectReasonName(r);
            drawCentered(name.length() ? name : ("reason " + String(r)),
                         225, 1, ST77XX_ORANGE);
        }
    } else {
        drawCentered("nothing stored", 212, 1, 0x4A69);
    }
}

// --- BOOT button: hold to forget the saved network -------------------------
void handleResetButton() {
    // Edge-triggered, not level-triggered. GPIO0 is also wired into this board's
    // auto-reset circuit alongside the CH340's DTR/RTS lines, so with USB
    // attached it can read LOW with nobody pressing anything. Acting on the
    // level alone would silently wipe stored credentials. Requiring a HIGH ->
    // LOW transition means a pin held low from boot, or held low by the USB
    // bridge, can never trigger it.
    static bool wasHigh = false;

    if (digitalRead(0) != LOW) { wasHigh = true; return; }
    if (!wasHigh) return;          // never seen HIGH: treat as stuck, ignore
    wasHigh = false;

    uint32_t start = millis();
    bool drewOverlay = false;
    int lastShown = -1;

    while (digitalRead(0) == LOW) {
        uint32_t held = millis() - start;
        if (held >= BTN_RESET_HOLD_MS) {
            tft.fillScreen(ST77XX_BLACK);
            drawCentered("FORGETTING", SCREEN_H / 2 - 20, 2, ST77XX_RED);
            drawCentered("WiFi network", SCREEN_H / 2 + 5, 1, ST77XX_WHITE);
            delay(800);
            wm.resetSettings();
            ESP.restart();
        }

        if (!drewOverlay) {
            tft.fillScreen(ST77XX_BLACK);
            drawCentered("Hold to forget", 60, 1, 0x7BEF);
            drawCentered("saved WiFi", 78, 1, 0x7BEF);
            drawCentered("Release to cancel", 190, 1, 0x7BEF);
            drewOverlay = true;
        }

        int remaining = (int)((BTN_RESET_HOLD_MS - held + 999) / 1000);
        if (remaining != lastShown) {
            tft.fillRect(0, 110, SCREEN_W, 50, ST77XX_BLACK);
            drawCentered(String(remaining), 110, 5, ST77XX_ORANGE);
            lastShown = remaining;
        }
        delay(30);
    }

    // Released early -- force a full repaint of whichever screen belongs here.
    if (drewOverlay) {
        lastLevel = -1; lastLabel = ""; lastSSID = ""; lastRSSI = 0;
        lastConxShown = 255;
        if (mode == Mode::Setup) drawSetupScreen();
        else                     tft.fillScreen(ST77XX_BLACK);
    }
}

void onPortalStart(WiFiManager* /*mgr*/) {
    mode = Mode::Setup;
    lastConxShown = 255;
    drawSetupScreen();
}

void setup() {
    Serial.begin(115200);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    pinMode(0, INPUT_PULLUP);   // BOOT button

    tft.init(SCREEN_W, SCREEN_H);
    tft.setRotation(2);
    tft.fillScreen(ST77XX_BLACK);

    for (int i = 0; i < SCREEN_W; i++) history[i] = RSSI_NONE;

    drawCentered("Starting", SCREEN_H / 2 - 10, 2, ST77XX_WHITE);

    // Deliberately NOT calling WiFi.mode(WIFI_STA) here. The working
    // spotify-nowplaying firmware -- same core, same WiFiManager, same IoT
    // network -- does not, and autoConnect() manages the radio mode itself
    // (WiFi_enableSTA). Setting the mode first while Arduino's persistence is
    // still enabled can write WiFi state to NVS behind WiFiManager's back,
    // which the library explicitly guards against internally.
    WiFi.setSleep(false); // matches spotify-nowplaying
    WiFi.onEvent(onWiFiEvent); // captures the raw 802.11 disconnect reason

    // Note for anyone hitting a connection failure on a WPA/WPA2-mixed or
    // WPA-only AP: the Arduino core refuses anything advertising weaker than
    // WPA2 (WiFiSTA.cpp: _minSecurity = WIFI_AUTH_WPA2_PSK), and that rejection
    // surfaces as WL_STATION_WRONG_PASSWORD -- indistinguishable from a genuinely
    // wrong password. If that is your situation, add:
    //     WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
    // It is deliberately NOT enabled by default: it lowers the security floor,
    // and it was not what fixed the failure during development (that was
    // removing WiFi.mode(WIFI_STA) above), so it has not earned being on.

    wm.setConfigPortalBlocking(false);      // loop() must keep running
    wm.setConnectTimeout(CONNECT_TIMEOUT_S);
    wm.setAPCallback(onPortalStart);

    // Non-blocking: returns immediately. true = connected with stored
    // credentials, false = portal is now open and wm.process() must be pumped.
    if (wm.autoConnect(AP_SSID, apPassword().c_str())) {
        mode = Mode::Monitor;
        tft.fillScreen(ST77XX_BLACK);
    } else {
        mode = Mode::Setup;
        // The AP callback normally paints this, but paint it here too in case
        // the portal was already up before the callback was registered.
        drawSetupScreen();
    }

    WiFi.setAutoReconnect(true);
}

void loop() {
    wm.process();          // services the portal while it is open
    handleResetButton();

    // --- Setup mode: portal is open, report why we're not connected --------
    if (mode == Mode::Setup) {
        if (WiFi.status() == WL_CONNECTED && !wm.getConfigPortalActive()) {
            mode = Mode::Monitor;
            lastLevel = -1; lastLabel = ""; lastSSID = ""; lastRSSI = 0;
            tft.fillScreen(ST77XX_BLACK);
        } else {
            drawSetupStatus();
            delay(50);
            return;
        }
    }

    // --- Monitor mode -----------------------------------------------------
    // Deliberately no reboot-on-disconnect: this is a signal monitor, so a weak
    // or lost signal is the measurement, not a fault. Rebooting would wipe the
    // history graph exactly when it is most interesting.
    static uint32_t lastReconnectMs = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastReconnectMs > RECONNECT_EVERY_MS) {
        lastReconnectMs = millis();
        WiFi.reconnect();
    }

    int currentRSSI = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : RSSI_NONE;

    String displaySSID = WiFi.SSID();
    if (displaySSID.length() == 0) displaySSID = "Reconnecting";

    String statusLabel = "";
    uint16_t statusColor = ST77XX_WHITE;
    int level = getSignalLevel(currentRSSI, statusLabel, statusColor);

    if (level != lastLevel) {
        drawWifiIcon(SCREEN_W / 2, 10, level, statusColor);
        lastLevel = level;
    }

    if (statusLabel != lastLabel || currentRSSI != lastRSSI || displaySSID != lastSSID) {
        tft.fillRect(0, 50, SCREEN_W, 90, ST77XX_BLACK);
        drawCentered(statusLabel, 60, 2, statusColor);
        drawCentered(displaySSID, 85, 1, 0x7BEF);
        String rssiText = (currentRSSI == RSSI_NONE) ? String("-- dBm")
                                                       : (String(currentRSSI) + " dBm");
        drawCentered(rssiText, 110, 2, ST77XX_WHITE);
        lastLabel = statusLabel;
        lastRSSI = currentRSSI;
        lastSSID = displaySSID;
    }

    // --- Scrolling graph ---
    int gTop = 150;
    int gBot = 230;

    for (int i = 0; i < SCREEN_W - 1; i++) history[i] = history[i + 1];
    history[SCREEN_W - 1] = currentRSSI;

    tft.fillRect(0, gTop - 5, SCREEN_W, (gBot - gTop) + 10, ST77XX_BLACK);
    tft.drawFastHLine(0, gTop, SCREEN_W, 0x3186);
    tft.drawFastHLine(0, gBot, SCREEN_W, 0x3186);

    for (int i = 1; i < SCREEN_W; i++) {
        // Skip only where neither point has a real reading. A boundary segment
        // (one real point, one RSSI_NONE) still draws -- constrain() clamps the
        // NONE side to the bottom of the axis, which is the correct visual for
        // "signal was here, then the connection was lost."
        if (history[i] == RSSI_NONE && history[i - 1] == RSSI_NONE) continue;
        int v1 = constrain(history[i - 1], RSSI_MIN, RSSI_MAX);
        int v2 = constrain(history[i],     RSSI_MIN, RSSI_MAX);
        int y1 = map(v1, RSSI_MIN, RSSI_MAX, gBot, gTop);
        int y2 = map(v2, RSSI_MIN, RSSI_MAX, gBot, gTop);
        String dummyL; uint16_t lineCol;
        getSignalLevel(history[i], dummyL, lineCol);
        tft.drawLine(i - 1, y1, i, y2, lineCol);
    }

    delay(150);
}
