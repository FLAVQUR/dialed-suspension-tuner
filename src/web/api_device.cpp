// The API for the device itself: status, time, WiFi, starting and stopping a
// run, the debug readings, the system page. Routes are in serverInit().
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "server.h"
#include "power/battery.h"
#include "gps/gps.h"
#include "system/system.h"
#include "system/clock.h"
#include "debug/loop_profiler.h"
#include "debug/log_capture.h"
#include "connectivity/network.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "run/run_service.h"
#include "suspension/suspension.h"
#include "imu/imu.h"
#include "zip_writer.h"
#include "dialed_web_server.h"
#include "system/version.h"
#include "bikes/bikes.h"
#include "api.h"

static const char* stateToString(SystemState s) {
    switch (s) {
        case BOOTING:   return "booting";
        case READY:     return "ready";
        case RECORDING: return "recording";
        case ERROR:     return "error";
        default:        return "unknown";
    }
}

void handleApiStatus() {
    float v = readBatteryVoltage();
    float p = batteryPercent(v);

    JsonDocument doc;

    JsonObject battery = doc["battery"].to<JsonObject>();
    battery["voltage"] = serialized(String(v, 2));
    battery["percent"] = serialized(String(p, 0));

    JsonObject gps = doc["gps"].to<JsonObject>();
    gps["fix"] = gpsHasFix();
    gps["satellites"] = gpsSatellites();
    gps["hdop"] = serialized(String(gpsHdop(), 2));

    JsonObject suspension = doc["suspension"].to<JsonObject>();
    suspension["front"] = serialized(String(suspensionGetTravelMM(FRONT), 0));
    suspension["front_pct"] = serialized(String(suspensionGetTravelPct(FRONT) * 100, 0));
    suspension["front_raw"] = serialized(String(suspensionGetRaw(FRONT), 0));
    suspension["rear"] = serialized(String(suspensionGetTravelMM(REAR), 0));
    suspension["rear_pct"] = serialized(String(suspensionGetTravelPct(REAR) * 100, 0));
    suspension["rear_raw"] = serialized(String(suspensionGetRaw(REAR), 0));

    JsonObject health = doc["health"].to<JsonObject>();
    health["storage"] = components.storage;
    health["imu"] = components.imu;
    health["gps"] = components.gps;

    // IMU link quality (see ImuHealth): resets climbing while the device sits
    // still means the INT line is unreliable, not that the sensor is broken.
    ImuHealth ih = imuGetHealth();
    health["imu_streaming"] = ih.streaming;
    health["imu_resets"] = ih.resets;

    JsonObject link = doc["imu_link"].to<JsonObject>();
    if (ih.msSinceRotation == UINT32_MAX) link["ms_since_rotation"] = nullptr;
    else link["ms_since_rotation"] = ih.msSinceRotation;
    if (ih.msSinceAccel == UINT32_MAX) link["ms_since_accel"] = nullptr;
    else link["ms_since_accel"] = ih.msSinceAccel;
    link["rotation_reports"] = ih.rotationReports;
    link["accel_reports"] = ih.accelReports;
    link["rotation_enabled"] = ih.rotationEnabled;
    link["accel_enabled"] = ih.accelEnabled;
    link["longest_drain_ms"] = ih.longestDrainMs;
    link["most_events_per_drain"] = ih.mostEventsPerDrain;

    // Free heap matters here because the same pool feeds lwIP's TCP buffers:
    // when it ran low, every socket write failed with EAGAIN and the dashboard
    // simply stopped loading. min_free is the low-water mark since boot, which
    // is what a transient squeeze during a run shows up in.
    JsonObject mem = doc["memory"].to<JsonObject>();
    mem["free_heap"] = ESP.getFreeHeap();
    mem["min_free_heap"] = ESP.getMinFreeHeap();
    mem["largest_block"] = ESP.getMaxAllocHeap();

    // Whether the device knows what time it is and who told it (see
    // clock.h) — the web UI offers its own time when nobody has yet.
    JsonObject clockInfo = doc["clock"].to<JsonObject>();
    clockInfo["set"] = clockIsSet();
    clockInfo["source"] = clockSourceName();
    clockInfo["epoch"] = clockNow();

    doc["reset_reason"] = systemResetReason();
    // A start that has been asked for and is about to happen counts as
    // recording here: as far as the page that asked is concerned, it is.
    doc["state"] = RunService::startIsPending() ? "recording" : stateToString(getState());
    doc["current_run"] = RunService::currentName();
    doc["recording_seconds"] = RunService::elapsedSeconds();

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// The browser's clock, offered as a time source of last resort (see clock.h)
// — refused once GPS or NTP has set the time.
void handleApiTimeSet() {
    if (!server.hasArg("epoch")) {
        server.send(400, "text/plain", "Missing epoch");
        return;
    }

    bool accepted = clockSetFromBrowser((uint32_t)strtoul(server.arg("epoch").c_str(), nullptr, 10));

    JsonDocument doc;
    doc["accepted"] = accepted;
    doc["source"] = clockSourceName();

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiWifiScan() {
    static WifiNetwork nets[20];
    int count = wifiScan(nets, 20);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = nets[i].ssid;
        obj["rssi"] = nets[i].rssi;
        obj["secure"] = nets[i].secure;
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiWifiMode() {
    if (!server.hasArg("mode")) {
        server.send(400, "text/plain", "Missing mode");
        return;
    }

    String m = server.arg("mode");

    if (m == "sta") {
        wifiRequestSwitchToSta();
    } else if (m == "ap") {
        wifiRequestSwitchToAp();
    } else {
        server.send(400, "text/plain", "mode must be sta or ap");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiWifiStatus() {
    WifiState s = wifiGetState();

    const char* modeStr;
    switch (s) {
        case WifiState::CONNECTING_STA: modeStr = "connecting"; break;
        case WifiState::STA_CONNECTED:  modeStr = "sta";        break;
        case WifiState::AP_MODE:        modeStr = "ap";         break;
        default:                        modeStr = "off";        break;
    }

    JsonDocument doc;
    doc["mode"] = modeStr;

    if (s == WifiState::STA_CONNECTED) {
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
    } else if (s == WifiState::AP_MODE) {
        doc["ssid"] = settings.apSSID;
        doc["ip"] = WiFi.softAPIP().toString();
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// Starting a run from here is asked for now and carried out half a second
// later (RunService::startSoon()): the run begins by switching the WiFi
// radio off, and this answer has to get out through it first. Until it
// happens the device already reports itself as recording (see
// handleApiStatus()), and a second toggle simply calls the start off.
void handleApiRunToggle() {
    SystemState state = getState();

    if (RunService::startIsPending()) {
        RunService::cancelPendingStart();
        server.send(200, "text/plain", "READY");
        return;
    }

    if (state == READY) {
        RunService::startSoon();
        server.send(200, "text/plain", "RECORDING");
    }
    else if (state == RECORDING) {
        RunService::stop();
        server.send(200, "text/plain", "READY");
    }
    else {
        server.send(409, "text/plain", "Device not ready");
    }
}

// A run that stops by itself (see RunService::startTimed()) — how the device
// is measured without anyone at its button. Answers with the run's name, so
// that whoever started it can find its files afterwards.
void handleApiRunTimed() {
    int seconds = server.arg("seconds").toInt();

    if (seconds < 1 || seconds > 120) {
        server.send(400, "text/plain", "seconds must be 1-120");
        return;
    }

    if (getState() != READY || RunService::startIsPending()) {
        server.send(409, "text/plain", "Device not ready");
        return;
    }

    // Scheduled, like any start asked for over the network.
    String name = RunService::startSoon((uint32_t)seconds);

    JsonDocument doc;
    doc["run"] = name;
    doc["seconds"] = seconds;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// Where loop() spent its time: the window in progress and the last finished
// recording (see loop_profiler.h).
void handleApiDebugLoop() {
    JsonDocument doc;
    LoopProfiler::writeJson(doc.to<JsonObject>());

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// What the core and its libraries have logged lately (see log_capture.h) —
// the SD driver's own reason for a failed write, above all. ?clear=1 empties
// it after reading.
void handleApiDebugLog() {
    String text = logCaptureRead();

    if (server.arg("clear") == "1")
        logCaptureClear();

    // Never an empty body: the web server logs a warning about those, which
    // would then be the first thing in here the next time it is read.
    if (text.length() == 0)
        text = "(nothing logged)\n";

    server.send(200, "text/plain", text);
}

void handleApiSystem() {
    JsonDocument doc;

    JsonObject device = doc["device"].to<JsonObject>();
    device["chip"] = "ESP32";
    device["cores"] = 2;
    device["freq"] = getCpuFrequencyMhz();
    device["firmware_version"] = FIRMWARE_VERSION;
    device["firmware_build"] = FIRMWARE_BUILD;

    JsonObject memory = doc["memory"].to<JsonObject>();
    memory["heap_total"] = ESP.getHeapSize();
    memory["heap_used"] = ESP.getHeapSize() - ESP.getFreeHeap();
    memory["heap_min_free"] = ESP.getMinFreeHeap();

    doc["uptime_s"] = millis() / 1000;

    JsonObject storage = doc["storage"].to<JsonObject>();
    storage["type"] = SD.cardType();
    storage["total_mb"] = SD.totalBytes() / (1024 * 1024);
    storage["used_mb"] = SD.usedBytes() / (1024 * 1024);

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}
