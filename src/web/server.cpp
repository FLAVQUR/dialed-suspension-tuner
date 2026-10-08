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
#include "system/loop_profiler.h"
#include "system/log_capture.h"
#include "connectivity/network.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "run/run_service.h"
#include "suspension/suspension.h"
#include "imu/imu.h"
#include "zip_writer.h"
#include "system/version.h"
#include "bikes/bikes.h"

WebServer server(80);

static const char* getMimeType(const String& filename) {
    if (filename.endsWith(".CSV")) return "text/csv";
    if (filename.endsWith(".JSON")) return "application/json";
    return "application/octet-stream";
}

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

void handleApiConfigGet() {
    JsonDocument doc;
    doc["bikeName"] = settings.bikeName;
    doc["fullSuspension"] = settings.fullSuspension;
    doc["wifiOffWhileRecording"] = settings.wifiOffWhileRecording;
    doc["theme"] = settings.theme;
    doc["suspensionSampleRateHz"] = settings.suspensionSampleRateHz;
    doc["trendsRunCount"] = settings.trendsRunCount;
    doc["wheelSize"] = settings.wheelSize;
    doc["rearWheelTravelMM"] = serialized(String(settings.rearWheelTravelMM, 0));

    // Passwords are write-only — never sent back to the browser.
    doc["apSSID"] = settings.apSSID;
    doc["staEnabled"] = settings.staEnabled;
    doc["staSSID"] = settings.staSSID;

    doc["frontLSC"] = settings.frontLSC;
    doc["frontHSC"] = settings.frontHSC;
    doc["frontLSR"] = settings.frontLSR;
    doc["frontHSR"] = settings.frontHSR;
    doc["frontVolumeTokens"] = settings.frontVolumeTokens;
    doc["frontPressurePsi"] = serialized(String(settings.frontPressurePsi, 0));
    doc["frontSagPct"] = serialized(String(settings.frontSagPct, 1));
    doc["frontHasLSC"] = settings.frontHasLSC;
    doc["frontHasHSC"] = settings.frontHasHSC;
    doc["frontHasLSR"] = settings.frontHasLSR;
    doc["frontHasHSR"] = settings.frontHasHSR;

    doc["rearLSC"] = settings.rearLSC;
    doc["rearHSC"] = settings.rearHSC;
    doc["rearLSR"] = settings.rearLSR;
    doc["rearHSR"] = settings.rearHSR;
    doc["rearVolumeTokens"] = settings.rearVolumeTokens;
    doc["rearPressurePsi"] = serialized(String(settings.rearPressurePsi, 0));
    doc["rearSagPct"] = serialized(String(settings.rearSagPct, 1));
    doc["rearHasLSC"] = settings.rearHasLSC;
    doc["rearHasHSC"] = settings.rearHasHSC;
    doc["rearHasLSR"] = settings.rearHasLSR;
    doc["rearHasHSR"] = settings.rearHasHSR;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiConfigPost() {
    bool connectivityChanged =
        server.hasArg("apSSID") || server.hasArg("apPassword") ||
        server.hasArg("staEnabled") || server.hasArg("staSSID") || server.hasArg("staPassword");

    if (server.hasArg("bikeName")) {
        String name = server.arg("bikeName");

        if (name.length() > MAX_BIKE_NAME_LEN)
            name = name.substring(0, MAX_BIKE_NAME_LEN);

        // A bike that has a saved profile is that profile, and its name is
        // what the profile is filed under — so a new name renames the
        // profile, rather than leaving it behind under the old one with
        // nothing keeping it up to date any more.
        if (name != settings.bikeName && BikeProfiles::exists(settings.bikeName)) {
            if (name.length() == 0) {
                server.send(409, "text/plain", "A saved bike needs a name");
                return;
            }

            if (BikeProfiles::nameTaken(name, settings.bikeName)) {
                server.send(409, "text/plain", "Another bike profile already has that name");
                return;
            }

            if (!BikeProfiles::rename(settings.bikeName, name)) {
                server.send(500, "text/plain", "Could not rename the bike profile");
                return;
            }
        }

        settings.bikeName = name;
    }

    if (server.hasArg("fullSuspension"))
        settings.fullSuspension = server.arg("fullSuspension") == "1";

    if (server.hasArg("wifiOffWhileRecording"))
        settings.wifiOffWhileRecording = server.arg("wifiOffWhileRecording") == "1";

    if (server.hasArg("theme")) {
        String theme = server.arg("theme");

        if (theme == "dark" || theme == "light") {
            settings.theme = theme;
        } else {
            server.send(400, "text/plain", "theme must be dark or light");
            return;
        }
    }

    if (server.hasArg("suspensionSampleRateHz")) {
        int hz = server.arg("suspensionSampleRateHz").toInt();

        if (hz == 500 || hz == 1000) {
            settings.suspensionSampleRateHz = hz;
        } else {
            server.send(400, "text/plain", "suspensionSampleRateHz must be 500 or 1000");
            return;
        }
    }

    if (server.hasArg("trendsRunCount")) {
        int n = server.arg("trendsRunCount").toInt();

        if (n < 5 || n > 100) {
            server.send(400, "text/plain", "trendsRunCount must be 5-100");
            return;
        }

        settings.trendsRunCount = n;
    }

    if (server.hasArg("wheelSize"))
        settings.wheelSize = server.arg("wheelSize");

    if (server.hasArg("rearWheelTravelMM"))
        settings.rearWheelTravelMM = server.arg("rearWheelTravelMM").toFloat();

    if (server.hasArg("frontLSC"))
        settings.frontLSC = server.arg("frontLSC").toInt();

    if (server.hasArg("frontHSC"))
        settings.frontHSC = server.arg("frontHSC").toInt();

    if (server.hasArg("frontLSR"))
        settings.frontLSR = server.arg("frontLSR").toInt();

    if (server.hasArg("frontHSR"))
        settings.frontHSR = server.arg("frontHSR").toInt();

    if (server.hasArg("frontVolumeTokens"))
        settings.frontVolumeTokens = server.arg("frontVolumeTokens").toInt();

    if (server.hasArg("frontPressurePsi"))
        settings.frontPressurePsi = server.arg("frontPressurePsi").toFloat();

    if (server.hasArg("frontHasLSC"))
        settings.frontHasLSC = server.arg("frontHasLSC") == "1";

    if (server.hasArg("frontHasHSC"))
        settings.frontHasHSC = server.arg("frontHasHSC") == "1";

    if (server.hasArg("frontHasLSR"))
        settings.frontHasLSR = server.arg("frontHasLSR") == "1";

    if (server.hasArg("frontHasHSR"))
        settings.frontHasHSR = server.arg("frontHasHSR") == "1";

    if (server.hasArg("rearLSC"))
        settings.rearLSC = server.arg("rearLSC").toInt();

    if (server.hasArg("rearHSC"))
        settings.rearHSC = server.arg("rearHSC").toInt();

    if (server.hasArg("rearLSR"))
        settings.rearLSR = server.arg("rearLSR").toInt();

    if (server.hasArg("rearHSR"))
        settings.rearHSR = server.arg("rearHSR").toInt();

    if (server.hasArg("rearVolumeTokens"))
        settings.rearVolumeTokens = server.arg("rearVolumeTokens").toInt();

    if (server.hasArg("rearPressurePsi"))
        settings.rearPressurePsi = server.arg("rearPressurePsi").toFloat();

    if (server.hasArg("rearHasLSC"))
        settings.rearHasLSC = server.arg("rearHasLSC") == "1";

    if (server.hasArg("rearHasHSC"))
        settings.rearHasHSC = server.arg("rearHasHSC") == "1";

    if (server.hasArg("rearHasLSR"))
        settings.rearHasLSR = server.arg("rearHasLSR") == "1";

    if (server.hasArg("rearHasHSR"))
        settings.rearHasHSR = server.arg("rearHasHSR") == "1";

    if (server.hasArg("apSSID")) {
        String ssid = server.arg("apSSID");

        if (ssid.length() == 0 || ssid.length() > 32) {
            server.send(400, "text/plain", "apSSID must be 1-32 characters");
            return;
        }
        settings.apSSID = ssid;
    }

    // Blank means "keep current" — the password field is never pre-filled.
    if (server.hasArg("apPassword") && server.arg("apPassword").length() > 0) {
        String pass = server.arg("apPassword");

        if (pass.length() < 8 || pass.length() > 63) {
            server.send(400, "text/plain", "apPassword must be 8-63 characters");
            return;
        }
        settings.apPassword = pass;
    }

    if (server.hasArg("staEnabled"))
        settings.staEnabled = server.arg("staEnabled") == "1";

    if (server.hasArg("staSSID"))
        settings.staSSID = server.arg("staSSID");

    if (server.hasArg("staPassword") && server.arg("staPassword").length() > 0) {
        String pass = server.arg("staPassword");

        if (pass.length() < 8 || pass.length() > 63) {
            server.send(400, "text/plain", "staPassword must be 8-63 characters");
            return;
        }
        settings.staPassword = pass;
    }

    settingsSave();

    // The active bike's profile on the card follows every edit (see
    // bikes.h). Not only when a bike field was among the arguments: telling
    // those apart isn't worth a list that has to be kept in step with the
    // one above, for a few hundred bytes written.
    BikeProfiles::syncActive();

    if (connectivityChanged)
        wifiApplyConnectivitySettings();

    server.send(200, "text/plain", "OK");
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

void handleApiRuns() {
    auto runs = RunService::list();

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& run : runs)
    {
        JsonObject obj = arr.add<JsonObject>();

        obj["name"] = run.name;
        obj["bike_name"] = run.bikeName;
        obj["size_kb"] = serialized(String(run.sizeBytes / 1024.0f, 1));
        obj["duration_s"] = serialized(String(run.duration_s, 1));
        obj["front_travel"] = serialized(String(run.frontTravelMM, 0));
        obj["rear_travel"] = serialized(String(run.rearTravelMM, 0));
        obj["full_suspension"] = run.fullSuspension;
        obj["recovered"] = run.recovered;
        obj["incomplete"] = run.incomplete;
        obj["no_gps"] = run.noGpsFix;
        obj["rows_dropped"] = run.rowsDropped;
        obj["notes"] = run.notes;
        obj["title"] = run.title;

        if (run.startUtc > 0)
            obj["start_utc"] = run.startUtc;

        if (run.hasStats) {
            obj["distance_km"] = serialized(String(run.distanceKm, 2));
            obj["max_speed_kmh"] = serialized(String(run.maxSpeedKmh, 1));
            obj["max_front_mm"] = serialized(String(run.maxFrontMM, 1));
            obj["max_rear_mm"] = serialized(String(run.maxRearMM, 1));
        }
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// Shared by handleApiRunDownload/handleApiRunDownloadDebug/
// handleApiNetworkDebug below — one request is handled at a time on this
// single-threaded server, so there's no concurrency risk, and a separate
// 16KB buffer per handler was enough to overflow the ESP32's DRAM budget
// (only one of these three ever runs at once anyway).
constexpr size_t DOWNLOAD_CHUNK_SIZE = 16384;
static uint8_t downloadBuf[DOWNLOAD_CHUNK_SIZE];

void handleApiRunDownload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing file");
        return;
    }

    String filename = server.arg("file");

    if (!filename.startsWith("/"))
        filename = "/" + filename;

    File file = storageGet(filename.c_str());

    if (!file) {
        server.send(404, "text/plain", "File not found");
        return;
    }

    String downloadName = filename.startsWith("/") ? filename.substring(1) : filename;

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
    server.setContentLength(file.size());
    server.send(200, getMimeType(filename), "");

    // Nagle's algorithm holds a small write back for up to ~40ms waiting
    // to see if more data is coming, hoping to coalesce it with the next
    // write into one packet — the exact opposite of useful here, since
    // the next chunk is already sitting in `buf` a few instructions later.
    // With the old 2KB chunk size, a multi-MB run CSV meant thousands of
    // chunks each eating that stall — the actual reason downloads were
    // taking 30-40s, not raw SD/WiFi throughput (SD is already at 20MHz
    // SPI, see storage.cpp).
    server.client().setNoDelay(true);

    // Deliberately not server.streamFile(): it blocks until the whole
    // transfer is done with no way for us to pet the watchdog in
    // between, and a multi-MB run CSV over WiFi — especially through
    // this device's own AP with a marginal signal, exactly the situation
    // right after finishing a ride — can easily take longer than the 8s
    // watchdog timeout (see main.cpp), rebooting the device mid-download
    // and killing the connection. This does the same transfer manually,
    // chunk by chunk, so the loop below can reset the watchdog as it goes.
    //
    // 16KB (not a stack array) — this handler runs on the main loop()
    // task, whose default stack is small enough that the old 2KB buffer
    // was already a meaningful slice of it; the shared static buffer above
    // moves this off the stack entirely, into .bss, so it can be sized for
    // fewer read/send round trips (the real lever on download time)
    // without touching stack headroom at all.
    int n;

    while ((n = file.read(downloadBuf, sizeof(downloadBuf))) > 0) {
        server.sendContent((const char*)downloadBuf, n);
        esp_task_wdt_reset();
    }

    file.close();
}

// Serves a run's compact binary telemetry (see writeCompactCsv() in
// run_service.cpp for the format and why it exists) instead of the raw
// .CSV — run.html/compare.html/trends.html's own charts only ever needed
// 12 of the .CSV's 17 columns anyway, and re-encoding those as scaled
// fixed-point integers instead of ASCII decimal text cuts what actually
// has to cross WiFi (the real bottleneck a download-speed investigation
// found — not SD, not chunk size/Nagle's algorithm, all already fixed
// above) by roughly 4x. ensureCompactCsv() generates the file on first
// request for any run that predates this feature, so this always has
// something to serve.
void handleApiRunTelemetry() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    if (!RunService::ensureCompactCsv(name)) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    File file = storageGet(("/" + name + ".CSVB").c_str());

    if (!file) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    server.setContentLength(file.size());
    server.send(200, "application/octet-stream", "");
    server.client().setNoDelay(true);

    int n;

    while ((n = file.read(downloadBuf, sizeof(downloadBuf))) > 0) {
        server.sendContent((const char*)downloadBuf, n);
        esp_task_wdt_reset();
    }

    file.close();
}

