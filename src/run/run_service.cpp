// The recorder: starting and stopping a run (timed runs included), the live
// stats, the run's .JSON. The writing itself is run_writer.cpp; the runs
// already on the card are run_files.cpp.
#include <cmath>
#include <ArduinoJson.h>
#include "run_service.h"
#include "run_files.h"
#include "run_writer.h"
#include "storage/storage.h"
#include "system/system.h"
#include "system/settings.h"
#include "system/clock.h"
#include "system/version.h"
#include "suspension/suspension.h"
#include "imu/imu.h"
#include "power/battery.h"

static String currentRun;
static uint32_t runStartMs = 0;

// The writer's last successful write (RunWriter::lastWriteMs()) further back
// than this means it is stuck (see lastWriteSuccessMs in run_writer.cpp).
// Latched for the run's .JSON: see isWriteStalled().
static bool writeEverStalledThisRun = false;
constexpr uint32_t WRITE_STALL_THRESHOLD_MS = 3000;

// The battery over the run, for its .JSON: whether a low battery makes the
// SD card or the IMU misbehave (2026-10-18, the rider's question — a ride on a
// nearly full battery had no SD failure where one at half charge had
// several). The voltage at the start and the lowest 200ms burst seen while
// recording (see batteryBurstVoltage()): bursts come every 3s, so a sag of
// a few milliseconds is out of reach.
static float batteryAtStart = 0;
static float batteryLowest = 0;

// Why the run ended, when it wasn't the rider: written as "stopped_by".
static const char* stopReason = nullptr;

// imuGetHealth().resets counts from boot; only the resets inside this run are
// interesting, so the count at start() is subtracted at stop(). Each reset
// wipes the enabled reports and costs ~200ms of blank pitch/roll/accel, so a
// run with several has visibly patchy IMU columns.
static uint32_t imuResetsAtStart = 0;

// Summary stats accumulated one sample at a time as the run happens
// (mirrors run.html's computeStats(), so its top-of-page numbers can come
// straight from the run's own .JSON instead of downloading and re-parsing
// the whole CSV on every page open). Deliberately limited to plain
// running sums/max — nothing here needs a second pass or a ride-relative
// threshold, unlike the Analysis tab's percentile-based event detection,
// which stays client-side.
struct RunStatsAccumulator {
    uint32_t count = 0;
    float sumFront = 0, sumRear = 0, sumSpeed = 0, sumGForce = 0;
    float maxFront = 0, maxRear = 0, maxSpeed = 0;
    float maxLean = 0, maxPitch = 0, maxGForce = 0;
    float minAlt = 1e9f, maxAlt = -1e9f;
    float distanceM = 0;
    bool havePrevGps = false;
    float prevLat = 0, prevLon = 0;
    bool gpsFixSeen = false;

    // Mirrors run.html's Analysis tab "moving time": fraction of samples
    // above a fixed speed threshold, applied to the run's total duration.
    uint32_t movingCount = 0;

    // Bottom-outs per end, counted as telemetry.js's countBottomOuts() does
    // (so Trends can read them here instead of downloading every run): one
    // when the travel reaches BOTTOM_OUT_PCT of the end's travel, and the
    // next only after it has come back under BOTTOM_OUT_RESET_PCT. The two
    // thresholds are set in mm when the run starts — the travel can't
    // change during a run.
    float boFrontAtMM = 0, boFrontResetMM = 0;
    float boRearAtMM = 0, boRearResetMM = 0;
    bool boFrontArmed = true, boRearArmed = true;
    uint16_t boFront = 0, boRear = 0;

    // Mirrors run.html's Analysis tab computeElevation(): a trailing 5-bucket
    // average over GPS altitude (to reject GPS altitude jitter), summing
    // only steps of at least 0.5m between consecutive smoothed values. The
    // ring buffer only needs to hold the last 5 raw bucket altitudes — the
    // average is cheap to recompute from just those.
    float altWindow[5] = {};
    int altWindowHead = 0;
    int altWindowCount = 0;
    bool haveSmoothedAlt = false;
    float lastSmoothedAlt = 0;
    float elevGainM = 0, elevLossM = 0;

    // Each bucket holds the last GPS altitude seen within a ~1s span before
    // being pushed into altWindow above — see ELEV_BUCKET_MS for why a
    // sample-count window isn't enough on its own.
    uint32_t elevBucketStartMs = 0;
    bool haveElevBucket = false;
    float pendingBucketAlt = 0;
};
static RunStatsAccumulator runStats;

