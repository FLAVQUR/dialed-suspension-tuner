#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "network.h"
#include "config/secrets.h"
#include "system/system.h"

void otaInit() {
    ArduinoOTA.setHostname("dialedtuner");
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        Serial.println("OTA START");
        // A flash write during OTA can occasionally take longer than the
        // watchdog timeout; without this, the watchdog could restart the
        // device mid-flash and corrupt the update. Re-armed in onEnd/onError.
        esp_task_wdt_delete(NULL);
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("OTA END");
        esp_task_wdt_add(NULL);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA ERROR[%u]\n", error);
        esp_task_wdt_add(NULL);
    });

    ArduinoOTA.begin();

    Serial.println("OTA READY");
}

void otaUpdate() {
    if(wifiGetMode() != WifiMode::NET_OFF) {
        ArduinoOTA.handle();
    }
}