#include <vector>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <sys/stat.h>
#include <ff.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "run_service.h"
#include "storage/storage.h"
#include "system/system.h"
#include "system/settings.h"
#include "system/clock.h"
#include "system/version.h"
#include "suspension/suspension.h"
#include "imu/imu.h"

static File csvFile;
static String currentRun;
static bool recording = false;
static uint32_t lastFlush = 0;
static uint32_t runStartMs = 0;

// Tried at 500ms (2026-09-26) to test whether the sync *rate* was what the card
// couldn't take — experiment 3 in summary.md — and the answer is no, so this is
// back at 100ms. RUN0037 (28s at 500ms, ~60 syncs) still had 3 failures where
// 100ms (~300 syncs) gives 4-6: the count barely moved while the per-sync
// failure rate went *up*, from ~1.7% to ~5%. Whatever this is tracks how much
// data a sync has to commit, not how often syncs happen.
//
// The longer interval was also strictly worse to live with: each failure put
// ~12KB at risk instead of ~2KB, and the slower recovery overflowed the pending
// buffer, so RUN0037 came back `incomplete` with 134 duplicated rows.
constexpr uint32_t RUN_FLUSH_INTERVAL_MS = 100;

// Field-observed failure mode: a hard impact jarred the SD card's contact
// just enough that a single storageFlush() call inside writeLogRow()
// never returned — the writer task froze there forever, silently
// dropping every sample for the rest of that ride (recording state and
// the button both kept working fine; only the SD side was actually
// dead). lastWriteSuccessMs only advances once writeLogRow() fully
// returns, so it stops moving exactly when a hang like that starts —
// isWriteStalled() below compares against it to notice from loop() (an
// entirely separate task/core, unaffected by the hang) that something is
// seriously wrong, well beyond any stall the queue is meant to absorb.
static volatile uint32_t lastWriteSuccessMs = 0;
static bool writeEverStalledThisRun = false;
constexpr uint32_t WRITE_STALL_THRESHOLD_MS = 3000;

// Second field-observed failure mode, and a quieter one: RUN0019 ran for
// 20.7s but its .CSV stopped growing at 9KB (~0.4s of rows), ending
// mid-row. Nothing hung — writeLogRow() kept returning, so the stall check
// above never fired. The ESP32 Arduino File wrapper buffers writes in
// stdio and its flush() returns void, so once one SD write fails (FatFS
// latches the error on the open handle) printf() keeps "succeeding" into
// the buffer while nothing reaches the card. The only visible symptom is
// that the file's on-disk size stops tracking what was written, which is
// what writeLogRow() checks after every flush.
//
// Field data after the first fix (RUN0021/RUN0022, SD at 10MHz) showed the
// failures are real and recur 2-3 times per ~18s run, each one silently
// losing the ~120ms of rows sitting in the buffer. So recovery no longer
// just reopens the file: the writer keeps every row written since the last
// confirmed flush and replays them after reopening (see tryRecover()), and
// the counters below are written into the run's .JSON so the failure rate
// and errno can be read back without a serial cable.
static volatile bool writeFailedThisRun = false;  // rows were genuinely lost
static bool needsRecovery = false;    // a sync failed; file must be reopened
static size_t csvBytesAccepted = 0;   // what the file should hold once synced
static size_t confirmedBytes = 0;     // what it held at the last flush known to have reached the card
static bool confirmedMidRow = false;  // ... and whether that flush ended in the middle of a row (see tryRecover())
static uint32_t nextReopenAttemptMs = 0;
// Was 1000ms on the reasoning that there is nothing useful to do faster than
// that. There is: rows keep arriving into `pending` the whole time a recovery
// is being retried, so every extra second of waiting is another ~200 rows of
// buffer. RUN0037 needed two retries, piled up past the buffer and lost rows
// (the run's only genuinely lost data, and its `incomplete` flag). A reopen
// costs a few ms, so retrying sooner is nearly free.
constexpr uint32_t REOPEN_RETRY_MS = 150;
static uint32_t sdFlushRetryOk = 0;   // second flush fixed it, nothing lost
static uint32_t sdRecoveries = 0;     // reopen + replay succeeded
static int sdLastErrno = 0;
static uint32_t sdLastGapBytes = 0;   // accepted - on disk at the last failure

// How the recoveries went, for the run's .JSON. The field test of 2026-10-10
// had two that outlasted the `pending` buffer and nothing on record to say
// how long they took or what kept failing: the attempts print to serial, and
// there is no serial cable on a trail.
static uint32_t recoveryStartMs = 0;  // when the sync that started this one failed
static uint32_t sdRecoverMaxMs = 0;   // the longest, from failed sync to rows on the card
static uint32_t sdReopenFailed = 0;   // attempts that could not reopen the file
static uint32_t sdStatFailed = 0;     // ... reopened it, but could not stat it
static uint32_t sdReplayFailed = 0;   // ... replayed the rows, and they did not reach the card

// Rows the writer took and never got onto the card: `pending` was already
// full when they arrived, or the card was still away when the run ended.
// With rowsDropped (below) this is everything a run is missing.
static volatile uint32_t rowsLost = 0;

// Set by stop() while it waits for the writer to finish. See sdWriterTask().
static volatile bool stopping = false;

// A 3.3V rail monitor on GPIO36 (its own 1kHz task, a divider at the SD card,
// and rail_min_v / rail_min_v_at_failure / rail_failures[] in the run .JSON)
// was fitted here on 2026-09-26 to test whether these sync failures coincided
// with a voltage sag. Across RUN0023-RUN0041 they never did: the rail read
// 3.36-3.40V at every single failure, against a 3.30-3.36V floor for the run as
// a whole. Removed once that was settled, so the enclosure could close again.
// Don't re-fit it without a new reason — the open lead is the VFS/FatFS layer
// (errno 0 with exactly one stdio buffer unconfirmed), not the power rail.
// imuGetHealth().resets counts from boot; only the resets inside this run are
// interesting, so the count at start() is subtracted at stop(). Each reset
// wipes the enabled reports and costs ~200ms of blank pitch/roll/accel, so a
// run with several has visibly patchy IMU columns.
static uint32_t imuResetsAtStart = 0;

// Samples the sampling loop produced but could not hand to the writer because
// the queue was full — real, permanently missing rows. Written from loop()
// (core 1), read at stop().
static volatile uint32_t rowsDropped = 0;

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