constexpr float G_TO_MS2 = 9.80665f;
constexpr float MOVING_SPEED_KMH = 2.0f;
// The dashboard's bottom-out rule (BOTTOM_OUT_PCT / BOTTOM_OUT_RESET_PCT in
// web/telemetry.js): keep them equal — tests/telemetry.test.mjs checks.
constexpr float BOTTOM_OUT_PCT = 95.0f;
constexpr float BOTTOM_OUT_RESET_PCT = 85.0f;
constexpr float ELEV_MIN_STEP_M = 0.5f;
constexpr int ELEV_SMOOTH_WINDOW = 5;

// The sampling loop logs hundreds of times a second — far faster than GPS
// altitude actually updates — so smoothing/threshold-testing every raw row
// covers only a few milliseconds per "window", not the several seconds the
// 0.5m threshold assumes. That diluted a real ~80m descent (RUN0017) down
// to ~12m: almost every consecutive row's altitude was near-identical, so
// nearly every step fell under the noise threshold and got discarded.
// Bucketing to one altitude sample per ~1s of wall-clock time first makes
// the window's timespan — and therefore the threshold — actually meaningful,
// regardless of how fast the loop runs.
constexpr uint32_t ELEV_BUCKET_MS = 1000;

// Pushes one bucketed altitude sample into the smoothing window and
// accumulates gain/loss against the previous smoothed value.
static void pushElevationBucket(float alt) {
    runStats.altWindow[runStats.altWindowHead % ELEV_SMOOTH_WINDOW] = alt;
    runStats.altWindowHead++;
    if (runStats.altWindowCount < ELEV_SMOOTH_WINDOW) runStats.altWindowCount++;

    float sum = 0;
    for (int i = 0; i < runStats.altWindowCount; i++) sum += runStats.altWindow[i];
    float smoothedAlt = sum / runStats.altWindowCount;

    if (runStats.haveSmoothedAlt) {
        float d = smoothedAlt - runStats.lastSmoothedAlt;
        if (fabsf(d) >= ELEV_MIN_STEP_M) {
            if (d > 0) runStats.elevGainM += d;
            else runStats.elevLossM += -d;
        }
    }
    runStats.lastSmoothedAlt = smoothedAlt;
    runStats.haveSmoothedAlt = true;
}

static float haversineMeters(float lat1, float lon1, float lat2, float lon2) {
    constexpr float R = 6371000.0f;
    constexpr float toRad = 3.14159265f / 180.0f;

    float dLat = (lat2 - lat1) * toRad;
    float dLon = (lon2 - lon1) * toRad;
    float a = sinf(dLat / 2) * sinf(dLat / 2) +
        cosf(lat1 * toRad) * cosf(lat2 * toRad) * sinf(dLon / 2) * sinf(dLon / 2);

    return 2 * R * asinf(sqrtf(a));
}

void RunService::init() {
    RunWriter::init();
}

