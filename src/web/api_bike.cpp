// The API for the bike and its sensors: suspension calibration, bike profiles,
// the IMU's zero. Routes are in serverInit().
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
