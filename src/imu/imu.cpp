#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <SparkFun_BNO08x_Arduino_Library.h>
#include <ArduinoJson.h>
#include "imu.h"
#include "config/pins.h"

BNO08x imu;

// A dedicated HSPI instance, separate from the SD card's (default/VSPI)
// bus — so IMU reads and SD writes never contend for the same physical
// SPI peripheral across the two cores. See RunService's SD-writer task in
// run_service.cpp for the other half of this split.
static SPIClass imuSpi(HSPI);

// The BNO08x library stores rotation-vector and accelerometer reports in
// shared state — imu.getYaw()/getAccelZ()/etc. just return whatever's
// currently there, regardless of which report type was last processed.
// Cache each group separately here, only updating a group when its own
// report type is confirmed via getSensorEventID(), so a read of one group
// never returns data reinterpreted from the other. Written only by
// imuTask(), read from other tasks/cores (logger, dashboard) — plain
// floats with no lock, same relaxed-flag style already used elsewhere in
// this codebase for cross-core state.
static float cachedPitch = 0, cachedRoll = 0, cachedYaw = 0;
static float cachedAccelX = 0, cachedAccelY = 0, cachedAccelZ = 0;

// Deliberately NOT the sensor's hardware maximum (~400Hz / 2ms interval).
// Draining a backlog of unread reports over SPI/I2C on every visit was
// previously costing a big chunk of the (then-shared) loop() budget —
// see the dual-core note below for why that budget concern doesn't apply
// the same way anymore, but there's still no benefit to requesting faster
// reports than anything downstream actually consumes.
constexpr uint16_t IMU_REPORT_INTERVAL_MS = 10;

// Mirrors SparkFun's own SPI example: enabling a report right after reset
// (or right after another enableX() call) needs a moment before the
// BNO08x actually accepts it and clears its internal reset status —
// firing enableRotationVector()+enableAccelerometer() back-to-back with
// no gap was very likely why one call kept coming back FAILED and, even
// when both reported OK, no reports ever actually started streaming.
// Also called again whenever imu.wasReset() fires, since a reset (the
// library triggers one itself if the INT line ever times out) silently
// wipes whatever reports were configured.
static void enableReports() {
    bool rvOk = imu.enableRotationVector(IMU_REPORT_INTERVAL_MS);
    delay(100);
    bool accOk = imu.enableAccelerometer(IMU_REPORT_INTERVAL_MS);
    delay(100);

    Serial.printf("IMU reports enabled: rotationVector=%s accelerometer=%s\n",
        rvOk ? "OK" : "FAILED", accOk ? "OK" : "FAILED");
}

// Runs on its own FreeRTOS task, deliberately never registered with the
// watchdog that watches loop() (see main.cpp). Field testing showed the
// BNO08x's SPI HAL can stall for 8+ seconds straight inside a single
// getSensorEvent() call when the INT line is unreliable — hal_wait_for_int()
// alone retries for up to 500ms per transaction, and there's no safe point
// to inject a watchdog reset in the middle of a third-party library call
// that never returns. Isolating IMU polling here means a stuck or
// misbehaving sensor only ever costs IMU data (pitch/roll/accel read as
// stale/zero) instead of taking down suspension logging, GPS, recording
// and the web dashboard with it.
// Written by imuTask, read by imuGetHealth() from other tasks — same
// lock-free plain-state style as the cached readings above.
static volatile uint32_t resetCount = 0;
static volatile uint32_t lastEventMs = 0;
static volatile bool gotFirstEvent = false;

// ─── How sure the sensor is of its orientation, second by second ───────────
// On rough ground the fused orientation goes wrong for seconds at a time
// (found 2026-10-16, see docs/hardware.md), and the accelerometer hitting its ±8g
// ceiling is the likeliest reason, not a proven one. These record, for each
// second of a run, what would tell: the worst accuracy status the rotation
// vector gave itself that second (0 unreliable … 3 high), the largest
// accuracy estimate it gave (in degrees), and how many accelerometer
// reports had an axis at the ceiling. Written by imuTask, cleared and read
// by RunService at the start and end of a run.
constexpr int DIAG_SECONDS = 900;   // 15 minutes; a run longer than that keeps its first 15
constexpr float ACCEL_CEILING_MS2 = 78.0f;