// Diagnostic columns added while chasing field-observed 140-400ms gaps
// between samples that coincided with hard impacts. write_us is the total
// time spent writing this row (the printf plus, if one happened, the
// flush, now measured on the SD-writer task below); flush_us is just the
// flush portion, 0 if no flush ran this row. This confirmed the periodic
// SD flush (11-20ms+) was blocking loop() and clipping fast suspension
// peaks — see the log()/sdWriterTask() split below, which moves the SD
// writing off the sampling core entirely. Kept for now to verify the fix:
// once a field run shows no more timestamp_ms gaps around flush_us spikes,
// these can go. parseCSV() in run.html only requires >=15 columns, so
// extra trailing ones are already safely ignored by the existing
// chart/stat code without any changes there.
static constexpr const char* CSV_HEADER =
"timestamp_ms,lat,lon,speed,sats,hdop,alt,pitch,roll,yaw,ax,ay,az,front_travel,rear_travel,write_us,flush_us";

// ─── SD writer task ────────────────────────────────────────────────────────
// The sampling loop (core 1) must never block on SD I/O — a periodic
// flush() takes 10-20ms+ at the current SPI clock, and a stall that long
// right as a bump peaks is exactly what was clipping recorded suspension
// travel below the real value. Instead, log() just copies the row into a
// queue (a few microseconds) and this dedicated task, pinned to core 0,
// does the actual printf()/flush() — real SD blocking still happens, it
// just no longer steals time from sampling. Only this task (plus start()/
// stop() around it, synchronized below) ever touches csvFile.
struct LogRow {
    bool isDrainMarker;

    uint32_t t;
    float lat, lon, speed;
    int sats;
    float hdop, alt, pitch, roll, yaw;
    float ax, ay, az;
    float front_travel, rear_travel;
};

// How far the SD writer may fall behind before rows are dropped (and counted,
// see rowsDropped): 0.9s at 500 rows/s. It was 256 rows, 0.51s, until a bench
// run on 2026-10-11 dropped 24: the card took 290ms over one write and 357ms
// over the next, with nothing wrong — it does that at the start of a file
// and every megabyte or so. The rows came from `pending` below, which no
// longer needs them (see sdWriterTask()): the two are 640 rows, 40KB, as
// before, from the same heap WiFi needs.
static constexpr int LOG_QUEUE_DEPTH = 448;
static QueueHandle_t logQueue = nullptr;
static SemaphoreHandle_t drainSem = nullptr;
static TaskHandle_t sdWriterTaskHandle = nullptr;

static String csvPath(const String& run);

// Rows written since the last flush that was confirmed to have reached the
// card (the row the flush happened on is excluded: its first half is what
// that flush committed). If a later sync fails, exactly these rows are the
// ones sitting in the dead handle's buffer, so they're what tryRecover()
// replays. If it ever overflows (card gone for seconds) the excess is a real,
// permanent gap and the run is flagged.
//
// Sized for RUN_FLUSH_INTERVAL_MS with room for a rate-limited tryRecover()
// retry on top: at 500 rows/s a flush window is 50 rows and a retry interval
// 75, against 192. It need not cover a long outage by itself any more — once
// it is full the rows wait in the queue (see sdWriterTask()), so what a
// recovery has to play with is both together, 640 rows. start() warns if the
// configured suspensionSampleRateHz could outrun it.
//
// On the heap, not a static array: at 64 bytes per row this is 12KB (24KB
// when it held 384 rows) and the static .bss segment had nowhere near that
// left (a static array overflowed
// dram0_0_seg at link time, even though the firmware's total RAM figure looks
// comfortable — most of that segment is already spoken for and what remains is
// runtime heap). Allocated once in init(), which runs before WiFi comes up, so
// it gets a contiguous block and is never resized.
//
// Keep this modest: the same heap feeds lwIP's TCP buffers. 768 rows (48KB)
// left so little that every WiFi socket write failed with EAGAIN and the
// dashboard stopped loading — init() now logs the free heap either side of
// this allocation so the margin is visible instead of guessed at.
static constexpr int PENDING_MAX = 192;
static LogRow* pending = nullptr;
static int pendingCapacity = 0;
static int pendingCount = 0;

static void pendingPush(const LogRow& r) {
    if (pendingCount < pendingCapacity) {
        pending[pendingCount++] = r;
    } else {
        writeFailedThisRun = true;
        rowsLost = rowsLost + 1;
    }
}

// First 15 columns of a row (everything up to and including the comma
// before write_us) — the diagnostic pair is appended separately because it
// can only be measured after the flush that follows this call.
// ─── Row formatting ────────────────────────────────────────────────────────
// Built by hand into a buffer instead of with printf("%.2f", ...), because
// that turned out to be the real ceiling on this whole pipeline. Varargs
// promote every float to double, the ESP32 has no double-precision hardware,
// and newlib's dtoa is therefore software emulation: RUN0040 measured a
// *median of 2082us* to format one row's 12 floats (mean 2320us). At the rate
// rows arrive that is ~89% of a core spent on decimal conversion alone, so the
// writer could not keep up and 3077 of that run's 11754 samples (26%) were
// dropped at the queue before ever reaching the card. It also explains the
// long-standing ~250-280 rows/s ceiling and why raising
// suspensionSampleRateHz never bought anything.
//
// Output is byte-for-byte what the printf produced, so CSV_HEADER,
// parseCsvLine() and run.html's parsers are unaffected.
static char* appendU32(char* p, uint32_t v) {
    char tmp[10];
    int n = 0;

    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n--) *p++ = tmp[n];

    return p;
}

// One double multiply + lround per field (a few us) instead of a full dtoa.
// Scaling in double, not float, because a float's 24-bit mantissa cannot hold
// e.g. 41.123456 * 1e6 exactly.
static char* appendFixed(char* p, float v, int decimals) {
    static const int32_t POW10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
    const int32_t scale = POW10[decimals];

    bool negative = v < 0;
    int32_t scaled = (int32_t)lround((double)fabsf(v) * scale);

    if (negative && scaled != 0)
        *p++ = '-';

    p = appendU32(p, (uint32_t)(scaled / scale));

    if (decimals > 0) {
        *p++ = '.';
        int32_t frac = scaled % scale;

        for (int d = decimals - 1; d >= 0; d--) {
            *p++ = (char)('0' + frac / POW10[d]);
            frac %= POW10[d];
        }
    }

    return p;
}

static size_t printRowPrefix(const LogRow& r) {
    char buf[192];
    char* p = buf;

    p = appendU32(p, r.t);                       *p++ = ',';
    p = appendFixed(p, r.lat, 6);                *p++ = ',';
    p = appendFixed(p, r.lon, 6);                *p++ = ',';
    p = appendFixed(p, r.speed, 2);              *p++ = ',';
    p = appendU32(p, (uint32_t)(r.sats < 0 ? 0 : r.sats)); *p++ = ',';
    p = appendFixed(p, r.hdop, 2);               *p++ = ',';
    p = appendFixed(p, r.alt, 2);                *p++ = ',';
    p = appendFixed(p, r.pitch, 2);              *p++ = ',';
    p = appendFixed(p, r.roll, 2);               *p++ = ',';
    p = appendFixed(p, r.yaw, 2);                *p++ = ',';
    p = appendFixed(p, r.ax, 3);                 *p++ = ',';
    p = appendFixed(p, r.ay, 3);                 *p++ = ',';
    p = appendFixed(p, r.az, 3);                 *p++ = ',';
    p = appendFixed(p, r.front_travel, 2);       *p++ = ',';
    p = appendFixed(p, r.rear_travel, 2);        *p++ = ',';

    return csvFile.write((const uint8_t*)buf, p - buf);
}

