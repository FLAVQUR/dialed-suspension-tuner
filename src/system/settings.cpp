#include <Preferences.h>
#include "settings.h"

static Preferences prefs;

SystemConfig settings;

// Suffixed with the last 2 bytes of the chip's factory-burned MAC so
// multiple bikes/devices don't all broadcast the exact same hotspot name —
// each one is still unique and reproducible without needing to be saved
// anywhere. Shared by settingsLoadDefaults() and settingsResetConnectivity()
// so the two can't drift apart.
static String defaultApSSID()
{
    uint16_t suffix = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
    char buf[24];
    snprintf(buf, sizeof(buf), "DialedTuner-%04X", suffix);
    return String(buf);
}

static constexpr char DEFAULT_AP_PASSWORD[] = "dialedtuner";

void settingsLoadDefaults()
{
    settings.bikeName = "";
    settings.fullSuspension = true;
    settings.wifiOffWhileRecording = true;
    settings.theme = "dark";
    settings.suspensionSampleRateHz = 500;
    settings.trendsRunCount = 20;

    settings.apSSID = defaultApSSID();
    settings.apPassword = DEFAULT_AP_PASSWORD;

    // No home network configured out of the box — set it once from the
    // Connectivity settings and it's saved to NVS from then on.
    settings.staEnabled = false;
    settings.staSSID = "";
    settings.staPassword = "";

    settings.wheelSize = "29";
    settings.rearWheelTravelMM = 0.0f;
    settings.frontMaxTravelMM = 0.0f;
    settings.rearStrokeMM = 0.0f;

    settings.frontLSC = 0;
    settings.frontHSC = 0;
    settings.frontLSR = 0;
    settings.frontHSR = 0;
    settings.frontVolumeTokens = 0;
    settings.frontPressurePsi = 0.0f;
    settings.frontSagPct = 0.0f;

    settings.frontHasLSC = true;
    settings.frontHasHSC = false;
    settings.frontHasLSR = true;
    settings.frontHasHSR = false;

    settings.rearLSC = 0;
    settings.rearHSC = 0;
    settings.rearLSR = 0;
    settings.rearHSR = 0;
    settings.rearVolumeTokens = 0;
    settings.rearPressurePsi = 0.0f;
    settings.rearSagPct = 0.0f;

    settings.rearHasLSC = true;
    settings.rearHasHSC = false;
    settings.rearHasLSR = true;
    settings.rearHasHSR = false;

    settings.imuUpX = 0.0f;
    settings.imuUpY = 0.0f;
    settings.imuUpZ = 0.0f;
}