static uint8_t diagStatus[DIAG_SECONDS];      // 255: no rotation-vector report that second
static uint8_t diagAccuracyDeg[DIAG_SECONDS];
static uint8_t diagClipped[DIAG_SECONDS];
static volatile bool diagActive = false;
static volatile uint32_t diagStartMs = 0;
static volatile int diagSeconds = 0;

static int diagSecond() {
    int s = (int)((millis() - diagStartMs) / 1000);
    if (s >= DIAG_SECONDS) return -1;
    if (s + 1 > diagSeconds) diagSeconds = s + 1;
    return s;
}

void imuDiagStart() {
    memset(diagStatus, 255, sizeof(diagStatus));
    memset(diagAccuracyDeg, 0, sizeof(diagAccuracyDeg));
    memset(diagClipped, 0, sizeof(diagClipped));
    diagSeconds = 0;
    diagStartMs = millis();
    diagActive = true;
}

void imuDiagStop() {
    diagActive = false;
}

void imuDiagToJson(JsonObject out) {
    JsonArray status = out["status"].to<JsonArray>();
    JsonArray accuracy = out["accuracy_deg"].to<JsonArray>();
    JsonArray clipped = out["accel_clipped"].to<JsonArray>();

    for (int s = 0; s < diagSeconds && s < DIAG_SECONDS; s++) {
        status.add(diagStatus[s] == 255 ? -1 : (int)diagStatus[s]);
        accuracy.add(diagAccuracyDeg[s]);
        clipped.add(diagClipped[s]);
    }
}

ImuHealth imuGetHealth() {
    ImuHealth h;
    h.resets = resetCount;
    h.msSinceEvent = gotFirstEvent ? (millis() - lastEventMs) : UINT32_MAX;
    h.streaming = gotFirstEvent && h.msSinceEvent < 1000;
    return h;
}

// How long the sensor gets to produce its first report before the "check the
// INT line" warning fires. Measured from when this task starts, NOT from
// boot: imuInit() runs late in setup() (after the 1s+0.5s delays, SD init and
// the orphaned-run scan), so millis() is already ~3s by then and a plain
// "millis() > 3000" test fired on this task's first pass on every single
// boot — the warning was pure noise, blamed on the INT solder joint, on
// hardware that was streaming fine moments later.
constexpr uint32_t FIRST_REPORT_GRACE_MS = 3000;

// Set by imuSleep(); the task itself sends the command, since the SPI link
// is its own.
static volatile bool sleepRequested = false;
static volatile bool sleeping = false;
static volatile bool sleepAccepted = false;
static bool taskStarted = false;

