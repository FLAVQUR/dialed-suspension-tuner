#include <esp_system.h>
#include "system.h"
#include "connectivity/network.h"
#include "system/settings.h"

static SystemState state = BOOTING;

// Whether the WiFi radio was taken down for the run in progress (see
// recordingPrepare()), and so has to come back when that run ends. Tracked
// rather than read from the setting, which can change while a run is on.
static bool wifiDownForRun = false;

ComponentStatus components;

SystemState getState() {
    return state;
}

void systemInit() {
    state = BOOTING;
}

const char* systemResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

void systemUpdate() {
    if (state == BOOTING) {
        if (allComponentsReady()) {
            state = READY;
        }
    }

    if (!components.storage || !components.imu) {
        state = ERROR;
    }
}

void recordingPrepare() {
    if (!settings.wifiOffWhileRecording || wifiGetMode() == WifiMode::NET_OFF)
        return;

    wifiSetMode(WifiMode::NET_OFF);
    wifiDownForRun = true;
}

void recordingRelease() {
    if (!wifiDownForRun)
        return;

    wifiDownForRun = false;
    wifiSetMode(WifiMode::NET_STA);
}

bool allComponentsReady() {
    // GPS is deliberately not required here — it's a nice-to-have for
    // route/speed/altitude data, not something a run needs to be
    // recordable at all (unlike storage, without which there's nowhere
    // to write to, or the IMU, a core telemetry sensor). A missing or
    // still-acquiring GPS module should never block reaching READY.
    return
        components.storage &&
        components.imu &&
        components.wifi;
}

void toggleRecording() {
    if (state == READY) {
        state = RECORDING;

        Serial.println("RECORDING");
    }
    else if (state == RECORDING) {
        state = READY;

        // The radio comes back, if it was taken down for this run.
        recordingRelease();

        Serial.println("READY");
    }
    else {
        Serial.println("INVALID STATE CHANGE");
    }
}