static void writeRunMeta(float durationSeconds) {
    // The last elevation bucket only gets pushed into the smoothing window
    // once a sample arrives *after* it's a full second old — the final
    // bucket of the run never gets that follow-up sample, so flush it here
    // or its last ~1s of altitude change goes uncounted.
    if (runStats.haveElevBucket) {
        pushElevationBucket(runStats.pendingBucketAlt);
        runStats.haveElevBucket = false;
    }

    File csv = storageGet(csvPath(currentRun).c_str());

    size_t csvSize = csv ? csv.size() : 0;

    storageClose(csv);

    File f = storageOpen(jsonPath(currentRun).c_str());

    if (!f)
        return;

    JsonDocument doc;

    doc["name"] = currentRun;
    doc["duration_s"] = serialized(String(durationSeconds, 1));

    // Worked back from "now" rather than captured at start(): the clock is
    // often set partway through a ride (GPS time arriving once there's sky),
    // and this way such a run still gets its date. Left out entirely when
    // the device never learned the time.
    if (clockIsSet())
        doc["start_utc"] = clockNow() - (uint32_t)durationSeconds;

    doc["csv_size"] = csvSize;

    // Which code recorded this run. Not written for a recovered run (see
    // recoverOrphanedRun()): the firmware doing the recovering is not
    // necessarily the one that was recording.
    doc["firmware"] = FIRMWARE_VERSION;
    doc["firmware_build"] = FIRMWARE_BUILD;

    RunWriter::Report written = RunWriter::report();

    doc["incomplete"] = writeEverStalledThisRun || written.failed;
    doc["no_gps"] = !runStats.gpsFixSeen;
    addImuReference(doc);

    // How sure the IMU was of its own orientation, second by second (see
    // imuDiagStart()) — recorded to find out why it loses it on rough ground.
    imuDiagToJson(doc["imu_diag"].to<JsonObject>());

    // Volts. Only in the .JSON: nothing on the dashboard reads it.
    if (batteryAtStart > 0) {
        JsonObject battery = doc["battery_v"].to<JsonObject>();
        battery["start"] = serialized(String(batteryAtStart, 2));
        battery["end"] = serialized(String(readBatteryVoltage(), 2));
        if (batteryLowest > 0) battery["lowest"] = serialized(String(batteryLowest, 2));
    }

    if (stopReason) doc["stopped_by"] = stopReason;

    // SD link health for this run — nonzero means the card hiccuped even
    // if nothing was lost (retry_ok: a second flush fixed it; recovered:
    // file reopened and buffered rows replayed).
    doc["sd_retry_ok"] = written.sdRetryOk;
    doc["sd_recovered"] = written.sdRecovered;
    doc["sd_last_errno"] = written.sdLastErrno;
    doc["sd_last_gap_bytes"] = written.sdLastGapBytes;

    // How the recoveries went: the longest one, and how many attempts failed
    // at each step (see tryRecover()). An ordinary recovery works first time
    // and takes a few ms.
    doc["sd_recover_max_ms"] = written.sdRecoverMaxMs;
    doc["sd_reopen_failed"] = written.sdReopenFailed;
    doc["sd_stat_failed"] = written.sdStatFailed;
    doc["sd_replay_failed"] = written.sdReplayFailed;

    // BNO08x link health during this run: how many times the sensor reset
    // (each one blanks the IMU columns for ~200ms) and whether it was still
    // streaming when the run ended.
    ImuHealth imuHealth = imuGetHealth();
    // Samples that never reached the writer (queue full during an SD
    // recovery), and samples the writer took but never got onto the card (see
    // rowsLost). Unlike the sd_* counters these are gone for good: together
    // they are what the run is missing. `incomplete` above covers rows_lost
    // and not rows_dropped.
    doc["rows_dropped"] = written.rowsDropped;
    doc["rows_lost"] = written.rowsLost;
    doc["rows_written"] = runStats.count - written.rowsDropped - written.rowsLost;

    // The spill buffer this run had behind the queue (0: none, the radio
    // stayed on or the heap had no room), the most it held at once, and the
    // free heap once it was taken — to see whether it was needed, and how
    // much room it left.
    doc["spill_rows"] = written.spillRows;
    doc["spill_max"] = written.spillMax;
    doc["heap_free_recording"] = written.heapFreeRecording;

    doc["imu_resets"] = imuHealth.resets - imuResetsAtStart;
    doc["imu_streaming"] = imuHealth.streaming;

    // Summary numbers accumulated sample-by-sample during the run (see
    // RunStatsAccumulator above) — lets run.html paint its
    // top-of-page stats straight from this small file instead of
    // downloading and re-parsing the entire CSV first. count==0 (e.g. a
    // run stopped before a single sample logged) leaves this out entirely
    // rather than writing zeros that would look like a real, tiny ride.
    if (runStats.count > 0) {
        JsonObject stats = doc["stats"].to<JsonObject>();
        float n = (float)runStats.count;

        stats["max_front_mm"] = serialized(String(runStats.maxFront, 1));
        stats["max_rear_mm"] = serialized(String(runStats.maxRear, 1));
        stats["avg_front_mm"] = serialized(String(runStats.sumFront / n, 1));
        stats["avg_rear_mm"] = serialized(String(runStats.sumRear / n, 1));
        stats["max_speed_kmh"] = serialized(String(runStats.maxSpeed, 1));
        stats["avg_speed_kmh"] = serialized(String(runStats.sumSpeed / n, 1));
        // Named "_deg" but actually radians (imuRoll()/imuPitch() are never
        // converted) — kept as-is so existing runs' .JSON files stay
        // consistent; run.html's statsFromMeta() converts on read.
        stats["max_lean_deg"] = serialized(String(runStats.maxLean, 1));
        stats["max_pitch_deg"] = serialized(String(runStats.maxPitch, 1));
        stats["max_g"] = serialized(String(runStats.maxGForce, 2));
        stats["avg_g"] = serialized(String(runStats.sumGForce / n, 2));
        stats["min_alt_m"] = serialized(String(runStats.minAlt, 1));
        stats["max_alt_m"] = serialized(String(runStats.maxAlt, 1));
        stats["distance_km"] = serialized(String(runStats.distanceM / 1000.0f, 2));
        stats["moving_s"] = serialized(String(durationSeconds * (runStats.movingCount / n), 1));
        stats["elev_gain_m"] = serialized(String(runStats.elevGainM, 1));
        stats["elev_loss_m"] = serialized(String(runStats.elevLossM, 1));
        stats["bottom_outs_front"] = runStats.boFront;
        stats["bottom_outs_rear"] = runStats.boRear;
    }

    // Everything about the bike itself (as opposed to this specific run)
    // lives under "bike", recorded as-is from Settings at the moment the
    // run ended — keeps the two kinds of data from running together.
    JsonObject bike = doc["bike"].to<JsonObject>();
    bike["name"] = settings.bikeName;
    bike["wheel_size"] = settings.wheelSize;
    bike["full_suspension"] = settings.fullSuspension;
    bike["front_travel"] = serialized(String(suspensionGetMaxTravelMM(FRONT), 1));
    bike["rear_travel"] = serialized(String(suspensionGetMaxTravelMM(REAR), 1));
    bike["rear_wheel_travel_mm"] = serialized(String(settings.rearWheelTravelMM, 1));

    JsonObject frontSetup = bike["front_suspension"].to<JsonObject>();
    if (settings.frontModel.length()) frontSetup["model"] = settings.frontModel;
    frontSetup["lsc"] = settings.frontLSC;
    frontSetup["hsc"] = settings.frontHSC;
    frontSetup["lsr"] = settings.frontLSR;
    frontSetup["hsr"] = settings.frontHSR;
    frontSetup["volume_tokens"] = settings.frontVolumeTokens;
    frontSetup["pressure_psi"] = serialized(String(settings.frontPressurePsi, 0));
    frontSetup["sag_pct"] = serialized(String(settings.frontSagPct, 1));
    frontSetup["has_lsc"] = settings.frontHasLSC;
    frontSetup["has_hsc"] = settings.frontHasHSC;
    frontSetup["has_lsr"] = settings.frontHasLSR;
    frontSetup["has_hsr"] = settings.frontHasHSR;

    JsonObject rearSetup = bike["rear_suspension"].to<JsonObject>();
    if (settings.rearModel.length()) rearSetup["model"] = settings.rearModel;
    rearSetup["lsc"] = settings.rearLSC;
    rearSetup["hsc"] = settings.rearHSC;
    rearSetup["lsr"] = settings.rearLSR;
    rearSetup["hsr"] = settings.rearHSR;
    rearSetup["volume_tokens"] = settings.rearVolumeTokens;
    rearSetup["pressure_psi"] = serialized(String(settings.rearPressurePsi, 0));
    rearSetup["sag_pct"] = serialized(String(settings.rearSagPct, 1));
    rearSetup["has_lsc"] = settings.rearHasLSC;
    rearSetup["has_hsc"] = settings.rearHasHSC;
    rearSetup["has_lsr"] = settings.rearHasLSR;
    rearSetup["has_hsr"] = settings.rearHasHSR;

    serializeJson(doc, f);

    storageClose(f);
}