// The write_us/flush_us pair that closes a row.
static size_t printRowDiagnostics(uint32_t writeUs, uint32_t flushUs) {
    char buf[24];
    char* p = buf;

    p = appendU32(p, writeUs);   *p++ = ',';
    p = appendU32(p, flushUs);   *p++ = '\n';

    return csvFile.write((const uint8_t*)buf, p - buf);
}

// How many bytes of the current run's CSV FatFS has actually committed to the
// directory entry, read with a plain stat() rather than csvFile.size().
//
// csvFile.size() cannot be trusted for this: VFSFileImpl::size() only re-stats
// the file when its _written flag is set (i.e. when something has been written
// since the last size() call), and _getStat() clears that flag. So the *first*
// size() after a write is fresh, and every later one — with nothing but a
// flush() in between — silently returns the previous, stale value. That made
// the retry flush below impossible to credit: the second syncedToDisk() could
// only ever repeat the first one's verdict, so sd_retry_ok was structurally
// always 0 and every single failure escalated to a full reopen + replay. The
// earlier conclusion that "a repeated flush never fixes it" came from this
// broken measurement, so it is not evidence of anything.
static String csvFullPath;   // "/sd" + csvPath(currentRun), set in start()

static bool onDiskSize(size_t& out) {
    struct stat st;

    if (csvFullPath.length() == 0 || stat(csvFullPath.c_str(), &st) != 0)
        return false;

    out = (size_t)st.st_size;
    return true;
}

static bool syncedToDisk() {
    size_t onDisk = 0;

    if (!onDiskSize(onDisk))
        return false;

    return onDisk >= csvBytesAccepted;
}


// Drops the broken handle, reopens the same file in append mode and replays
// the pending rows, so a failed sync costs nothing but a few milliseconds.
// The last committed row may be cut off mid-line (its diagnostic columns
// never made it), so a newline goes first to keep the replayed rows on
// their own lines; replayed rows get 0,0 as their write_us/flush_us.
// Returns true once everything is confirmed on disk. Rate-limited to one
// attempt per REOPEN_RETRY_MS — nothing useful to do faster than that.
static bool tryRecover() {
    // Only ever mid-recording: after stop() currentRun is empty and this
    // would otherwise create a stray "/.CSV".
    if (currentRun.length() == 0)
        return false;

    uint32_t now = millis();

    if (now < nextReopenAttemptMs)
        return false;

    nextReopenAttemptMs = now + REOPEN_RETRY_MS;

    if (csvFile)
        csvFile.close();  // may itself report an error; nothing to do about it

    csvFile = SD.open(csvPath(currentRun).c_str(), FILE_APPEND);

    if (!csvFile) {
        sdReopenFailed++;
        Serial.println("RunService: reopen failed, will retry");
        return false;
    }

    // Freshly reopened, so this reflects what is really on the card — and
    // deliberately the same stat() the checks above use, not csvFile.size().
    size_t reopenedSize = 0;
    if (!onDiskSize(reopenedSize)) {
        sdStatFailed++;
        Serial.println("RunService: cannot stat the CSV after reopen, will retry");
        return false;
    }

    // The last good flush always lands mid-row — it is taken right after a
    // row's first 15 columns, and the row's write_us,flush_us follow it in
    // the buffer that the failed sync just lost. That line used to be left
    // short and the replay started on a new one. It is finished now, with
    // the same "0,0" as every replayed row: its data is whole, and a line
    // short of two columns is one a spreadsheet or a strict parser stumbles
    // on (the dashboard's own .CSVB conversion reads it either way). Only
    // when the card holds exactly what that flush left; otherwise where the
    // file ends is not known, and the replay starts on a line of its own.
    csvBytesAccepted = reopenedSize;

    if (reopenedSize == confirmedBytes)
        csvBytesAccepted += csvFile.print(confirmedMidRow ? "0,0\n" : "");
    else
        csvBytesAccepted += csvFile.print('\n');

    for (int i = 0; i < pendingCount; i++) {
        csvBytesAccepted += printRowPrefix(pending[i]);
        csvBytesAccepted += csvFile.print("0,0\n");
    }

    storageFlush(csvFile);

    if (!syncedToDisk()) {
        sdReplayFailed++;
        Serial.println("RunService: replay after reopen did not stick, will retry");
        return false;
    }

    uint32_t took = millis() - recoveryStartMs;
    if (took > sdRecoverMaxMs)
        sdRecoverMaxMs = took;

    Serial.printf("RunService: SD recovered, %d rows replayed\n", pendingCount);
    pendingCount = 0;
    needsRecovery = false;
    sdRecoveries++;

    // The replay ends with a whole row.
    confirmedBytes = csvBytesAccepted;
    confirmedMidRow = false;
    lastFlush = millis();
    return true;
}

