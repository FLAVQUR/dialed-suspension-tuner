#pragma once

#include <Arduino.h>

enum WifiMode {
    NET_OFF,
    NET_STA,
    NET_AP
};

enum class WifiState {
    IDLE,
    CONNECTING_STA,
    STA_CONNECTED,
    AP_MODE,
    OFF,
    FAIL
};

void wifiInit();
void wifiUpdate();

void wifiSetMode(WifiMode mode);
WifiMode wifiGetMode();
WifiState wifiGetState();

// Re-applies the AP/station settings to whichever mode is currently active,
// so a settings-page credential change takes effect immediately instead of
// requiring a reboot.
void wifiApplyConnectivitySettings();

// Resets connectivity settings to their firmware defaults and restarts the
// device — the emergency escape hatch for the button long-press gesture
// when a rider forgets a custom AP/WiFi password.
void wifiResetCredentials();

// Requests switching to station (home WiFi) or Access Point mode a short
// moment from now, rather than immediately — used by the connection-mode
// dropdown in Settings, so the HTTP response confirming the request has
// time to leave the device first if the browser is currently connected
// through the mode being switched away from.
void wifiRequestSwitchToSta();
void wifiRequestSwitchToAp();

struct WifiNetwork {
    String ssid;
    int8_t rssi;
    bool secure;
};

// Scans for nearby WiFi networks (blocking, ~2-4s). Returns the number of
// networks written into `out` (deduped by SSID, strongest signal first),
// up to `maxResults`.
int wifiScan(WifiNetwork* out, int maxResults);