// See startTimed(). Cleared by every start and every stop, so only a run
// started through startTimed() ever has it set.
static bool autoStopArmed = false;
static uint32_t autoStopAtMs = 0;

bool RunService::startTimed(uint32_t seconds) {
    // start() answers true for a run already in progress; a timed run has to
    // be one of its own, or its timer would cut someone else's ride short.
    if (getState() != READY)
        return false;

    if (!start())
        return false;

    autoStopAtMs = millis() + seconds * 1000UL;
    autoStopArmed = true;
    return true;
}

// A start asked for over the network (see startSoon()). The run begins by
// taking the WiFi radio down, so the answer to the request that asked for it
// has to be on its way to the browser first; this is how long it gets. It
// was the delay before the radio was shut down *during* the run, back when
// the run started at once and the shutdown left a hole in it.
constexpr uint32_t NETWORK_START_DELAY_MS = 500;

static bool startPending = false;
static uint32_t startAtMs = 0;
static uint32_t pendingTimedSeconds = 0;    // 0 = an ordinary, open-ended run
static String pendingName;

static String runName(int number) {
    char name[16];
    sprintf(name, "RUN%04d", number);
    return name;
}

String RunService::startSoon(uint32_t timedSeconds) {
    if (getState() != READY)
        return "";

    // What start() will call it, barring another run appearing on the card
    // in the next half second.
    pendingName = runName(nextRunNumber());
    pendingTimedSeconds = timedSeconds;
    startAtMs = millis() + NETWORK_START_DELAY_MS;
    startPending = true;

    return pendingName;
}