static void writeLogRow(const LogRow& row) {
    uint32_t callStartUs = micros();

    csvBytesAccepted += printRowPrefix(row);

    uint32_t flushUs = 0;
    bool flushConfirmed = false;   // this row's prefix is safely on the card
    bool recoveryTookRow = false;  // failure path already owns this row

    // By the clock — and early, when `pending` has room for only this row.
    // After the card has kept the writer waiting, the rows that queued up
    // behind it are written in one burst, far more of them than a flush
    // window holds; without a flush to confirm them and empty `pending`, the
    // burst overflowed it and the run was flagged for rows that were in fact
    // on their way to the card (found on the bench, 2026-10-11, the moment
    // the queue was made deeper than `pending`).
    bool pendingNearlyFull = pendingCapacity > 0 && pendingCount >= pendingCapacity - 1;

    if (millis() - lastFlush > RUN_FLUSH_INTERVAL_MS || pendingNearlyFull)
    {
        uint32_t flushStartUs = micros();
        errno = 0;
        storageFlush(csvFile);
        int flushErrno = errno;
        flushUs = micros() - flushStartUs;
        lastFlush = millis();

        // Exactly one csvFile.size() per flush, and it has to be this one: it
        // is fresh because printRowPrefix() wrote just above, and it costs no
        // extra SD I/O (see onDiskSize() for the caching rule). Reading it into
        // a local also keeps the verdict and the gap below consistent.
        size_t onDisk = csvFile.size();
        bool retried = false;

        if (onDisk < csvBytesAccepted) {
            // FatFS' f_sync leaves its dirty flag set when it fails, so a
            // transient card error might clear on an immediate second attempt.
            // Measured useless so far — 0 of the 14 failures in RUN0035 cleared
            // this way, the first run where the check actually worked — but
            // it's cheap and the failure mode is still unexplained.
            storageFlush(csvFile);
            retried = true;

            // csvFile.size() would be stale here (nothing written since the
            // read above), so this one has to stat(). That's a real SD read,
            // which is why it stays out of the path every flush takes:
            // doing it there pushed RUN0035 to 14 failures from the usual 4-6.
            if (!onDiskSize(onDisk))
                onDisk = 0;
        }

        if (onDisk >= csvBytesAccepted) {
            flushConfirmed = true;
            pendingCount = 0;
            confirmedBytes = csvBytesAccepted;
            confirmedMidRow = true;

            if (retried) {
                sdFlushRetryOk++;
                Serial.printf("RunService: SD sync failed on %s, retry flush fixed it (nothing lost)\n",
                              currentRun.c_str());
            }
        } else {
            sdLastErrno = flushErrno;
            sdLastGapBytes = (uint32_t)(csvBytesAccepted - onDisk);
            Serial.printf("RunService: SD sync failed on %s (errno %d, %u bytes unconfirmed) — recovering\n",
                          currentRun.c_str(), flushErrno, (unsigned)sdLastGapBytes);

            needsRecovery = true;
            recoveryStartMs = millis();
            // The whole row is replayed from `pending`, so the diagnostic
            // columns below are skipped rather than left as a stub line.
            pendingPush(row);
            recoveryTookRow = true;
            nextReopenAttemptMs = 0;
            tryRecover();
        }
    }

    uint32_t writeUs = micros() - callStartUs;

    if (!recoveryTookRow) {
        csvBytesAccepted += printRowDiagnostics(writeUs, flushUs);

        // Not covered by a confirmed flush yet: keep it in case the next
        // sync fails and this row has to be rewritten.
        if (!flushConfirmed)
            pendingPush(row);
    }

    lastWriteSuccessMs = millis();
}

static void sdWriterTask(void*) {
    LogRow row;

    for (;;) {
        // The card is away and `pending` has no room left. Taking more rows
        // off the queue now would only be to throw them away, which is what
        // this used to do: on 2026-10-10 two runs lost 0.4-0.5s that way while
        // the queue behind sat empty. Left where they are, the rows have the
        // queue's places on top of `pending`'s — 640 rows, 1.28s at 500Hz,
        // where `pending` alone was 0.77s — and whatever arrives once the
        // queue is full too is turned away in log() and counted in
        // rowsDropped.
        // Not while the run is being stopped: stop() is waiting for its
        // marker to come through, so the queue has to be emptied whatever
        // becomes of the rows.
        if (needsRecovery && pendingCount >= pendingCapacity && !stopping) {
            if (!tryRecover())
                vTaskDelay(pdMS_TO_TICKS(5));

            continue;
        }

        if (xQueueReceive(logQueue, &row, portMAX_DELAY) != pdTRUE)
            continue;

        if (row.isDrainMarker) {
            // Last chance to get the pending rows onto the card before
            // stop() closes the file.
            if (needsRecovery) {
                nextReopenAttemptMs = 0;
                tryRecover();
            }

            if (needsRecovery) {
                // Still broken: those rows never landed.
                writeFailedThisRun = true;
                rowsLost = rowsLost + pendingCount;

                uint32_t took = millis() - recoveryStartMs;
                if (took > sdRecoverMaxMs)
                    sdRecoverMaxMs = took;
            }

            xSemaphoreGive(drainSem);
            continue;
        }

        if (needsRecovery) {
            // Waiting for the card to come back: hold the rows instead of
            // dropping them, and retry (rate limited inside).
            pendingPush(row);
            tryRecover();
            continue;
        }

        if (!csvFile)
            continue;

        writeLogRow(row);
    }
}

void RunService::init() {
    if (logQueue) return;  // already initialized

    uint32_t heapBefore = ESP.getFreeHeap();

    pending = (LogRow*)malloc(sizeof(LogRow) * PENDING_MAX);
    pendingCapacity = pending ? PENDING_MAX : 0;

    if (pending)
        Serial.printf("RunService: pending-row buffer %d rows (%u bytes); free heap %u -> %u\n",
                      PENDING_MAX, (unsigned)(sizeof(LogRow) * PENDING_MAX),
                      (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
    else
        Serial.println("RunService: could not allocate the pending-row buffer — "
                       "a failed SD sync will lose rows and flag the run");

    logQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(LogRow));
    drainSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(
        sdWriterTask, "sdWriter",
        4096, nullptr, 1,
        &sdWriterTaskHandle,
        0  // core 0 — away from loop()'s sampling on core 1
    );
}

static String csvPath(const String& run) {
    return "/" + run + ".CSV";
}

static String jsonPath(const String& run) {
    return "/" + run + ".JSON";
}

static String csvbPath(const String& run) {
    return "/" + run + ".CSVB";
}

// ─── Compact binary telemetry (.CSVB) ──────────────────────────────────────
//
// The dashboard's own charts (run.html/compare.html/trends.html) only ever
// read 12 of the .CSV's 17 columns — never sats/hdop/yaw/write_us/flush_us
// (see each page's own parseCSV()). Re-encoding just those 12 as scaled
// fixed-point integers instead of ASCII decimal text, once, right after a
// run finishes, took RUN0017's ~4.85MB .CSV down to ~1.1MB: a run.html
// download-speed investigation found the real bottleneck wasn't SD reads
// (fast, ~1.5MB/s) but the WiFi upload itself, capped around ~250KB/s
// regardless of chunk size/Nagle's algorithm/WiFi power-save — an ESP32
// lwIP characteristic, not something fixable in application code. Cutting
// the bytes that have to cross that fixed ceiling is the lever that's
// actually left. The plain .CSV is kept as-is and still written live
// during recording — this is a read-only post-process, so it can't affect
// recording reliability, and a rider pulling the SD card still gets an
// ordinary, directly-readable CSV.
//
// Row layout (30 bytes, little-endian — native on this chip, and decoded
// with explicit `true` in run.html's DataView calls regardless):
//   0  uint32  t          milliseconds
//   4  int32   lat_e6     degrees * 1e6
//   8  int32   lon_e6     degrees * 1e6
//   12 int16   speed_e2   km/h * 100
//   14 int16   alt_e1     meters * 10 (not *100 — Portuguese trail
//                         elevations regularly clear 327m, the ±327.67m
//                         range *100 would otherwise clamp to)
//   16 int16   pitch_e2   RADIANS * 100 (not degrees — the BNO08x library
//                         returns radians and nothing converts them; run.html
//                         converts on decode, see its RAD_TO_DEG)
//   18 int16   roll_e2    RADIANS * 100
//   20 int16   ax_e1      m/s^2 * 10
//   22 int16   ay_e1      m/s^2 * 10
//   24 int16   az_e1      m/s^2 * 10
//   26 uint16  front_e2   mm * 100
//   28 uint16  rear_e2    mm * 100
// Preceded by an 8-byte file header: 4-byte magic "RBC1" + uint32 row count.
#pragma pack(push, 1)
struct CompactRow {
    uint32_t t;
    int32_t  lat_e6;
    int32_t  lon_e6;
    int16_t  speed_e2;
    int16_t  alt_e1;
    int16_t  pitch_e2;
    int16_t  roll_e2;
    int16_t  ax_e1;
    int16_t  ay_e1;
    int16_t  az_e1;
    uint16_t front_e2;
    uint16_t rear_e2;
};
#pragma pack(pop)
static_assert(sizeof(CompactRow) == 30, "CompactRow must stay byte-packed at exactly 30 bytes — run.html's decoder assumes this layout");

// Scales/rounds into the target integer type, clamping instead of letting
// an out-of-range value silently wrap around — a corrupted-looking spike
// in a chart is a far easier thing to notice and shrug off than a garbage
// value from integer wraparound that reads as plausible.
static int16_t clampI16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lroundf(v);
}