// Bundles a run's .CSV and .JSON into a single .zip so the rider only
// needs one download instead of two — see zip_writer.h for how the
// archive itself gets built.
void handleApiRunDownloadZip() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    std::vector<ZipEntry> entries = {
        { "/" + name + ".CSV", name + ".CSV" },
        { "/" + name + ".JSON", name + ".JSON" }
    };

    sendZipResponse(server, entries, name + ".zip");
}

void handleApiRunDelete() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err || !doc["name"].is<const char*>()) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String name = doc["name"].as<String>();

    bool ok = RunService::remove(name);

    if (!ok) {
        server.send(500, "text/plain", "Delete failed");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiRunNotes() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err || !doc["name"].is<const char*>()) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String name = doc["name"].as<String>();

    // Two rider-editable fields share this endpoint, and only the ones
    // actually sent are touched — renaming a run must not blank its notes.
    bool hasNotes = doc["notes"].is<const char*>();
    bool hasTitle = doc["title"].is<const char*>();

    if (!hasNotes && !hasTitle) {
        server.send(400, "text/plain", "Nothing to update");
        return;
    }

    bool ok = true;
    if (hasNotes) ok = RunService::setNotes(name, doc["notes"].as<String>()) && ok;
    if (hasTitle) ok = RunService::setTitle(name, doc["title"].as<String>()) && ok;

    if (!ok) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiRunsClear() {
    bool ok = RunService::removeAll();

    if (!ok) {
        server.send(500, "text/plain", "Delete failed");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiSuspensionCaptureZero() {
    if (!server.hasArg("side")) {
        server.send(400, "text/plain", "Missing side");
        return;
    }

    String sideArg = server.arg("side");
    SuspensionSide side = (sideArg == "rear") ? REAR : FRONT;

    suspensionCaptureZero(side);
    server.send(200, "text/plain", "OK");
}

void handleApiSuspensionCaptureMax() {
    if (!server.hasArg("side") || !server.hasArg("mm")) {
        server.send(400, "text/plain", "Missing side or mm");
        return;
    }

    String sideArg = server.arg("side");
    SuspensionSide side = (sideArg == "rear") ? REAR : FRONT;
    float mm = server.arg("mm").toFloat();

    if (!suspensionCaptureMax(side, mm)) {
        server.send(400, "text/plain", "mm must be > 0");
        return;
    }

    BikeProfiles::syncActive();     // the travel entered with it is part of the bike's profile

    server.send(200, "text/plain", "OK");
}

void handleApiSuspensionSetTravel() {
    if (!server.hasArg("side") || !server.hasArg("mm")) {
        server.send(400, "text/plain", "Missing side or mm");
        return;
    }

    String sideArg = server.arg("side");
    SuspensionSide side = (sideArg == "rear") ? REAR : FRONT;
    float mm = server.arg("mm").toFloat();

    if (!suspensionSetTravelMM(side, mm)) {
        server.send(400, "text/plain", "mm must be > 0");
        return;
    }

    BikeProfiles::syncActive();     // travel is part of the bike's profile

    server.send(200, "text/plain", "OK");
}

// Static sag is a physical measurement (rider sitting still on the bike),
// not a value worth typing in by hand — it goes stale the moment pressure
// or tokens change, so capturing it live from the sensor is both easier
// and more trustworthy than a manually-entered guess. Sitting on the bike
// loads both ends at once, so this captures whichever side(s) are
// calibrated in a single action rather than making the rider get on and
// off the bike twice — front, then rear (skipped on a hardtail), each
// only if that side already has a real zero/max calibration.
void handleApiSuspensionCaptureSag() {
    JsonDocument doc;
    bool anyCaptured = false;

    if (suspensionGetCalibration(FRONT).valid) {
        settings.frontSagPct = suspensionGetTravelPct(FRONT) * 100.0f;
        doc["frontSagPct"] = serialized(String(settings.frontSagPct, 1));
        anyCaptured = true;
    }

    if (settings.fullSuspension && suspensionGetCalibration(REAR).valid) {
        settings.rearSagPct = suspensionGetTravelPct(REAR) * 100.0f;
        doc["rearSagPct"] = serialized(String(settings.rearSagPct, 1));
        anyCaptured = true;
    }

    if (!anyCaptured) {
        server.send(400, "text/plain", "Calibrate zero/max first");
        return;
    }

    settingsSave();
    BikeProfiles::syncActive();     // static sag is part of the bike's profile

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiSuspensionReset() {
    suspensionResetCalibration();
    server.send(200, "text/plain", "OK");
}

void handleApiBikesList() {
    JsonDocument doc;

    JsonArray arr = doc["bikes"].to<JsonArray>();
    for (const auto& name : BikeProfiles::list())
        arr.add(name);

    doc["active"] = settings.bikeName;

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiBikeActivate() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    bool ok = BikeProfiles::activate(server.arg("name"));
    server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Profile not found");
}

void handleApiBikeSave() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    if (name.length() > MAX_BIKE_NAME_LEN)
        name = name.substring(0, MAX_BIKE_NAME_LEN);

    bool ok = BikeProfiles::save(name);

    // "Save As": the new profile becomes the active bike. What is edited
    // next then goes into it, and the profile it was started from stays as
    // it was — the other way round, a copy saved to try something different
    // would be the one left untouched while the original changed.
    if (ok && settings.bikeName != name) {
        settings.bikeName = name;
        settingsSave();
    }

    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Failed to save");
}

// One saved profile, as it is on the card.
void handleApiBikeProfile() {
    String json = BikeProfiles::read(server.arg("name"));

    if (json.length() == 0) {
        server.send(404, "text/plain", "Profile not found");
        return;
    }

    server.send(200, "application/json", json);
}

void handleApiBikeDelete() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    // The active bike's own settings ARE the live settings — deleting
    // its saved copy while it's still active would just leave a stale
    // file waiting to reappear next time someone (re)creates a profile
    // with the same name, so it's simpler to just disallow this outright.
    if (name == settings.bikeName) {
        server.send(400, "text/plain", "Cannot delete the active bike");
        return;
    }

    bool ok = BikeProfiles::remove(name);
    server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Profile not found");
}

void handleApiBikeRename() {
    if (!server.hasArg("oldName") || !server.hasArg("newName")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    if (BikeProfiles::nameTaken(server.arg("newName"), server.arg("oldName"))) {
        server.send(409, "text/plain", "Another bike profile already has that name");
        return;
    }

    bool ok = BikeProfiles::rename(server.arg("oldName"), server.arg("newName"));
    server.send(ok ? 200 : 404, "text/plain", ok ? "OK" : "Profile not found");
}

static void writeCalibrationJson(JsonObject& obj, SuspensionSide side, const SuspensionCalibration& cal) {
    obj["valid"] = cal.valid;
    obj["zeroSet"] = cal.zeroSet;
    obj["maxSet"] = cal.maxSet;
    obj["zeroRaw"] = cal.zeroRaw;
    obj["maxRaw"] = cal.maxRaw;
    obj["maxMM"] = serialized(String(suspensionGetMaxTravelMM(side), 1));
}

void handleApiSuspensionCalibrationGet() {
    auto f = suspensionGetCalibration(FRONT);
    auto r = suspensionGetCalibration(REAR);

    JsonDocument doc;

    JsonObject front = doc["front"].to<JsonObject>();
    writeCalibrationJson(front, FRONT, f);

    JsonObject rear = doc["rear"].to<JsonObject>();
    writeCalibrationJson(rear, REAR, r);

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

static bool imuIsZeroed() {
    float mag = sqrtf(settings.imuUpX * settings.imuUpX +
                      settings.imuUpY * settings.imuUpY +
                      settings.imuUpZ * settings.imuUpZ);
    return mag >= 0.5f;
}

// Live IMU readout for the Settings page's "IMU Orientation" card: the raw
// sensor angles always, and — once zeroed — the bike-frame lean/pitch
// computed on the device (imuBikeAngles(), the same maths run.html applies to
// logged data), so a rider can zero the IMU, tilt the bike and watch the
// numbers respond without recording a run.
void handleApiImuGet() {
    JsonDocument doc;

    doc["pitchRad"] = serialized(String(imuPitch(), 3));
    doc["rollRad"] = serialized(String(imuRoll(), 3));

    bool zeroed = imuIsZeroed();
    doc["zeroed"] = zeroed;

    if (zeroed) {
        const float up[3] = { settings.imuUpX, settings.imuUpY, settings.imuUpZ };

        JsonArray arr = doc["up"].to<JsonArray>();
        for (int i = 0; i < 3; i++)
            arr.add(serialized(String(up[i], 5)));

        doc["mountTiltDeg"] = serialized(String(asinf(up[0] / sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2])) * 57.29578f, 1));

        float lean = 0, pitchUp = 0;
        if (imuBikeAngles(imuPitch(), imuRoll(), up, lean, pitchUp)) {
            doc["leanDeg"] = serialized(String(lean, 1));
            doc["pitchUpDeg"] = serialized(String(pitchUp, 1));
        }
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// Blocks the web server for ~1.5s while it averages the rest orientation
// (single-threaded WebServer — fine for a one-off button press, and
// imuMeasureRest() keeps the watchdog fed). Refused while recording: the
// run's own imu_up reference must not change under it, and the loop() time
// belongs to sampling.
void handleApiImuZero() {
    if (getState() == RECORDING) {
        server.send(409, "text/plain", "Stop the recording first");
        return;
    }

    ImuRestReading r = imuMeasureRest(1500);

    if (!r.ok) {
        server.send(400, "text/plain", r.error);
        return;
    }

    settings.imuUpX = r.up[0];
    settings.imuUpY = r.up[1];
    settings.imuUpZ = r.up[2];
    settingsSave();

    JsonDocument doc;
    doc["mountTiltDeg"] = serialized(String(r.mountTiltDeg, 1));
    doc["wobbleDeg"] = serialized(String(r.wobbleDeg, 2));

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiImuClear() {
    settings.imuUpX = 0.0f;
    settings.imuUpY = 0.0f;
    settings.imuUpZ = 0.0f;
    settingsSave();
    server.send(200, "text/plain", "OK");
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// Serves one of the dashboard's files at exactly `uri`. The filesystem image
// holds each of them gzipped (path + ".gz" — tools/gzip_web_files.py does
// that when the image is built), and streamFile() adds "Content-Encoding:
// gzip" by itself for a file whose name ends in .gz, so the browser gets the
// page at about a third of its size and unpacks it. A plain file is still
// served if that is what's there (an image built without the script).
//
// Not server.serveStatic(): it decides at registration time whether its
// path is a file, and with only the .gz on the filesystem it decides it
// isn't — the route then matches every URI that merely starts with `uri`,
// so the one for "/" would answer (with a 404) every GET meant for the API.
static void serveWebFile(const char* uri, const char* path, const char* contentType) {
    server.on(uri, HTTP_GET, [path, contentType]() {
        String gz = String(path) + ".gz";

        File f = LittleFS.open(LittleFS.exists(gz) ? gz : String(path), "r");
        if (!f || f.isDirectory()) {
            server.send(404, "text/plain", "Not found");
            return;
        }

        server.streamFile(f, contentType);
        f.close();
    });
}

bool serverInit() {
    Serial.println("Initializing Web Server...");

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return false;
    }

    serveWebFile("/", "/index.html", "text/html");
    serveWebFile("/runs", "/runs.html", "text/html");
    serveWebFile("/run", "/run.html", "text/html");
    serveWebFile("/compare", "/compare.html", "text/html");
    serveWebFile("/trends", "/trends.html", "text/html");
    serveWebFile("/settings", "/settings.html", "text/html");
    serveWebFile("/style.css", "/style.css", "text/css");
    serveWebFile("/common.js", "/common.js", "application/javascript");
    serveWebFile("/telemetry.js", "/telemetry.js", "application/javascript");
    serveWebFile("/version.json", "/version.json", "application/json");

    server.on("/api/status", handleApiStatus);
    server.on("/api/time", HTTP_POST, handleApiTimeSet);
    server.on("/api/runs", HTTP_GET, handleApiRuns);
    server.on("/api/runs", HTTP_DELETE, handleApiRunDelete);
    server.on("/api/runs/notes", HTTP_POST, handleApiRunNotes);
    server.on("/api/runs/clear", HTTP_POST, handleApiRunsClear);
    server.on("/api/runs/download", handleApiRunDownload);
    server.on("/api/runs/telemetry", handleApiRunTelemetry);
    server.on("/api/runs/downloadzip", handleApiRunDownloadZip);
    server.on("/api/config", HTTP_GET, handleApiConfigGet);
    server.on("/api/config", HTTP_POST, handleApiConfigPost);
    server.on("/api/system", handleApiSystem);
    server.on("/api/run/toggle", HTTP_POST, handleApiRunToggle);
    server.on("/api/run/timed", HTTP_POST, handleApiRunTimed);
    server.on("/api/debug/loop", HTTP_GET, handleApiDebugLoop);
    server.on("/api/debug/log", HTTP_GET, handleApiDebugLog);

    server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
    server.on("/api/wifi/status", HTTP_GET, handleApiWifiStatus);
    server.on("/api/wifi/mode", HTTP_POST, handleApiWifiMode);

    server.on("/api/suspension/calibration",      HTTP_GET,  handleApiSuspensionCalibrationGet);
    server.on("/api/suspension/calibration/zero", HTTP_POST, handleApiSuspensionCaptureZero);
    server.on("/api/suspension/calibration/max",  HTTP_POST, handleApiSuspensionCaptureMax);
    server.on("/api/suspension/travel",           HTTP_POST, handleApiSuspensionSetTravel);
    server.on("/api/suspension/sag/capture",      HTTP_POST, handleApiSuspensionCaptureSag);
    server.on("/api/suspension/reset",            HTTP_POST, handleApiSuspensionReset);

    server.on("/api/imu",            HTTP_GET,  handleApiImuGet);
    server.on("/api/imu/zero",       HTTP_POST, handleApiImuZero);
    server.on("/api/imu/clear",      HTTP_POST, handleApiImuClear);

    server.on("/api/bikes",          HTTP_GET,  handleApiBikesList);
    server.on("/api/bikes/profile",  HTTP_GET,  handleApiBikeProfile);
    server.on("/api/bikes/activate", HTTP_POST, handleApiBikeActivate);
    server.on("/api/bikes/save",     HTTP_POST, handleApiBikeSave);
    server.on("/api/bikes/delete",   HTTP_POST, handleApiBikeDelete);
    server.on("/api/bikes/rename",   HTTP_POST, handleApiBikeRename);

    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("Web Server Ready!");
    return true;
}

void serverHandle() {
    if(wifiGetMode() != WifiMode::NET_OFF) {
        server.handleClient();
    }
}