bool RunService::startIsPending() {
    return startPending;
}

void RunService::cancelPendingStart() {
    startPending = false;
}

void RunService::update() {
    if (startPending && (int32_t)(millis() - startAtMs) >= 0) {
        uint32_t seconds = pendingTimedSeconds;
        startPending = false;

        if (seconds > 0) startTimed(seconds);
        else start();
    }

    if (autoStopArmed && (int32_t)(millis() - autoStopAtMs) >= 0)
        stop();

    if (getState() == RECORDING) {
        float v = batteryBurstVoltage();
        if (v > 0 && (batteryLowest == 0 || v < batteryLowest)) batteryLowest = v;
    }
}

bool RunService::start() {
    if (getState() == RECORDING)
        return true;

    autoStopArmed = false;

    // A press of the button while a network start was still waiting starts
    // the run now; the waiting one is dropped.
    startPending = false;

    // Before anything touches the card: the WiFi radio goes down first (when
    // the settings say so), which takes ~100ms. Done once the run had already
    // begun, that was 100ms of every recording with no rows in it.
    recordingPrepare();

    currentRun = runName(nextRunNumber());

    if (!RunWriter::open(currentRun))
    {
        currentRun = "";
        recordingRelease();
        return false;
    }

    imuResetsAtStart = imuGetHealth().resets;

    toggleRecording();

    runStartMs = millis();
    writeEverStalledThisRun = false;
    runStats = RunStatsAccumulator{};
    runStats.boFrontAtMM = suspensionGetMaxTravelMM(FRONT) * BOTTOM_OUT_PCT / 100.0f;
    runStats.boFrontResetMM = suspensionGetMaxTravelMM(FRONT) * BOTTOM_OUT_RESET_PCT / 100.0f;
    runStats.boRearAtMM = suspensionGetMaxTravelMM(REAR) * BOTTOM_OUT_PCT / 100.0f;
    runStats.boRearResetMM = suspensionGetMaxTravelMM(REAR) * BOTTOM_OUT_RESET_PCT / 100.0f;
    imuDiagStart();
    batteryAtStart = readBatteryVoltage();
    batteryLowest = 0;
    stopReason = nullptr;

    // Last, after everything else the run takes from the heap: the radio is
    // down by now (recordingPrepare() above), and its memory free. No row
    // reaches log() before this returns — both run on loop()'s task.
    RunWriter::takeSpill();

    Serial.println("Run started: " + currentRun);
    return true;
}

void RunService::stop() {
    autoStopArmed = false;

    if (getState() != RECORDING)
        return;

    RunWriter::drain();
    imuDiagStop();
    RunWriter::close();

    float durationSeconds =
        (millis() - runStartMs) / 1000.0f;

    writeRunMeta(durationSeconds);
    writeCompactCsv(currentRun);

    toggleRecording();

    Serial.println("Run stopped: " + currentRun);

    uint32_t rowsDropped = RunWriter::report().rowsDropped;
    if (rowsDropped > 0)
        Serial.printf("Rows: %u dropped of %u sampled (%.1f%% lost — writer queue full)\n",
                      (unsigned)rowsDropped, (unsigned)runStats.count,
                      100.0f * rowsDropped / (float)runStats.count);

    currentRun = "";
}

void RunService::stopForLowBattery() {
    stopReason = "low_battery";
    stop();
}