static uint16_t clampU16(float v) {
    if (v > 65535.0f) return 65535;
    if (v < 0.0f) return 0;
    return (uint16_t)lroundf(v);
}

// Splits one CSV line in place (like strtok — the commas themselves become
// the field terminators) and pulls out just the 12 fields writeCompactCsv()
// below needs, by their fixed column index in CSV_HEADER. Returns false if
// the line doesn't even have enough fields to be a real data row (e.g. a
// truncated last line from a run that was still recording when power was
// lost).
static bool parseCsvLine(
    char* line,
    uint32_t& t, float& lat, float& lon, float& speed,
    float& alt, float& pitch, float& roll,
    float& ax, float& ay, float& az,
    float& front, float& rear
) {
    char* fields[17] = {};
    int n = 0;
    char* p = line;

    fields[n++] = p;
    while (*p && n < 17) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }

    if (n < 15) return false;

    t     = strtoul(fields[0], nullptr, 10);
    lat   = strtof(fields[1], nullptr);
    lon   = strtof(fields[2], nullptr);
    speed = strtof(fields[3], nullptr);
    alt   = strtof(fields[6], nullptr);
    pitch = strtof(fields[7], nullptr);
    roll  = strtof(fields[8], nullptr);
    ax    = strtof(fields[10], nullptr);
    ay    = strtof(fields[11], nullptr);
    az    = strtof(fields[12], nullptr);
    front = strtof(fields[13], nullptr);
    rear  = strtof(fields[14], nullptr);

    return true;
}

// Parses one already-extracted CSV line and appends its encoded row to
// `out` if the line is valid — shared by both places writeCompactCsv()
// below finds a complete line (mid-buffer, and the trailing one after the
// read loop ends), so there's exactly one place that knows how a
// CompactRow gets built.
static void encodeAndWriteRow(File& out, char* line, uint32_t& rowCount) {
    uint32_t t;
    float lat, lon, speed, alt, pitch, roll, ax, ay, az, front, rear;

    if (!parseCsvLine(line, t, lat, lon, speed, alt, pitch, roll, ax, ay, az, front, rear))
        return;

    CompactRow row;
    row.t        = t;
    row.lat_e6   = (int32_t)lroundf(lat * 1e6f);
    row.lon_e6   = (int32_t)lroundf(lon * 1e6f);
    row.speed_e2 = clampI16(speed * 100.0f);
    row.alt_e1   = clampI16(alt * 10.0f);
    row.pitch_e2 = clampI16(pitch * 100.0f);
    row.roll_e2  = clampI16(roll * 100.0f);
    row.ax_e1    = clampI16(ax * 10.0f);
    row.ay_e1    = clampI16(ay * 10.0f);
    row.az_e1    = clampI16(az * 10.0f);
    row.front_e2 = clampU16(front * 100.0f);
    row.rear_e2  = clampU16(rear * 100.0f);

    out.write((const uint8_t*)&row, sizeof(row));
    rowCount++;
}

static bool writeCompactCsv(const String& runName) {
    File in = storageGet(csvPath(runName).c_str());
    if (!in) return false;

    // storageOpen()'s FILE_WRITE positions at the end of an existing file
    // (append), not the start — harmless for the normal case (this only
    // ever runs when ensureCompactCsv() has already confirmed no .CSVB
    // exists yet), but removing any stray leftover first guarantees a
    // clean file regardless of how this ever ends up getting called. Only
    // when one actually exists: SD.remove() on a missing file logs a
    // vfs_api.cpp "does not exists" error, and on a fresh run that's every time.
    if (storageExists(csvbPath(runName).c_str()))
        SD.remove(csvbPath(runName).c_str());

    File out = storageOpen(csvbPath(runName).c_str());
    if (!out) {
        in.close();
        return false;
    }

    out.write((const uint8_t*)"RBC1", 4);
    uint32_t rowCount = 0;
    out.write((const uint8_t*)&rowCount, sizeof(rowCount));

    // Reading line-by-line via readBytesUntil() turned out to read the
    // underlying file one byte at a time internally — unnoticeable for
    // the handful of lines this pattern is used for elsewhere in this
    // file, but a real run has tens of thousands of rows, and that many
    // individual SD reads (instead of the bulk, ~1.5MB/s reads used
    // everywhere else, e.g. handleApiRunDownload's) is what actually made
    // migrating a run take over a minute the first time. Reading in large
    // chunks ourselves and splitting lines out of that buffer in memory
    // avoids it entirely.
    constexpr size_t READ_BUF_SIZE = 8192;
    static uint8_t readBuf[READ_BUF_SIZE];

    char lineBuf[192];
    size_t lineLen = 0;
    bool firstLine = true;
    int n;

    while ((n = in.read(readBuf, READ_BUF_SIZE)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)readBuf[i];

            if (c == '\n') {
                lineBuf[lineLen] = '\0';

                if (firstLine)
                    firstLine = false;  // header row
                else if (lineLen > 0)
                    encodeAndWriteRow(out, lineBuf, rowCount);

                lineLen = 0;
            } else if (c != '\r' && lineLen < sizeof(lineBuf) - 1) {
                lineBuf[lineLen++] = c;
            }
        }

        esp_task_wdt_reset();
    }

    // A trailing line with no final newline (e.g. a run that lost power
    // mid-write) still deserves encoding, same as any other line.
    if (lineLen > 0 && !firstLine) {
        lineBuf[lineLen] = '\0';
        encodeAndWriteRow(out, lineBuf, rowCount);
    }

    out.seek(4);
    out.write((const uint8_t*)&rowCount, sizeof(rowCount));

    in.close();
    out.close();

    return true;
}