static void imuTask(void*) {
    const uint32_t taskStartMs = millis();
    bool warnedNoData = false;
    uint32_t lastReenableMs = 0;
    bool warnedStale = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2));

        if (sleepRequested) {
            sleepAccepted = imu.modeSleep();
            sleeping = true;
            vTaskSuspend(nullptr);
        }

        // Checking the throttle before wasReset() itself matters: wasReset()
        // clears its internal flag as soon as it's called, so checking it
        // first (and dropping the result when throttled) would silently
        // lose that reset instead of retrying once the window reopens.
        //
        // There used to be a "give up for the rest of the session" cutoff
        // here after 5 resets — added back when a stuck IMU could still
        // trip the loop() watchdog and reboot the whole device, so capping
        // retries limited the damage. Now that this task is isolated from
        // that watchdog entirely (see the comment above imuTask), that
        // safety net is gone but so is the reason for the cutoff: a field
        // run showed it permanently blanking IMU data for the rest of a
        // ride (or even the rest of a boot session) after a brief rough
        // patch caused 5 resets in a few seconds, even though the link
        // would have recovered fine on its own. Retrying forever, still
        // throttled to once/second, can only ever cost a bit of core-0
        // time — never the rest of the system.
        if (millis() - lastReenableMs > 1000 && imu.wasReset()) {
            resetCount++;
            Serial.printf("IMU: sensor reset detected (#%u) — re-enabling reports\n", resetCount);

            enableReports();
            lastReenableMs = millis();
        }

        bool gotEventThisPass = false;

        while (imu.getSensorEvent()) {
            gotFirstEvent = true;
            gotEventThisPass = true;

            switch (imu.getSensorEventID()) {
                case SENSOR_REPORTID_ROTATION_VECTOR:
                    cachedPitch = imu.getPitch();
                    cachedRoll = imu.getRoll();
                    cachedYaw = imu.getYaw();

                    if (diagActive) {
                        int s = diagSecond();
                        if (s >= 0) {
                            uint8_t status = imu.getQuatAccuracy();
                            float deg = imu.getQuatRadianAccuracy() * 57.29578f;
                            uint8_t degByte = deg >= 255.0f ? 255 : (deg <= 0.0f ? 0 : (uint8_t)deg);

                            if (diagStatus[s] == 255 || status < diagStatus[s]) diagStatus[s] = status;
                            if (degByte > diagAccuracyDeg[s]) diagAccuracyDeg[s] = degByte;
                        }
                    }
                    break;

                case SENSOR_REPORTID_ACCELEROMETER:
                    cachedAccelX = imu.getAccelX();
                    cachedAccelY = imu.getAccelY();
                    cachedAccelZ = imu.getAccelZ();

                    if (diagActive &&
                        (fabsf(cachedAccelX) >= ACCEL_CEILING_MS2 || fabsf(cachedAccelY) >= ACCEL_CEILING_MS2 || fabsf(cachedAccelZ) >= ACCEL_CEILING_MS2)) {
                        int s = diagSecond();
                        if (s >= 0 && diagClipped[s] < 255) diagClipped[s]++;
                    }
                    break;
            }
        }

        if (gotEventThisPass) {
            lastEventMs = millis();
            warnedStale = false;  // re-arm: worth warning again if it drops out a second time
        }

        if (!gotFirstEvent && !warnedNoData && millis() - taskStartMs > FIRST_REPORT_GRACE_MS) {
            warnedNoData = true;
            Serial.println("IMU: no sensor reports in the first 3s of polling — check INT wiring/solder joint");
        } else if (gotFirstEvent && !warnedStale && millis() - lastEventMs > 3000) {
            // Distinct from the "never worked at all" warning above — this
            // is a link that was fine and then went quiet mid-session,
            // which is exactly what a field run showed happening partway
            // through a ride.
            warnedStale = true;
            Serial.println("IMU: no sensor reports in the last 3s — link dropped mid-session, still retrying");
        }
    }
}

bool imuInit() {
    Serial.println("Initializing IMU...");

    imuSpi.begin(PIN_IMU_SCK, PIN_IMU_MISO, PIN_IMU_MOSI, PIN_IMU_CS);

    // Tried bumping back to the BNO08x's 3MHz ceiling after resoldering
    // INT (RUN0012 had run cleanly at 400kHz) — it broke again at 3MHz, so
    // the joint is still marginal enough that clock speed matters after
    // all. Staying at 400kHz until the wiring itself gets more robust
    // (shorter leads / a proper header instead of loose jumpers, or a
    // deliberate external pull-up on INT).
    if (!imu.beginSPI(PIN_IMU_CS, PIN_IMU_INT, PIN_IMU_RST, 400000, imuSpi))
    {
        Serial.println("IMU not found!");
        return false;
    }

    enableReports();

    xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, 1, nullptr, 0);
    taskStarted = true;

    Serial.println("IMU Ready!");
    return true;
}

bool imuSleep() {
    if (!taskStarted)
        return false;

    sleepRequested = true;

    uint32_t t0 = millis();
    while (!sleeping && millis() - t0 < 1000)
        delay(5);

    return sleeping && sleepAccepted;
}

float imuPitch() {
    return cachedPitch;
}

float imuRoll() {
    return cachedRoll;
}

float imuYaw() {
    return cachedYaw;
}

float imuAccelX() {
    return cachedAccelX;
}

float imuAccelY() {
    return cachedAccelY;
}

float imuAccelZ() {
    return cachedAccelZ;
}

// ─── Zero IMU (mount correction) — see imu.h ───────────────────────────────

constexpr float RAD_TO_DEG_F = 57.29577951f;

// Gravity-up direction in sensor axes from ZYX Euler angles (third row of
// the sensor→world rotation matrix). Independent of yaw, which is why the
// .CSV/.CSVB not carrying yaw doesn't matter for any of this.
static void gravityUpFromEuler(float pitchRad, float rollRad, float out[3]) {
    float cp = cosf(pitchRad);
    out[0] = -sinf(pitchRad);
    out[1] = cp * sinf(rollRad);
    out[2] = cp * cosf(rollRad);
}

