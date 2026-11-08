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

DialedWebServer server(80);

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// Serves one of the dashboard's files at exactly `uri`. The filesystem image
// holds each of them gzipped (path + ".gz" — tools/gzip_web_files.py does
// that when the image is built), and streamWebFile() adds "Content-Encoding:
// gzip" for a file whose name ends in .gz, so the browser gets the
// page at about a third of its size and unpacks it. A plain file is still
// served if that is what's there (an image built without the script).
//
// Not server.serveStatic(): it decides at registration time whether its
// path is a file, and with only the .gz on the filesystem it decides it
// isn't — the route then matches every URI that merely starts with `uri`,
// so the one for "/" would answer (with a 404) every GET meant for the API.
//
// An image (.png, .ico) is already compressed and goes into the image as it
// is, so it is opened directly: looking for a .gz that isn't there makes the
// framework log an error on every request. `maxAgeSeconds` lets the browser
// keep a file that never changes between dashboard versions (the icons)
// instead of asking this one-request-at-a-time server again on every page.
static void serveWebFile(const char* uri, const char* path, const char* contentType,
                         uint32_t maxAgeSeconds = 0) {
    server.on(uri, HTTP_GET, [path, contentType, maxAgeSeconds]() {
        String plain(path);
        bool image = plain.endsWith(".png") || plain.endsWith(".ico");
        String gz = plain + ".gz";

        File f = LittleFS.open(!image && LittleFS.exists(gz) ? gz : plain, "r");
        if (!f || f.isDirectory()) {
            server.send(404, "text/plain", "Not found");
            return;
        }

        if (maxAgeSeconds)
            server.sendHeader("Cache-Control", "max-age=" + String(maxAgeSeconds));

        // A page is told the theme with the page itself, so its first script
        // can apply it before anything is drawn. Without it, a browser's
        // first visit (nothing in its localStorage yet) drew the run page's
        // charts in the dark theme's colours until /api/config answered.
        if (strcmp(contentType, "text/html") == 0)
            server.sendHeader("Set-Cookie", "theme=" + settings.theme + "; Path=/; Max-Age=31536000; SameSite=Lax");

        server.streamWebFile(f, contentType);
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
    serveWebFile("/run.js", "/run.js", "application/javascript");
    serveWebFile("/version.json", "/version.json", "application/json");

    // The logo's icons (brand/export/web, copied into data/ by brand/export_all.sh)
    const uint32_t ICON_MAX_AGE = 7 * 24 * 3600;
    serveWebFile("/favicon.svg", "/favicon.svg", "image/svg+xml", ICON_MAX_AGE);
    serveWebFile("/favicon.ico", "/favicon.ico", "image/x-icon", ICON_MAX_AGE);
    serveWebFile("/apple-touch-icon.png", "/apple-touch-icon.png", "image/png", ICON_MAX_AGE);

    server.on("/api/status", handleApiStatus);
    server.on("/api/time", HTTP_POST, handleApiTimeSet);
    server.on("/api/runs", HTTP_GET, handleApiRuns);
    server.on("/api/runs", HTTP_DELETE, handleApiRunDelete);
    server.on("/api/runs/notes", HTTP_POST, handleApiRunNotes);
    server.on("/api/runs/bottom-outs", HTTP_POST, handleApiRunBottomOuts);
    server.on("/api/runs/setup", HTTP_POST, handleApiRunSetup);
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
