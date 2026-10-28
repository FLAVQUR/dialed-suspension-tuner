#include <Arduino.h>
#include <esp_task_wdt.h>

#include "button/button.h"
#include "connectivity/network.h"
#include "connectivity/ota.h"
#include "web/server.h"
#include "gps/gps.h"
#include "imu/imu.h"
#include "storage/storage.h"
#include "led/led.h"
#include "button/button.h"
#include "system/system.h"
#include "system/loop_profiler.h"
#include "system/log_capture.h"
#include "power/battery.h"
#include "power/low_battery.h"
#include "logger/logger.h"
#include "suspension/suspension.h"
#include "system/settings.h"
#include "run/run_service.h"

// If loop() ever stops coming back around within this long — an I2C lockup,
// a wedged SD operation, an unhandled edge case in the web server — the
// device is unattended on a trail with no way for the rider to know except
// a frozen dashboard. This is well above every stall length actually
// observed while profiling (worst seen: ~171ms SD flush, ~124ms GPS
// hiccup), so it won't false-trigger on normal jitter, but still recovers
// promptly if something truly hangs.
constexpr uint32_t WATCHDOG_TIMEOUT_S = 8;

void setup() {
    Serial.begin(115200);

    // From here on, what the core and its libraries log is also kept in
    // memory — see log_capture.h.
    logCaptureInit();

    Serial.printf("Reset reason: %s\n", systemResetReason());

    // Before anything else is started: a wake from the low-battery cutoff's
    // sleep with the battery still flat goes straight back to sleep.
    lowBatteryBootCheck();
    Serial.println("Initializing components...");

    systemInit();
    settingsLoadDefaults();
    settingsLoad();

    settingsPrint();

    ledInit();
    buttonInit();
    suspensionInit();

    delay(1000);

    components.storage = storageInit();

    if (components.storage) {
        RunService::init();
        RunService::recoverOrphanedRuns();
    }

    batteryInit();

    delay(500);

    components.imu = imuInit();

    // components.gps itself is set continuously from inside gpsUpdate()
    // as real NMEA data comes in — gpsInit() only sets up the UART, it
    // can't know yet whether a module is actually attached.
    gpsInit();

    wifiInit();
    otaInit();
    serverInit();

    esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);  // watch the loop() task

    // Core 0's IDLE task is watched by default, and starving it for 8s resets
    // the chip. The SD library busy-waits on the card without yielding, and a
    // card that stops answering can keep the sdWriter task spinning that long
    // (RUN0031: task_wdt with IDLE0 starved and sdWriter running, Wi-Fi off).
    // The reset threw away the rest of the run; without the check the stalled
    // writer just stops the CSV growing (rows are dropped once the queue is
    // full, isWriteStalled() raises the LED alert) and everything else — the
    // watched loop(), recording state, the button — keeps working. Same
    // reasoning as keeping imuTask off the watchdog.
    disableCore0WDT();
}

// A loop() iteration this long leaves a visible hole in the recorded data:
// nothing is sampled while it runs, so the CSV jumps straight over it.
// RUN0038 had 15 such holes (65-270ms) adding up to 2.3s of a 27s run — long
// enough to miss a suspension impact peak, which is the whole reason sampling
// was moved off the SD writer's core in the first place. The writer is not the
// cause: the rows either side of each hole show flush_us == 0, so no flush was
// even running. This reports which call inside loop() actually blocked.
constexpr uint32_t LOOP_STALL_REPORT_US = 50000;

// One pass of loop() is one sample period, and the sample comes first.
//
// The pass starts by waiting for the sampling clock, reads the suspension the
// moment a sample is due and logs the row. Everything else — LED, battery,
// GPS, WiFi, the web server — then runs in what is left of the period, which
// at 500Hz is about three quarters of it. Done in that order, none of that
// work can make a sample late: the worst it can do is overrun into the next
// period, and the clock absorbs that (see suspensionUpdate()).
//
// It used to be the other way round: the housekeeping ran first on every
// pass, at whatever rate loop() span, and the suspension was read whenever
// the pass got to it.
//
// Every section is timed (see loop_profiler.h): the totals are read back over
// the API, and a single pass that ran long is reported below.
void loop() {
    LoopProfiler::passStart();

    suspensionWaitUntilDue();
    LoopProfiler::lap(LoopProfiler::WAIT);

    suspensionUpdate();
    LoopProfiler::lap(LoopProfiler::SUSPENSION);

    loggerUpdate();
    RunService::update();
    LoopProfiler::lap(LoopProfiler::LOGGER);

    systemUpdate();
    buttonUpdate();
    LoopProfiler::lap(LoopProfiler::SYSTEM);

    ledSetWriteStall(RunService::isWriteStalled());
    ledUpdate();
    LoopProfiler::lap(LoopProfiler::LED);

    batteryUpdate();
    lowBatteryUpdate();
    LoopProfiler::lap(LoopProfiler::BATTERY);

    gpsUpdate();
    LoopProfiler::lap(LoopProfiler::GPS);

    wifiUpdate();
    LoopProfiler::lap(LoopProfiler::WIFI);

    otaUpdate();
    LoopProfiler::lap(LoopProfiler::OTA);

    serverHandle();
    LoopProfiler::lap(LoopProfiler::SERVER);

    // The time spent waiting for the clock is not a stall.
    uint32_t busy = LoopProfiler::passEnd(getState() == RECORDING);

    if (busy > LOOP_STALL_REPORT_US) {
        uint32_t worstUs = 0;
        const char* worst = LoopProfiler::worstSection(worstUs);

        Serial.printf("loop stall %lu us — worst: %s %lu us\n",
                      (unsigned long)busy, worst, (unsigned long)worstUs);
    }

    esp_task_wdt_reset();
}