// Bike-frame basis for a rest reference: forward is the sensor's x axis
// projected onto the horizontal plane (the master's tilt is about its own y
// axis, so x is what tells forward from sideways), up is the reference
// itself, left = up × forward. Returns false if the reference is unusable or
// x is so close to vertical that "forward" is ill-defined.
static bool bikeBasis(const float upIn[3], float f[3], float l[3], float u[3]) {
    float n = sqrtf(upIn[0] * upIn[0] + upIn[1] * upIn[1] + upIn[2] * upIn[2]);
    if (n < 0.5f)
        return false;  // never zeroed (all zeros)

    for (int i = 0; i < 3; i++)
        u[i] = upIn[i] / n;

    // forward = x − (x·up)·up, with x = (1, 0, 0)
    f[0] = 1.0f - u[0] * u[0];
    f[1] = -u[0] * u[1];
    f[2] = -u[0] * u[2];

    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 0.2f)
        return false;  // x within ~11° of vertical

    for (int i = 0; i < 3; i++)
        f[i] /= fl;

    l[0] = u[1] * f[2] - u[2] * f[1];
    l[1] = u[2] * f[0] - u[0] * f[2];
    l[2] = u[0] * f[1] - u[1] * f[0];
    return true;
}

bool imuBikeAngles(float pitchRad, float rollRad, const float up[3],
                   float& leanDeg, float& pitchUpDeg) {
    float f[3], l[3], u[3];
    if (!bikeBasis(up, f, l, u))
        return false;

    float g[3];
    gravityUpFromEuler(pitchRad, rollRad, g);

    auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    leanDeg = atan2f(dot(g, l), dot(g, u)) * RAD_TO_DEG_F;

    float s = dot(g, f);
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    pitchUpDeg = asinf(s) * RAD_TO_DEG_F;
    return true;
}

ImuRestReading imuMeasureRest(uint32_t durationMs) {
    ImuRestReading r{};
    r.ok = false;
    r.error = "";

    // Welford's running mean/variance per component — no need to keep the
    // samples, and the spread of the direction is all that's needed to tell
    // "held still" from "being moved around".
    constexpr uint32_t STEP_MS = 10;
    double mean[3] = {0, 0, 0}, m2[3] = {0, 0, 0}, accSum = 0;
    int n = 0;

    for (uint32_t t = 0; t < durationMs; t += STEP_MS) {
        float g[3];
        gravityUpFromEuler(cachedPitch, cachedRoll, g);

        n++;
        for (int i = 0; i < 3; i++) {
            double d = g[i] - mean[i];
            mean[i] += d / n;
            m2[i] += d * (g[i] - mean[i]);
        }

        accSum += sqrtf(cachedAccelX * cachedAccelX + cachedAccelY * cachedAccelY + cachedAccelZ * cachedAccelZ);

        delay(STEP_MS);
        esp_task_wdt_reset();  // called from the web handler on loop()'s watchdog-monitored task
    }

    if (n == 0) {
        r.error = "No samples";
        return r;
    }

    // At rest the accelerometer reads ~1g. Anything far from it means the
    // sensor isn't reporting (cached values still at their zero init) or is
    // being shaken.
    float accMean = (float)(accSum / n);
    if (accMean < 9.0f || accMean > 10.6f) {
        r.error = "IMU not responding or not still (accelerometer magnitude is not ~1g)";
        return r;
    }

    float wobbleRad = sqrtf((float)((m2[0] + m2[1] + m2[2]) / n));
    r.wobbleDeg = wobbleRad * RAD_TO_DEG_F;

    if (r.wobbleDeg > 0.75f) {
        r.error = "Bike moved during the capture — hold it still and try again";
        return r;
    }

    float len = sqrtf((float)(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]));
    for (int i = 0; i < 3; i++)
        r.up[i] = (float)(mean[i] / len);

    float f[3], l[3], u[3];
    if (!bikeBasis(r.up, f, l, u)) {
        r.error = "IMU x axis is almost vertical — forward direction can't be determined";
        return r;
    }

    r.mountTiltDeg = asinf(r.up[0]) * RAD_TO_DEG_F;
    r.ok = true;
    return r;
}