void settingsLoad()
{
    prefs.begin("bike", false);

    settings.bikeName =
        prefs.getString(
            "bikeName",
            settings.bikeName
        );

    settings.fullSuspension =
        prefs.getBool(
            "fullSusp",
            settings.fullSuspension
        );

    settings.wifiOffWhileRecording =
        prefs.getBool(
            "wifiOffRec",
            settings.wifiOffWhileRecording
        );

    settings.theme =
        prefs.getString(
            "theme",
            settings.theme
        );

    settings.suspensionSampleRateHz =
        prefs.getUShort(
            "suspHz",
            settings.suspensionSampleRateHz
        );

    settings.trendsRunCount =
        prefs.getUShort(
            "trendCount",
            settings.trendsRunCount
        );

    settings.apSSID =
        prefs.getString(
            "apSSID",
            settings.apSSID
        );

    settings.apPassword =
        prefs.getString(
            "apPass",
            settings.apPassword
        );

    settings.staEnabled =
        prefs.getBool(
            "staEnabled",
            settings.staEnabled
        );

    settings.staSSID =
        prefs.getString(
            "staSSID",
            settings.staSSID
        );

    settings.staPassword =
        prefs.getString(
            "staPass",
            settings.staPassword
        );

    settings.wheelSize =
        prefs.getString(
            "wheelSize",
            settings.wheelSize
        );

    settings.rearWheelTravelMM =
        prefs.getFloat(
            "rWheelTrvl",
            settings.rearWheelTravelMM
        );

    settings.frontMaxTravelMM = prefs.getFloat("fMaxTrvl", settings.frontMaxTravelMM);
    settings.rearStrokeMM = prefs.getFloat("rStrokeMM", settings.rearStrokeMM);

    settings.frontLSC = prefs.getUChar("fLSC", settings.frontLSC);
    settings.frontHSC = prefs.getUChar("fHSC", settings.frontHSC);
    settings.frontLSR = prefs.getUChar("fLSR", settings.frontLSR);
    settings.frontHSR = prefs.getUChar("fHSR", settings.frontHSR);
    settings.frontVolumeTokens = prefs.getUChar("fVolTok", settings.frontVolumeTokens);

    settings.frontPressurePsi =
        prefs.getFloat(
            "fPressPsi",
            settings.frontPressurePsi
        );

    settings.frontSagPct = prefs.getFloat("fSagPct", settings.frontSagPct);

    settings.frontHasLSC = prefs.getBool("fHasLSC", settings.frontHasLSC);
    settings.frontHasHSC = prefs.getBool("fHasHSC", settings.frontHasHSC);
    settings.frontHasLSR = prefs.getBool("fHasLSR", settings.frontHasLSR);
    settings.frontHasHSR = prefs.getBool("fHasHSR", settings.frontHasHSR);

    settings.rearLSC = prefs.getUChar("rLSC", settings.rearLSC);
    settings.rearHSC = prefs.getUChar("rHSC", settings.rearHSC);
    settings.rearLSR = prefs.getUChar("rLSR", settings.rearLSR);
    settings.rearHSR = prefs.getUChar("rHSR", settings.rearHSR);
    settings.rearVolumeTokens = prefs.getUChar("rVolTok", settings.rearVolumeTokens);

    settings.rearPressurePsi =
        prefs.getFloat(
            "rPressPsi",
            settings.rearPressurePsi
        );

    settings.rearSagPct = prefs.getFloat("rSagPct", settings.rearSagPct);

    settings.rearHasLSC = prefs.getBool("rHasLSC", settings.rearHasLSC);
    settings.rearHasHSC = prefs.getBool("rHasHSC", settings.rearHasHSC);
    settings.rearHasLSR = prefs.getBool("rHasLSR", settings.rearHasLSR);
    settings.rearHasHSR = prefs.getBool("rHasHSR", settings.rearHasHSR);

    settings.imuUpX = prefs.getFloat("imuUpX", settings.imuUpX);
    settings.imuUpY = prefs.getFloat("imuUpY", settings.imuUpY);
    settings.imuUpZ = prefs.getFloat("imuUpZ", settings.imuUpZ);
}