bool RunService::ensureCompactCsv(const String& name) {
    if (storageExists(csvbPath(name).c_str()))
        return true;

    return writeCompactCsv(name);
}

// Continues from the highest run number that already exists on the card,
// rather than backfilling the first free slot — with RUN0001/RUN0002/
// RUN0004 present (RUN0003 deleted), the next run must be RUN0005, not a
// resurrected RUN0003 that could get confused with — or, worse, silently
// overwrite remnants of — the run that used to have that number.
// The names of the files in the card's root ("RUN0047.CSV"), read from the
// directory and nothing else.
//
// Everything that walks the card goes through this, because the obvious way —
// File::openNextFile() — opens each entry, and on FAT opening a file means
// searching the directory for it. Listing N files that way reads the
// directory N times over, and that is before any of them is read. With 104
// runs on the card (2026-10-11) starting a run, which only wants the highest
// run number, took longer than the 8s task watchdog allows and reset the
// device.
static std::vector<String> rootFileNames() {
    std::vector<String> names;

    File root = SD.open("/");
    if (!root)
        return names;

    while (true) {
        String path = root.getNextFileName();
        if (path.length() == 0) break;

        names.push_back(path.startsWith("/") ? path.substring(1) : path);
    }

    root.close();

    return names;
}

static int nextRunNumber() {
    int maxSeen = 0;

    for (const String& name : rootFileNames())
    {
        if (!name.startsWith("RUN"))
            continue;

        int dotIdx = name.indexOf('.');
        if (dotIdx < 4)
            continue;

        int num = name.substring(3, dotIdx).toInt();
        if (num > maxSeen)
            maxSeen = num;
    }

    return maxSeen + 1;
}

// Copies the current "Zero IMU" reference into a run's .JSON as imu_up (unit
// gravity-up vector in sensor axes), so run.html can rotate that run's
// pitch/roll into the bike's frame without asking the device what the
// reference was back then. Left out entirely if the IMU was never zeroed —
// run.html treats a missing imu_up as "show raw sensor angles".
static void addImuReference(JsonDocument& doc) {
    float mag = sqrtf(settings.imuUpX * settings.imuUpX +
                      settings.imuUpY * settings.imuUpY +
                      settings.imuUpZ * settings.imuUpZ);

    if (mag < 0.5f)
        return;

    JsonArray up = doc["imu_up"].to<JsonArray>();
    up.add(serialized(String(settings.imuUpX, 5)));
    up.add(serialized(String(settings.imuUpY, 5)));
    up.add(serialized(String(settings.imuUpZ, 5)));
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

    doc["incomplete"] = writeEverStalledThisRun || writeFailedThisRun;
    doc["no_gps"] = !runStats.gpsFixSeen;
    addImuReference(doc);

    // How sure the IMU was of its own orientation, second by second (see
    // imuDiagStart()) — recorded to find out why it loses it on rough ground.
    imuDiagToJson(doc["imu_diag"].to<JsonObject>());

    // SD link health for this run — nonzero means the card hiccuped even
    // if nothing was lost (retry_ok: a second flush fixed it; recovered:
    // file reopened and buffered rows replayed).
    doc["sd_retry_ok"] = sdFlushRetryOk;
    doc["sd_recovered"] = sdRecoveries;
    doc["sd_last_errno"] = sdLastErrno;
    doc["sd_last_gap_bytes"] = sdLastGapBytes;

    // How the recoveries went: the longest one, and how many attempts failed
    // at each step (see tryRecover()). An ordinary recovery works first time
    // and takes a few ms.
    doc["sd_recover_max_ms"] = sdRecoverMaxMs;
    doc["sd_reopen_failed"] = sdReopenFailed;
    doc["sd_stat_failed"] = sdStatFailed;
    doc["sd_replay_failed"] = sdReplayFailed;

    // BNO08x link health during this run: how many times the sensor reset
    // (each one blanks the IMU columns for ~200ms) and whether it was still
    // streaming when the run ended.
    ImuHealth imuHealth = imuGetHealth();
    // Samples that never reached the writer (queue full during an SD
    // recovery), and samples the writer took but never got onto the card (see
    // rowsLost). Unlike the sd_* counters these are gone for good: together
    // they are what the run is missing. `incomplete` above covers rows_lost
    // and not rows_dropped.
    doc["rows_dropped"] = rowsDropped;
    doc["rows_lost"] = rowsLost;
    doc["rows_written"] = runStats.count - rowsDropped - rowsLost;

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

static void loadRunSetup(JsonObjectConst src, RunSetup& setup) {
    if (!src["lsc"].is<int>())
        return;

    setup.recorded = true;
    setup.lsc = src["lsc"] | 0;
    setup.hsc = src["hsc"] | 0;
    setup.lsr = src["lsr"] | 0;
    setup.hsr = src["hsr"] | 0;
    setup.volumeTokens = src["volume_tokens"] | 0;
    setup.pressurePsi = src["pressure_psi"] | 0.0f;
    setup.sagPct = src["sag_pct"] | 0.0f;
    setup.lockout = src["lockout"] | false;
    setup.hasLSC = src["has_lsc"] | true;
    setup.hasHSC = src["has_hsc"] | true;
    setup.hasLSR = src["has_lsr"] | true;
    setup.hasHSR = src["has_hsr"] | true;
}

static bool loadRunMeta(const String& runName, RunInfo& run) {
    File f = storageGet(jsonPath(runName).c_str());

    if (!f)
        return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);

    f.close();

    if (err)
        return false;

    run.bikeName = doc["bike"]["name"] | "";
    run.frontTravelMM = doc["bike"]["front_travel"] | 0.0f;
    run.rearTravelMM = doc["bike"]["rear_travel"] | 0.0f;
    run.fullSuspension = doc["bike"]["full_suspension"] | true;
    run.sizeBytes = doc["csv_size"] | 0U;
    loadRunSetup(doc["bike"]["front_suspension"], run.frontSetup);
    loadRunSetup(doc["bike"]["rear_suspension"], run.rearSetup);
    run.duration_s = doc["duration_s"] | 0.0f;
    run.startUtc = doc["start_utc"] | 0U;
    run.recovered = doc["recovered"] | false;
    run.incomplete = doc["incomplete"] | false;
    run.noGpsFix = doc["no_gps"] | false;
    run.rowsDropped = doc["rows_dropped"] | 0U;
    run.hasRowsLost = doc["rows_lost"].is<unsigned int>();
    run.rowsLost = doc["rows_lost"] | 0U;
    run.rowsWritten = doc["rows_written"] | 0U;
    run.notes = doc["notes"] | "";
    run.title = doc["title"] | "";

    JsonObjectConst stats = doc["stats"];
    if (!stats.isNull()) {
        run.hasStats = true;
        run.distanceKm = stats["distance_km"] | 0.0f;
        run.maxSpeedKmh = stats["max_speed_kmh"] | 0.0f;
        run.maxFrontMM = stats["max_front_mm"] | 0.0f;
        run.maxRearMM = stats["max_rear_mm"] | 0.0f;
    }

    return true;
}

