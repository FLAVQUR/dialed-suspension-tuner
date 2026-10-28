#pragma once

#include <Arduino.h>

// The low-battery cutoff. Without it the device ran the cell down to ~2.7V
// before the charger board's protection let go (drain test, 2026-10-28).
//
// Once the smoothed voltage has stayed below LOW_BATTERY_CUTOFF_V for two
// minutes, a run that is recording is stopped (its .JSON says
// "stopped_by": "low_battery"), the GPS and the IMU are put to sleep, the SD
// card is closed and the ESP32 goes into deep sleep. The battery is the
// device's only supply, so the cutoff has to keep checking on its own: every
// 10 minutes the chip wakes, reads the battery and, unless it has been charged
// back above LOW_BATTERY_RESUME_V, goes straight back to sleep. A press of the
// button boots it in full regardless, and the two minutes start over.
//
// What is still drawing current asleep (the SD card, the LED, the boards'
// regulators) was not measured: there is no meter.

constexpr float LOW_BATTERY_CUTOFF_V = 3.30f;
constexpr float LOW_BATTERY_RESUME_V = 3.50f;

// First thing in setup(). After a wake from the cutoff's own sleep, reads the
// battery and goes back to sleep if it hasn't been charged; returns otherwise.
void lowBatteryBootCheck();

// Every pass of loop(), after batteryUpdate(). Does not return once the
// cutoff fires.
void lowBatteryUpdate();
