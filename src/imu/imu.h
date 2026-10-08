#pragma once

#include <Arduino.h>

// Polling happens internally on its own FreeRTOS task started here — see
// imu.cpp for why (a stuck INT line can stall the BNO08x's SPI HAL for
// seconds at a time, which is not safe to do on the watchdog-monitored
// loop() task). There is no imuUpdate(): nothing to call from loop().
bool imuInit();

// Link health, for judging whether the INT line is behaving without reading
// the serial log. resets counts every time the sensor's reset flag fired
// (each one wipes the enabled reports and costs ~200ms to restore);
// streaming is false when no report has arrived for over a second.
struct ImuHealth {
    uint32_t resets;
    bool streaming;
    uint32_t msSinceEvent;   // UINT32_MAX if no report ever arrived
};
ImuHealth imuGetHealth();

// Orientation in RADIANS, straight from the BNO08x library's
// getPitch()/getRoll()/getYaw() — not converted to degrees anywhere on the
// device. Consumers that want degrees (run.html) convert on their side.
float imuPitch();
float imuRoll();
float imuYaw();
float imuAccelX();
float imuAccelY();
float imuAccelZ();

// ─── Zero IMU (mount correction) ───────────────────────────────────────────
// The master sits tilted inside the frame triangle (on the Tyee its x axis
// points ~64° above horizontal), so the sensor's Euler roll/pitch are NOT
// the bike's lean/pitch: with x that steep, roll turns about a nearly
// vertical axis and a real lean shows up amplified and mixed with yaw
// (5° of lean reads as ~11° of roll, 20° as ~40°). Subtracting an offset
// can't fix that; rotating into the bike's frame can, and it only needs the
// direction of gravity ("up") in the sensor's axes with the bike standing
// vertical and still. run.html applies the same maths to the logged
// pitch/roll (see bikeAngles() there) — keep the two in step.
struct ImuRestReading {
    bool ok;
    const char* error;    // set when !ok, human-readable
    float up[3];          // mean gravity-up direction in sensor axes (unit)
    float wobbleDeg;      // spread of that direction during the capture
    float mountTiltDeg;   // how far the sensor's x axis points above horizontal
};

// Blocks for durationMs (feeding the loop-task watchdog) averaging the
// gravity direction derived from the rotation vector — the same source the
// logged pitch/roll come from, so the reference and the data can't disagree
// — and rejects the capture if the sensor isn't responding or moved.
ImuRestReading imuMeasureRest(uint32_t durationMs);

// Bike-frame angles from raw Euler radians and a rest reference:
// leanDeg is + when leaning right, pitchUpDeg is + when the nose is up.
// Returns false if the reference isn't usable (never zeroed / degenerate).
bool imuBikeAngles(float pitchRad, float rollRad, const float up[3],
                   float& leanDeg, float& pitchUpDeg);