// Rewrites one rider-editable string in an already-finished run's .JSON,
// leaving everything else in the file as it was.
static bool setRunMetaString(const String& name, const char* key, const String& value) {
    File f = storageGet(jsonPath(name).c_str());
    if (!f)
        return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
        return false;

    doc[key] = value;

    File out = storageOpen(jsonPath(name).c_str());
    if (!out)
        return false;

    serializeJson(doc, out);
    storageClose(out);

    return true;
}

bool RunService::setNotes(const String& name, const String& notes) {
    return setRunMetaString(name, "notes", notes);
}

bool RunService::setTitle(const String& name, const String& title) {
    return setRunMetaString(name, "title", title);
}

bool RunService::setSetupField(const String& name, bool rear, const char* field, float value) {
    File f = storageGet(jsonPath(name).c_str());
    if (!f)
        return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
        return false;

    // Only a setup that was recorded can be corrected — a run without one
    // doesn't get a block made of this one field.
    JsonObject setup = doc["bike"][rear ? "rear_suspension" : "front_suspension"];
    if (setup.isNull())
        return false;

    // Each in the form writeRunMeta() gives it. "lockout" is only ever there
    // when it is true.
    if (strcmp(field, "lockout") == 0) {
        if (value != 0)
            setup[field] = true;
        else
            setup.remove(field);
    } else if (strcmp(field, "pressure_psi") == 0) {
        setup[field] = serialized(String(value, 0));
    } else {
        setup[field] = (uint8_t)value;
    }

    File out = storageOpen(jsonPath(name).c_str());
    if (!out)
        return false;

    serializeJson(doc, out);
    storageClose(out);

    return true;
}

// Reads the timestamp_ms of the first data row (skipping the header).
static bool readFirstCsvTimestamp(File& csv, uint32_t& outMs) {
    csv.seek(0);
    csv.readStringUntil('\n');  // header

    String line = csv.readStringUntil('\n');
    int comma = line.indexOf(',');
    if (comma <= 0)
        return false;

    outMs = line.substring(0, comma).toInt();
    return true;
}

// Reads the timestamp_ms of the last complete data row. Seeks near the end
// instead of scanning the whole file — rows are short and fixed-ish in
// width, so a small tail comfortably contains at least one full line.
static bool readLastCsvTimestamp(File& csv, uint32_t& outMs) {
    size_t size = csv.size();
    if (size == 0)
        return false;

    constexpr size_t TAIL_BYTES = 256;
    size_t start = (size > TAIL_BYTES) ? size - TAIL_BYTES : 0;

    csv.seek(start);
    String tail = csv.readString();
    tail.trim();  // drop the trailing newline left by the last printf()

    int lastNewline = tail.lastIndexOf('\n');
    String lastLine = (lastNewline >= 0) ? tail.substring(lastNewline + 1) : tail;

    int comma = lastLine.indexOf(',');
    if (comma <= 0)
        return false;

    outMs = lastLine.substring(0, comma).toInt();
    return true;
}

// Best-effort reconstruction for a run whose .JSON never got written —
// duration comes from the CSV's own first/last timestamps, but the bike/
// setup fields can only reflect whatever Settings currently hold, since
// that's the only information still available at boot.
static void recoverOrphanedRun(const String& runName) {
    File csv = storageGet(csvPath(runName).c_str());
    if (!csv)
        return;

    uint32_t firstMs = 0, lastMs = 0;
    bool gotFirst = readFirstCsvTimestamp(csv, firstMs);
    bool gotLast = readLastCsvTimestamp(csv, lastMs);
    size_t csvSize = csv.size();
    csv.close();

    float durationSeconds = (gotFirst && gotLast && lastMs >= firstMs)
        ? (lastMs - firstMs) / 1000.0f
        : 0.0f;

    File f = storageOpen(jsonPath(runName).c_str());
    if (!f)
        return;

    JsonDocument doc;

    doc["name"] = runName;
    doc["duration_s"] = serialized(String(durationSeconds, 1));
    doc["csv_size"] = csvSize;
    doc["recovered"] = true;
    addImuReference(doc);

    JsonObject bike = doc["bike"].to<JsonObject>();
    bike["name"] = settings.bikeName;
    bike["wheel_size"] = settings.wheelSize;
    bike["full_suspension"] = settings.fullSuspension;
    bike["front_travel"] = serialized(String(suspensionGetMaxTravelMM(FRONT), 1));
    bike["rear_travel"] = serialized(String(suspensionGetMaxTravelMM(REAR), 1));
    bike["rear_wheel_travel_mm"] = serialized(String(settings.rearWheelTravelMM, 1));

    JsonObject frontSetup = bike["front_suspension"].to<JsonObject>();
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

    writeCompactCsv(runName);

    Serial.println("Recovered orphaned run: " + runName + " (" + String(durationSeconds, 1) + "s)");
}

void RunService::recoverOrphanedRuns() {
    std::vector<String> files = rootFileNames();

    // A .CSV with no .JSON next to it, both looked for in the names just
    // read rather than on the card (see rootFileNames()).
    for (const String& name : files) {
        if (!name.endsWith(".CSV"))
            continue;

        String base = name.substring(0, name.length() - 4);
        String json = base + ".JSON";

        bool hasJson = false;
        for (const String& other : files) {
            if (other == json) {
                hasJson = true;
                break;
            }
        }

        if (!hasJson)
            recoverOrphanedRun(base);
    }
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
    csvFullPath = String("/sd") + csvPath(currentRun);  // SD.begin()'s default mountpoint

    csvFile = storageOpen(csvPath(currentRun).c_str());

    if (!csvFile)
    {
        Serial.println("RunService: failed to open CSV");
        currentRun = "";
        csvFullPath = "";
        recordingRelease();
        return false;
    }

    csvBytesAccepted = csvFile.println(CSV_HEADER);
    storageFlush(csvFile);
    confirmedBytes = csvBytesAccepted;
    confirmedMidRow = false;

    writeFailedThisRun = false;
    needsRecovery = false;
    nextReopenAttemptMs = 0;
    pendingCount = 0;
    sdFlushRetryOk = 0;
    sdRecoveries = 0;
    sdLastErrno = 0;
    sdLastGapBytes = 0;
    sdRecoverMaxMs = 0;
    sdReopenFailed = 0;
    sdStatFailed = 0;
    sdReplayFailed = 0;
    rowsLost = 0;
    imuResetsAtStart = imuGetHealth().resets;
    rowsDropped = 0;

    // Only a warning, not a failure: an overflow is detected and flagged
    // rather than silently losing rows. The sampling loop does deliver the
    // configured rate since 2026-10-07 (it used to manage 300-390 rows/s of a
    // configured 500), so this is a real check now: 500Hz is 50 rows per
    // flush window, well inside the buffer.
    uint32_t rowsPerInterval = (uint32_t)settings.suspensionSampleRateHz * RUN_FLUSH_INTERVAL_MS / 1000;
    if ((int)rowsPerInterval > pendingCapacity)
        Serial.printf("RunService: warning — %uHz x %ums = up to %u rows per flush window, "
                      "pending buffer holds %d; a sync failure could lose rows\n",
                      settings.suspensionSampleRateHz, (unsigned)RUN_FLUSH_INTERVAL_MS,
                      (unsigned)rowsPerInterval, pendingCapacity);

    toggleRecording();
    lastFlush = millis();

    runStartMs = millis();
    lastWriteSuccessMs = runStartMs;
    writeEverStalledThisRun = false;
    runStats = RunStatsAccumulator{};
    imuDiagStart();

    Serial.println("Run started: " + currentRun);
    return true;
}

