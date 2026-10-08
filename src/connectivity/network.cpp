#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "network.h"
#include "system/system.h"
#include "system/settings.h"
#include "system/clock.h"

static WifiMode mode = WifiMode::NET_STA;
static WifiState state = WifiState::IDLE;
static unsigned long connectStart = 0;
static constexpr uint32_t STA_TIMEOUT_MS = 20000;

static bool modeSwitchPending = false;
static WifiMode pendingSwitchMode = WifiMode::NET_STA;
static uint32_t modeSwitchAtMs = 0;
constexpr uint32_t MODE_SWITCH_DELAY_MS = 500;

// Remembers whether the device last settled into station or AP mode, so
// the next boot can skip straight to AP instead of burning the full 20s
// STA timeout again when it's likely still out of range of the configured
// home network. Its own tiny NVS namespace, separate from the main
// settings — this changes on its own every time the mode settles, not
// from a Settings save.
static Preferences netPrefs;
static constexpr uint8_t LAST_MODE_STA = 1;
static constexpr uint8_t LAST_MODE_AP  = 2;

static void persistLastMode(uint8_t m) {
    netPrefs.begin("net", false);
    netPrefs.putUChar("lastMode", m);
    netPrefs.end();
}

// (Re)starts mDNS under the fixed "dialedtuner" hostname. Used for both STA
// and AP mode so the device is always reachable at dialedtuner.local.
static void startMdns() {
    MDNS.end();

    if (MDNS.begin("dialedtuner")) {
        Serial.println("mDNS started");
    } else {
        Serial.println("mDNS failed to start");
    }
}

void wifiInit() {
    Serial.println("Connecting to WiFi... ");

    netPrefs.begin("net", true);
    uint8_t lastMode = netPrefs.getUChar("lastMode", LAST_MODE_STA);
    netPrefs.end();

    // Note: if the rider gets back in range of their home network, this
    // won't rediscover it on its own next boot — the device will keep
    // booting straight to AP until "Switch to WiFi Now" is tapped once
    // from Settings. That's the deliberate trade for not wasting 20s on
    // every boot away from home.
    if (lastMode == LAST_MODE_AP) {
        wifiSetMode(WifiMode::NET_AP);
    } else {
        wifiSetMode(WifiMode::NET_STA);
    }
}

void wifiUpdate() {
    const uint32_t now = millis();

    if (modeSwitchPending && now >= modeSwitchAtMs) {
        modeSwitchPending = false;
        wifiSetMode(pendingSwitchMode);
        return;
    }

    switch (state)
    {
        // =========================
        // STA CONNECTING
        // =========================
        case WifiState::CONNECTING_STA:
        {
            wl_status_t status = WiFi.status();

            if (status == WL_CONNECTED)
            {
                state = WifiState::STA_CONNECTED;
                components.wifi = true;
                persistLastMode(LAST_MODE_STA);

                startMdns();

                // On the home network there may be internet — ask for the
                // time (see clock.h). Harmless if there isn't.
                clockStartNtp();

                Serial.println("WiFi STA Ready");
                Serial.print("IP: ");
                Serial.println(WiFi.localIP());

                return;
            }

            // controlled timeout
            if (now - connectStart > STA_TIMEOUT_MS)
            {
                Serial.println("STA timeout → switching to AP");

                // wifiSetMode(NET_AP) already does its own MDNS.end() /
                // WiFi.mode() transition below — calling
                // WiFi.disconnect(true) here first (an aggressive
                // disconnect-and-erase-config) immediately before that mode
                // switch is a known ESP32 WiFi driver race: esp_wifi_deinit()
                // can run before esp_wifi_stop() has actually finished,
                // which is exactly what "wifi not stop" / 0x3003 in the
                // serial log means, and it left the driver's internal state
                // torn down enough that the next WiFi call (softAP) crashed
                // with LoadProhibited. Just switching modes directly avoids
                // the race entirely.
                wifiSetMode(WifiMode::NET_AP);
            }

            break;
        }

        // =========================
        // STA CONNECTED
        // =========================
        case WifiState::STA_CONNECTED:
        {
            components.wifi = true;

            // sanity check (automatic recovery)
            if (WiFi.status() != WL_CONNECTED)
            {
                Serial.println("WiFi lost → reconnecting");

                wifiSetMode(WifiMode::NET_STA);
            }

            break;
        }

        // =========================
        // AP MODE
        // =========================
        case WifiState::AP_MODE:
        {
            components.wifi = true;
            break;
        }

        // =========================
        // OFF / FAIL / IDLE
        // =========================
        case WifiState::OFF:
        case WifiState::FAIL:
        case WifiState::IDLE:
        default:
        {
            components.wifi = false;
            break;
        }
    }
}