void RunService::log(
    uint32_t t,
    float lat,
    float lon,
    float speed,
    int sats,
    float hdop,
    float alt,
    float pitch,
    float roll,
    float yaw,
    float ax,
    float ay,
    float az,
    float front_travel,
    float rear_travel) {
    // Only the recording state decides whether a row is taken — not the
    // state of csvFile, which this used to test as well. The file belongs to
    // the writer task, and while that task recovers from a failed write it
    // closes and reopens it: for those few milliseconds the handle is
    // invalid, and every row sampled in them was thrown away right here,
    // before the queue, counted nowhere (1 to 20 rows per recovery, found
    // 2026-10-07 by comparing rows sampled with rows written). Rows that
    // arrive during a recovery are queued like any others and the writer
    // deals with them.
    if (getState() != RECORDING)
        return;

    LogRow row;
    row.isDrainMarker = false;
    row.t = t;
    row.lat = lat;
    row.lon = lon;
    row.speed = speed;
    row.sats = sats;
    row.hdop = hdop;
    row.alt = alt;
    row.pitch = pitch;
    row.roll = roll;
    row.yaw = yaw;
    row.ax = ax;
    row.ay = ay;
    row.az = az;
    row.front_travel = front_travel;
    row.rear_travel = rear_travel;

    if (front_travel > runStats.maxFront) runStats.maxFront = front_travel;
    if (rear_travel > runStats.maxRear) runStats.maxRear = rear_travel;
    if (speed > runStats.maxSpeed) runStats.maxSpeed = speed;
    runStats.sumFront += front_travel;
    runStats.sumRear += rear_travel;
    runStats.sumSpeed += speed;

    // A travel of 0 mm (no calibration) never counts: the thresholds are 0 too.
    if (runStats.boFrontAtMM > 0) {
        if (runStats.boFrontArmed && front_travel >= runStats.boFrontAtMM) { runStats.boFront++; runStats.boFrontArmed = false; }
        else if (!runStats.boFrontArmed && front_travel < runStats.boFrontResetMM) runStats.boFrontArmed = true;
    }
    if (runStats.boRearAtMM > 0) {
        if (runStats.boRearArmed && rear_travel >= runStats.boRearAtMM) { runStats.boRear++; runStats.boRearArmed = false; }
        else if (!runStats.boRearArmed && rear_travel < runStats.boRearResetMM) runStats.boRearArmed = true;
    }

    if (alt < runStats.minAlt) runStats.minAlt = alt;
    if (alt > runStats.maxAlt) runStats.maxAlt = alt;

    float leanAbs = fabsf(roll);
    if (leanAbs > runStats.maxLean) runStats.maxLean = leanAbs;
    float pitchAbs = fabsf(pitch);
    if (pitchAbs > runStats.maxPitch) runStats.maxPitch = pitchAbs;

    float gForce = sqrtf(ax * ax + ay * ay + az * az) / G_TO_MS2;
    if (gForce > runStats.maxGForce) runStats.maxGForce = gForce;
    runStats.sumGForce += gForce;

    if (lat != 0 && lon != 0) {
        runStats.gpsFixSeen = true;

        // Only when the position has moved: the module reports a new one a
        // few times a second and rows are logged 500 times a second, so all
        // but a handful of these would be the distance from a point to
        // itself — six trig calls a row to add zero.
        if (!runStats.havePrevGps || lat != runStats.prevLat || lon != runStats.prevLon) {
            if (runStats.havePrevGps)
                runStats.distanceM += haversineMeters(runStats.prevLat, runStats.prevLon, lat, lon);
            runStats.prevLat = lat;
            runStats.prevLon = lon;
            runStats.havePrevGps = true;
        }

        if (!runStats.haveElevBucket) {
            runStats.elevBucketStartMs = t;
            runStats.haveElevBucket = true;
        } else if (t - runStats.elevBucketStartMs >= ELEV_BUCKET_MS) {
            pushElevationBucket(runStats.pendingBucketAlt);
            runStats.elevBucketStartMs = t;
        }
        runStats.pendingBucketAlt = alt;
    }

    if (speed > MOVING_SPEED_KMH) runStats.movingCount++;

    runStats.count++;

    // Never blocks: see RunWriter::push().
    RunWriter::push(row);
}

String RunService::currentName() {
    return startPending ? pendingName : currentRun;
}

uint32_t RunService::elapsedSeconds() {
    if (getState() != RECORDING)
        return 0;

    return (millis() - runStartMs) / 1000;
}

bool RunService::isWriteStalled() {
    if (getState() != RECORDING)
        return false;

    bool stalled = (millis() - RunWriter::lastWriteMs()) > WRITE_STALL_THRESHOLD_MS;

    // Latched, not just reported: once a run has stalled, it stays
    // flagged "incomplete" in its .JSON even if this happens to be
    // called again after the queue somehow drained (it won't, in
    // practice, if the writer task is truly stuck — but this is the
    // single source of truth for that flag either way).
    if (stalled)
        writeEverStalledThisRun = true;

    // A detected (and possibly recovered) SD write failure keeps the LED
    // alert on for the rest of the run: rows were lost, so the rider
    // should know this run is compromised rather than find out at the desk.
    return stalled || RunWriter::failed();
}