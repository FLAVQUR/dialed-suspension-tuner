#pragma once
#include <Arduino.h>

enum SuspensionSide { FRONT, REAR };

// Deliberately holds only the sensor's own electrical calibration points —
// zeroRaw/maxRaw are tied to how the potentiometer happens to be physically
// mounted right now, not to which bike is logically active. The declared
// travel spec (mm) lives in settings.frontMaxTravelMM/rearStrokeMM instead
// (see settings.h) so it travels with a bike profile — see bikes.cpp.
struct SuspensionCalibration {
    int   zeroRaw;   // ADC reading (mV, via analogReadMilliVolts) at rest, 0mm
    int   maxRaw;    // ADC reading (mV) at full travel
    bool  zeroSet;
    bool  maxSet;
    bool  valid;     // zeroSet && maxSet
};

void suspensionInit();

// Call on every pass of loop(). Takes a sample when one is due on the
// sampling clock (settings.suspensionSampleRateHz), does nothing otherwise.
void suspensionUpdate();

// Returns when the next sample is due, having waited (spinning) for at most
// one sample period. loop() calls this right before suspensionUpdate(): the
// sample is then read within microseconds of its due time, and everything
// else loop() does comes after it, in the gap before the next one — instead
// of whatever happened to be running delaying the sample.
void suspensionWaitUntilDue();

// How many samples have been taken since boot, and the time the latest one
// was due (ms, on the millis() clock). A recording has one row per sample,
// stamped with that time — so its rows sit exactly one sample period apart.
uint32_t suspensionSampleCount();
uint32_t suspensionSampleTimeMs();

// Calibration: capture the rest position once, then the far end of travel
// once (ruler-measured mm). The far point's raw (mV) reading keeps
// extending itself automatically afterwards if a deeper real-world
// compression is ever observed, so a harder bottom-out never needs a
// manual recapture — but the declared mm value itself never changes on its
// own; it's a fixed spec the rider typed in, not something to be inferred
// from voltage drift.
bool suspensionCaptureZero(SuspensionSide side);
bool suspensionCaptureMax(SuspensionSide side, float mm);

// Persists just the declared travel spec (mm) without requiring a live
// sensor reading — lets a rider record "this shock is 55mm" in Bike
// Information before the sensor is even wired/calibrated, instead of that
// number only ever being saveable as a side effect of Capture Max. Does
// NOT set maxSet/valid — an actual bottom-out calibration is still needed
// for real travel% readings.
bool suspensionSetTravelMM(SuspensionSide side, float mm);

void suspensionResetCalibration();
SuspensionCalibration suspensionGetCalibration(SuspensionSide side);

// Readings
float suspensionGetRaw(SuspensionSide side);
float suspensionGetTravelMM(SuspensionSide side);          // interpolated between the zero/max calibration points, EMA-smoothed for live display
float suspensionGetInstantTravelMM(SuspensionSide side);   // same, but from the unsmoothed reading — use for logging so fast hits aren't lagged/clipped
float suspensionGetTravelPct(SuspensionSide side);      // travel / max calibrated mm, 0.0 to 1.0
float suspensionGetMaxTravelMM(SuspensionSide side);   // calibrated full travel (mm)
