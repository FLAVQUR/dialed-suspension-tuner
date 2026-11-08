// The API for the live settings (/api/config). Routes are in serverInit().
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

    doc["frontModel"] = settings.frontModel;
    doc["rearModel"] = settings.rearModel;

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

    // Free text, trimmed and cut to length like the bike's name.
    auto modelArg = [](const char* key) {
        String model = server.arg(key);
        model.trim();
        return model.length() > MAX_SUSPENSION_MODEL_LEN ? model.substring(0, MAX_SUSPENSION_MODEL_LEN) : model;
    };

    if (server.hasArg("frontModel"))
        settings.frontModel = modelArg("frontModel");

    if (server.hasArg("rearModel"))
        settings.rearModel = modelArg("rearModel");

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