void settingsSave()
{
    prefs.putString(
        "bikeName",
        settings.bikeName
    );

    prefs.putBool(
        "fullSusp",
        settings.fullSuspension
    );

    prefs.putBool(
        "wifiOffRec",
        settings.wifiOffWhileRecording
    );

    prefs.putString(
        "theme",
        settings.theme
    );

    prefs.putUShort(
        "suspHz",
        settings.suspensionSampleRateHz
    );

    prefs.putUShort(
        "trendCount",
        settings.trendsRunCount
    );

    prefs.putString(
        "apSSID",
        settings.apSSID
    );

    prefs.putString(
        "apPass",
        settings.apPassword
    );

    prefs.putBool(
        "staEnabled",
        settings.staEnabled
    );

    prefs.putString(
        "staSSID",
        settings.staSSID
    );

    prefs.putString(
        "staPass",
        settings.staPassword
    );

    prefs.putString(
        "wheelSize",
        settings.wheelSize
    );

    prefs.putFloat(
        "rWheelTrvl",
        settings.rearWheelTravelMM
    );

    prefs.putFloat("fMaxTrvl", settings.frontMaxTravelMM);
    prefs.putFloat("rStrokeMM", settings.rearStrokeMM);

    prefs.putUChar("fLSC", settings.frontLSC);
    prefs.putUChar("fHSC", settings.frontHSC);
    prefs.putUChar("fLSR", settings.frontLSR);
    prefs.putUChar("fHSR", settings.frontHSR);
    prefs.putUChar("fVolTok", settings.frontVolumeTokens);

    prefs.putFloat(
        "fPressPsi",
        settings.frontPressurePsi
    );

    prefs.putFloat("fSagPct", settings.frontSagPct);

    prefs.putBool("fHasLSC", settings.frontHasLSC);
    prefs.putBool("fHasHSC", settings.frontHasHSC);
    prefs.putBool("fHasLSR", settings.frontHasLSR);
    prefs.putBool("fHasHSR", settings.frontHasHSR);

    prefs.putUChar("rLSC", settings.rearLSC);
    prefs.putUChar("rHSC", settings.rearHSC);
    prefs.putUChar("rLSR", settings.rearLSR);
    prefs.putUChar("rHSR", settings.rearHSR);
    prefs.putUChar("rVolTok", settings.rearVolumeTokens);

    prefs.putFloat(
        "rPressPsi",
        settings.rearPressurePsi
    );

    prefs.putFloat("rSagPct", settings.rearSagPct);

    prefs.putBool("rHasLSC", settings.rearHasLSC);
    prefs.putBool("rHasHSC", settings.rearHasHSC);
    prefs.putBool("rHasLSR", settings.rearHasLSR);
    prefs.putBool("rHasHSR", settings.rearHasHSR);

    prefs.putFloat("imuUpX", settings.imuUpX);
    prefs.putFloat("imuUpY", settings.imuUpY);
    prefs.putFloat("imuUpZ", settings.imuUpZ);
}

void settingsResetConnectivity()
{
    settings.apSSID = defaultApSSID();
    settings.apPassword = DEFAULT_AP_PASSWORD;

    settings.staEnabled = false;
    settings.staSSID = "";
    settings.staPassword = "";

    settingsSave();
}

void settingsPrint() {
    Serial.println("Settings:");
    Serial.println("Bike: " + settings.bikeName);
    Serial.println("Full Suspension: " + String(settings.fullSuspension ? "yes" : "no"));
    Serial.println("Wheel Size: " + settings.wheelSize);
    Serial.println("WiFi off while recording: " + String(settings.wifiOffWhileRecording ? "yes" : "no"));
    Serial.println("Theme: " + settings.theme);
    Serial.println("AP SSID: " + settings.apSSID);
    Serial.println("Station: " + String(settings.staEnabled ? settings.staSSID : "disabled"));
    Serial.println("Suspension sample rate: " + String(settings.suspensionSampleRateHz) + " Hz");
    Serial.println("Front suspension: LSC " + String(settings.frontLSC) + " / HSC " + String(settings.frontHSC) +
        " / LSR " + String(settings.frontLSR) + " / HSR " + String(settings.frontHSR) +
        " / " + String(settings.frontVolumeTokens) + " tokens / " + String(settings.frontPressurePsi, 0) + " psi" +
        " / sag " + (settings.frontSagPct > 0 ? String(settings.frontSagPct, 1) + "%" : "not captured"));
    Serial.println("Rear suspension: LSC " + String(settings.rearLSC) + " / HSC " + String(settings.rearHSC) +
        " / LSR " + String(settings.rearLSR) + " / HSR " + String(settings.rearHSR) +
        " / " + String(settings.rearVolumeTokens) + " tokens / " + String(settings.rearPressurePsi, 0) + " psi" +
        " / sag " + (settings.rearSagPct > 0 ? String(settings.rearSagPct, 1) + "%" : "not captured"));
}
