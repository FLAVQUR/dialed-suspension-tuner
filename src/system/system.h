#pragma once

#include <Arduino.h>

enum SystemState {
    BOOTING,
    READY,
    RECORDING,
    ERROR
};

struct ComponentStatus
{
    bool storage = false;
    bool imu = false;
    bool gps = false;
    bool wifi = false;
};

extern ComponentStatus components;

void systemInit();
void systemUpdate();

// Why the chip last reset, e.g. "panic", "brownout", "task_wdt", "int_wdt",
// "software", "power_on". Captured once at boot, so the dashboard can say
// what happened after an unexplained mid-run reboot without a serial cable.
const char* systemResetReason();

bool allComponentsReady();

SystemState getState();
void toggleRecording();

// Gets the device ready to record: with "WiFi off while recording" set, the
// radio is taken down here — before the run's file is opened and before its
// first row, since shutting it down holds loop() up for ~100ms and that used
// to be a hole in every recording, half a second in. Blocks for that long.
//
// recordingRelease() undoes it: the radio comes back if it was taken down.
// toggleRecording() calls it when a run ends; a run that could not be
// started after all has to call it itself.
void recordingPrepare();
void recordingRelease();