#include "logger.h"
#include "run/run_service.h"
#include "gps/gps.h"
#include "imu/imu.h"
#include "system/system.h"
#include "system/loop_profiler.h"
#include "suspension/suspension.h"

// A CSV row is logged for every suspension sample — one row, one fresh
// reading, stamped with the time that sample was due (see suspensionUpdate()
// for the clock). GPS and IMU are simply read as often, returning whatever
// their own slower update cycle last cached, so their columns repeat between
// updates.
//
// The logger used to keep a timer of its own at the same nominal rate. Two
// timers that each restart from whenever they happened to fire don't stay in
// step: a row could repeat the previous sample or skip one, and both ran
// slow.
static uint32_t lastLoggedSample = 0;

void loggerUpdate() {
    uint32_t sample = suspensionSampleCount();

    // Kept up to date while idle, so that a run's first row is the first
    // sample taken after it started and not one left over from before.
    if (getState() != RECORDING) {
        lastLoggedSample = sample;
        return;
    }

    if (sample == lastLoggedSample)
        return;

    lastLoggedSample = sample;

    LoopProfiler::rowLogged();

    RunService::log(
        suspensionSampleTimeMs(),

        gpsLatitude(),
        gpsLongitude(),
        gpsSpeedKmh(),
        gpsSatellites(),
        gpsHdop(),
        gpsAltitude(),

        imuPitch(),
        imuRoll(),
        imuYaw(),

        imuAccelX(),
        imuAccelY(),
        imuAccelZ(),

        suspensionGetInstantTravelMM(FRONT),
        suspensionGetInstantTravelMM(REAR)
    );
}
