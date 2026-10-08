#pragma once
#include <Arduino.h>

struct SystemConfig {
    String bikeName;
    bool fullSuspension;
    bool wifiOffWhileRecording;

    String theme;  // "dark" or "light" — UI theme for the web dashboard

    // Connectivity — the Access Point is always available as a fallback;
    // the station (home/"master" network) is optional. staSSID/staPassword
    // seed from config/secrets.h on first boot so existing behavior is
    // unchanged until the rider reconfigures this from Settings.
    String apSSID;
    String apPassword;

    bool   staEnabled;
    String staSSID;
    String staPassword;

    // Suspension ADC sampling rate (500 or 1000 Hz). Also drives how often a
    // CSV row gets logged during a run — GPS and IMU columns just repeat
    // their latest cached reading between their own, slower updates. See
    // logger.cpp and suspension.cpp. The IMU's own report rate is fixed at
    // its hardware ceiling (see imu.cpp) — not configurable, so there's only
    // one Hz setting in the whole app.
    uint16_t suspensionSampleRateHz;

    // How many of the most recent runs the Trends page pulls in and
    // analyzes — each one costs a full CSV read/parse client-side, so
    // this is a real tradeoff between how much history you see and how
    // long the page takes to load, not just a display preference.
    uint16_t trendsRunCount;

    String wheelSize;  // "29", "27.5", or "mullet" (29 front / 27.5 rear)

    // Wheel-equivalent rear travel (e.g. 160mm), purely informational — the
    // rear is calibrated directly in shock-stroke mm (see suspension.cpp),
    // not derived from this via a leverage curve.
    float rearWheelTravelMM;

    // The real travel specs used in every %-travel calculation (see
    // suspension.cpp) — front fork travel and rear shock stroke, ruler-
    // measured. Lives here rather than in SuspensionCalibration so it
    // travels with a bike profile (bikes.cpp): switching bikes restores the
    // correct course together with LSC/HSC/pressure, instead of only the
    // sensor's zero/max electrical points migrating. rearStrokeMM is NOT
    // the same number as rearWheelTravelMM above (shock stroke vs.
    // wheel-equivalent via leverage ratio).
    float frontMaxTravelMM;
    float rearStrokeMM;

    // Suspension setup — not used in any calculation today (travel/sag come
    // from the calibration), just recorded for reference since it's handy
    // to have logged alongside a run later. Split into low/high-speed
    // compression and rebound rather than one combined "compression"/
    // "rebound" value, since that's how most air forks/shocks with more
    // than a single damper adjuster are actually specced — a fork with
    // just one compression knob can leave HSC at 0 and use LSC alone.
    uint8_t frontLSC;
    uint8_t frontHSC;
    uint8_t frontLSR;
    uint8_t frontHSR;
    uint8_t frontVolumeTokens;
    float   frontPressurePsi;

    // Whether this fork/shock actually HAS each adjuster — 0 clicks is a
    // real, meaningful setting (fully open) and must stay distinguishable
    // from "no such dial exists" (e.g. a RockShox Domain's HSC is a fixed
    // shim stack with no click count at all). Without this, LSC/HSC/LSR/HSR
    // above default to 0 either way and the dashboard's tuning suggestions
    // (run.html's generateSuggestions()) can't tell which is which —
    // defaults to true for LSC/LSR (the split nearly every damper has some
    // form of) and false for HSC/HSR (the split most dampers don't).
    bool frontHasLSC;
    bool frontHasHSC;
    bool frontHasLSR;
    bool frontHasHSR;

    // Static sag — how much travel (%) is used just from the rider's own
    // weight, sitting still on the bike. Captured live from the sensor
    // (see suspensionGetTravelPct(), exposed via
    // /api/suspension/sag/capture) rather than typed in, since it's a
    // physical measurement, not a spec. 0 means never captured. Goes
    // stale whenever the pressure/tokens above change — there's no way to
    // detect that automatically, so run.html's Analysis tab just trusts whatever
    // was last captured.
    float   frontSagPct;

    uint8_t rearLSC;
    uint8_t rearHSC;
    uint8_t rearLSR;
    uint8_t rearHSR;
    uint8_t rearVolumeTokens;
    float   rearPressurePsi;
    float   rearSagPct;

    // See frontHasLSC etc. above.
    bool rearHasLSC;
    bool rearHasHSC;
    bool rearHasLSR;
    bool rearHasHSR;

    // "Zero IMU" reference: the direction of gravity ("up") in the IMU's own
    // axes with the bike standing vertical and still — a unit vector, all
    // zeros = never zeroed. The master sits tilted inside the frame (its x
    // axis ~64° above horizontal on the Tyee), so the raw Euler roll/pitch
    // aren't bike lean/pitch; run.html rotates them into the bike's frame
    // using this. Like the suspension calibration it describes how the
    // master is mounted right now, so it is deliberately NOT part of a bike
    // profile (switching profiles must not lose it). Each run's .JSON gets
    // its own copy as "imu_up". See imu.h.
    float imuUpX;
    float imuUpY;
    float imuUpZ;
};

extern SystemConfig settings;

constexpr size_t MAX_BIKE_NAME_LEN = 40;

void settingsLoadDefaults();
void settingsLoad();
void settingsSave();
void settingsPrint();

// Resets only the connectivity fields (AP + station credentials) back to
// their firmware defaults and persists them — used by the button
// long-press emergency reset, without touching bike/calibration settings.
void settingsResetConnectivity();