void wifiSetMode(WifiMode newMode) {
    mode = newMode;

    switch (newMode) {
        case WifiMode::NET_OFF:
            MDNS.end();

            // WiFi.mode(WIFI_MODE_NULL) already stops and tears down the
            // driver on its own. A preceding WiFi.disconnect(true) here hit
            // the exact same "wifi not stop" / esp_wifi_deinit race fixed
            // in the STA-timeout path above — except this one ran on every
            // single recording start (whenever wifiOffWhileRecording turns
            // the radio off), not just a rare STA timeout, which is almost
            // certainly what was making the record button seem so
            // inconsistent — the device was likely crash-rebooting ~500ms
            // into most recordings.
            WiFi.mode(WIFI_MODE_NULL);

            state = WifiState::OFF;
            break;

        case WifiMode::NET_STA:
            // No home network configured — go straight to the AP fallback
            // instead of waiting out the full STA timeout for nothing.
            if (!settings.staEnabled || settings.staSSID.length() == 0) {
                wifiSetMode(WifiMode::NET_AP);
                return;
            }

            WiFi.mode(WIFI_MODE_STA);

            // Modem sleep is on by default and lets the radio doze between
            // packets to save power — fine for idle polling, but it adds
            // tens of ms of latency to every packet once something's
            // actually transferring data, which is most of why a multi-MB
            // run CSV download over the home network was still crawling
            // even after chunk size and Nagle's algorithm were fixed in
            // server.cpp/zip_writer.cpp. AP mode already disables this
            // (below) — STA mode never did.
            WiFi.setSleep(false);

            WiFi.begin(settings.staSSID.c_str(), settings.staPassword.c_str());

            connectStart = millis();

            state = WifiState::CONNECTING_STA;
            break;

        case WifiMode::NET_AP:
            MDNS.end();

            WiFi.mode(WIFI_MODE_AP);
            WiFi.setSleep(false);

            // Explicit, deterministic AP IP/gateway so it's always the same
            // address to type in (192.168.4.1) regardless of what the SDK's
            // default would otherwise pick.
            {
                IPAddress apIP(192, 168, 4, 1);
                WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            }

            WiFi.softAP(settings.apSSID.c_str(), settings.apPassword.c_str());

            // Same hostname as in STA mode, so http://dialedtuner.local works
            // regardless of which network the phone is on. Must come after
            // softAP() so the AP interface is already up for mDNS to bind to.
            startMdns();

            state = WifiState::AP_MODE;
            persistLastMode(LAST_MODE_AP);
            break;
    }
}

WifiMode wifiGetMode() {
    return mode;
}

WifiState wifiGetState() {
    return state;
}

void wifiApplyConnectivitySettings() {
    // Re-enter whichever mode is currently active so a settings-page
    // credential change (AP name/password, or station SSID/password) takes
    // effect immediately rather than waiting for a reboot.
    if (state == WifiState::AP_MODE) {
        wifiSetMode(WifiMode::NET_AP);
    } else if (state != WifiState::OFF) {
        wifiSetMode(WifiMode::NET_STA);
    }
}

static void requestModeSwitch(WifiMode m) {
    pendingSwitchMode = m;
    modeSwitchPending = true;
    modeSwitchAtMs = millis() + MODE_SWITCH_DELAY_MS;
}

void wifiRequestSwitchToSta() {
    requestModeSwitch(WifiMode::NET_STA);
}

void wifiRequestSwitchToAp() {
    requestModeSwitch(WifiMode::NET_AP);
}

void wifiResetCredentials() {
    Serial.println("Connectivity reset to defaults — restarting");

    settingsResetConnectivity();

    delay(300);
    ESP.restart();
}

int wifiScan(WifiNetwork* out, int maxResults) {
    bool wasApOnly = (mode == WifiMode::NET_AP);

    // AP-only mode can't scan on its own — briefly add STA capability.
    if (wasApOnly) WiFi.mode(WIFI_MODE_APSTA);

    int found = WiFi.scanNetworks();
    int count = 0;

    for (int i = 0; i < found && count < maxResults; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;

        // Dedupe repeated SSIDs (e.g. mesh access points), keeping the
        // strongest signal seen.
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (out[j].ssid == ssid) {
                dup = true;
                if (WiFi.RSSI(i) > out[j].rssi) {
                    out[j].rssi = WiFi.RSSI(i);
                    out[j].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
                }
                break;
            }
        }

        if (!dup) {
            out[count].ssid = ssid;
            out[count].rssi = WiFi.RSSI(i);
            out[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            count++;
        }
    }

    WiFi.scanDelete();
    if (wasApOnly) WiFi.mode(WIFI_MODE_AP);

    // Simple insertion sort by signal strength — count is small (<= maxResults).
    for (int i = 1; i < count; i++) {
        WifiNetwork key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    return count;
}