void RunService::stop() {
    autoStopArmed = false;

    if (getState() != RECORDING)
        return;

    // Rows already queued from the last few samples haven't necessarily
    // been written yet — drain them through the SD writer task before
    // touching csvFile here, otherwise this close() could race the
    // writer's own printf()/flush() on the same File object.
    LogRow marker{};
    marker.isDrainMarker = true;
    stopping = true;
    xQueueSend(logQueue, &marker, portMAX_DELAY);
    xSemaphoreTake(drainSem, pdMS_TO_TICKS(3000));
    stopping = false;
    imuDiagStop();

    storageClose(csvFile);

    float durationSeconds =
        (millis() - runStartMs) / 1000.0f;

    writeRunMeta(durationSeconds);
    writeCompactCsv(currentRun);

    toggleRecording();

    Serial.println("Run stopped: " + currentRun);
    csvFullPath = "";

    if (rowsDropped > 0)
        Serial.printf("Rows: %u dropped of %u sampled (%.1f%% lost — writer queue full)\n",
                      (unsigned)rowsDropped, (unsigned)runStats.count,
                      100.0f * rowsDropped / (float)runStats.count);

    currentRun = "";
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

    // Never block the sampling loop: if the writer task has fallen far
    // enough behind to fill the queue, drop this sample rather than stall
    // core 1 waiting for room.
    //
    // The drop is counted, not shrugged off. This return value used to be
    // ignored, which made it the one place data could vanish with no trace
    // anywhere: RUN0039 lost ~4.5s of samples out of 34s in 21 holes and
    // still reported `incomplete: false`, because that flag only ever tracked
    // the pending buffer and the writer stall. Every hole lines up with an SD
    // recovery long enough to fill the queue (a reopen + replaying ~300 rows,
    // sometimes more than once) — see rows_dropped in the run's .JSON.
    if (xQueueSend(logQueue, &row, 0) != pdTRUE)
        rowsDropped++;
}

std::vector<String> RunService::names() {
    std::vector<String> runs;

    for (const String& name : rootFileNames()) {
        if (name.endsWith(".CSV"))
            runs.push_back(name.substring(0, name.length() - 4));
    }

    std::sort(runs.begin(), runs.end(), [](const String& a, const String& b) {
        return a > b;
    });

    return runs;
}

std::vector<RunStamp> RunService::stamps() {
    std::vector<RunStamp> runs;

    // Through FatFS itself rather than the Arduino File API or readdir():
    // those give a directory entry's name and nothing else, and the entry
    // also holds the file's size and when it was last written — which is the
    // whole stamp, at no cost beyond the pass names() already makes (25ms for
    // the 26 files of 8 runs, 2026-10-13). "0:" is the SD card: the only
    // FatFS volume there is, the dashboard's own files being on LittleFS.
    // FatFS takes its own lock per volume, so this is safe next to the
    // writer task.
    FF_DIR dir;
    FILINFO entry;

    if (f_opendir(&dir, "0:/") != FR_OK) {
        // The names, then, with no stamps: the dashboard reads those runs
        // every time instead of keeping them, which is slower and not wrong.
        for (const String& name : names())
            runs.push_back({ name, "" });

        return runs;
    }

    std::vector<RunStamp> jsons;

    while (f_readdir(&dir, &entry) == FR_OK && entry.fname[0]) {
        String name = entry.fname;

        if (name.endsWith(".CSV")) {
            runs.push_back({ name.substring(0, name.length() - 4), "" });
        } else if (name.endsWith(".JSON")) {
            jsons.push_back({
                name.substring(0, name.length() - 5),
                String((unsigned long)entry.fsize) + "." + String(entry.fdate) + "." + String(entry.ftime)
            });
        }
    }

    f_closedir(&dir);

    for (RunStamp& run : runs) {
        for (const RunStamp& json : jsons) {
            if (json.name == run.name) {
                run.stamp = json.stamp;
                break;
            }
        }
    }

    std::sort(runs.begin(), runs.end(), [](const RunStamp& a, const RunStamp& b) {
        return a.name > b.name;
    });

    return runs;
}

RunInfo RunService::info(const String& name) {
    RunInfo run;
    run.name = name;

    loadRunMeta(name, run);

    // The .CSV's size is in the run's .JSON. Asking the card for it is a
    // second search of the directory, which is most of what a run costs
    // here: only for a run whose .JSON does not say.
    if (run.sizeBytes == 0) {
        struct stat st;
        if (stat((String("/sd") + csvPath(name)).c_str(), &st) == 0)
            run.sizeBytes = (size_t)st.st_size;
    }

    return run;
}

bool RunService::remove(const String& name) {
    String base = name;

    if (base.endsWith(".CSV"))
        base.replace(".CSV", "");

    String csv = csvPath(base);
    String json = jsonPath(base);

    bool ok1 = SD.remove(csv);
    bool ok2 = SD.remove(json);

    // Best-effort — most runs won't have one yet if this predates the
    // compact-telemetry feature, and ensureCompactCsv() regenerating one
    // for a run that's mid-delete isn't a real scenario, so a missing
    // .CSVB here isn't a failure worth reporting alongside the two above.
    if (storageExists(csvbPath(base).c_str()))
        SD.remove(csvbPath(base));

    return ok1 && ok2;
}

bool RunService::removeAll() {
    bool allOk = true;

    for (const String& name : names()) {
        if (!remove(name))
            allOk = false;
    }

    return allOk;
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

    bool stalled = (millis() - lastWriteSuccessMs) > WRITE_STALL_THRESHOLD_MS;

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
    return stalled || writeFailedThisRun;
}
