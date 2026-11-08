// The run's SD writer: everything between a sample leaving the sampling loop
// and its row being safely on the card. The sampling loop hands rows over
// (push()); a task on core 0 writes them, checks that every flush reached the
// card, and recovers when one did not. run_service.cpp drives it through
// run_writer.h and owns everything else about a run.
#include <atomic>
#include <cmath>
#include <cerrno>
#include <sys/stat.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "run_writer.h"
#include "run_files.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "connectivity/network.h"

static File csvFile;
static String currentRun;   // the run being written; empty when there is none
static uint32_t lastFlush = 0;

// Tried at 500ms (2026-09-26) to test whether the sync *rate* was what the card
// couldn't take — "Syncing less often" in docs/sd-card.md — and the answer is no, so this is
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

// Samples the sampling loop produced but could not hand to the writer because
// the queue was full — real, permanently missing rows. Written from loop()
// (core 1), read at stop().
static volatile uint32_t rowsDropped = 0;

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
// LogRow is in run_writer.h: log() builds it.

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

// ─── Spill buffer: more room behind the queue, while the radio is off ───────
//
// The queue covers 0.9s of a card that has stopped answering. On 2026-10-25
// (RUN0057) the card stopped for 1.36s over three writes, no write failing,
// and 151 rows were dropped. The queue can't simply be made deeper: it is
// allocated at boot from the heap WiFi needs (see `pending` below for what
// 48KB too many did to lwIP). But with the radio off for a run, the WiFi
// driver's memory is free — so a run takes a second, larger buffer for its
// own duration, and gives it back before the radio comes up again.
//
// Rows go into it only once the queue is full, and every row after that
// goes into it too until it is empty again, so the writer — which takes from
// the queue first and from here only when the queue is empty — sees them in
// the order they were sampled. One producer (log(), core 1) and one consumer
// (the writer task, core 0): a ring with an index each side, no lock.
//
// Taken in chunks, not as one block: with the radio off there is ~100KB
// free, but in pieces — on the bench the largest one was 31–41KB and shrank
// from run to run, so a single block gave 223–383 rows. 8KB chunks fit in
// the gaps, and it is the total that counts.
static constexpr uint32_t SPILL_CHUNK_ROWS = 64;        // 4KB a chunk
static constexpr uint32_t SPILL_MAX_CHUNKS = 16;        // 1024 rows, 2s at 500Hz, 64KB
static constexpr uint32_t SPILL_HEAP_RESERVE = 32768;   // free heap left for everything else
static LogRow* spillChunks[SPILL_MAX_CHUNKS] = {};
static uint32_t spillChunkCount = 0;
static uint32_t spillCapacity = 0;          // set before the run starts, 0 when there is none

static LogRow& spillSlot(uint32_t index) {
    uint32_t i = index % spillCapacity;
    return spillChunks[i / SPILL_CHUNK_ROWS][i % SPILL_CHUNK_ROWS];
}
// Release on the side that writes an index, acquire on the side that reads
// it: a row is in the ring before the other core can see the head move past
// it, and out of it before the tail move lets the slot be reused.
static std::atomic<uint32_t> spillHead{0};  // written by log() only
static std::atomic<uint32_t> spillTail{0};  // written by the writer task only
static uint32_t spillMaxUsed = 0;           // the most it held at once, for the run's .JSON
static uint32_t heapFreeAtStart = 0;        // free heap once the run's buffers are taken

static uint32_t spillCount() {
    return spillHead.load(std::memory_order_acquire) - spillTail.load(std::memory_order_acquire);
}

// log()'s side. False when there is no spill buffer or it is full.
static bool spillPush(const LogRow& r) {
    uint32_t head = spillHead.load(std::memory_order_relaxed);
    uint32_t used = head - spillTail.load(std::memory_order_acquire);

    if (spillCapacity == 0 || used >= spillCapacity)
        return false;

    spillSlot(head) = r;
    spillHead.store(head + 1, std::memory_order_release);

    if (used + 1 > spillMaxUsed)
        spillMaxUsed = used + 1;

    return true;
}

// The writer's side. Never touches the buffer when it is empty, which is
// what lets stop() free it once it has drained.
static bool spillPop(LogRow& r) {
    uint32_t tail = spillTail.load(std::memory_order_relaxed);

    if (spillHead.load(std::memory_order_acquire) == tail)
        return false;

    r = spillSlot(tail);
    spillTail.store(tail + 1, std::memory_order_release);
    return true;
}

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

// The next row for the writer, oldest first: the queue, then the spill
// buffer. While the spill holds rows, log() adds nothing to the queue, so
// whatever is left in the queue is older than all of them.
//
// stop()'s drain marker is the one thing that can arrive in the queue while
// the spill holds rows — and those rows were sampled before it. So it is held
// back until the spill has been emptied, then handed over.
//
// Blocking on the queue is safe: the spill only fills while the queue is
// full, and the writer empties the queue before it looks at the spill.
static bool markerHeldBack = false;

// At the start of a run, once the radio is down: up to SPILL_MAX_CHUNKS
// chunks, while the free heap stays above SPILL_HEAP_RESERVE. None with the
// radio on — that heap is lwIP's.
static uint32_t spillRowsThisRun = 0;

static void spillTake() {
    spillMaxUsed = 0;
    spillRowsThisRun = 0;

    // Left over from a run whose drain never finished (see spillGiveBack()).
    if (spillCapacity > 0) {
        spillRowsThisRun = spillCapacity;
        return;
    }

    if (wifiGetMode() != WifiMode::NET_OFF)
        return;

    uint32_t chunkBytes = SPILL_CHUNK_ROWS * sizeof(LogRow);

    while (spillChunkCount < SPILL_MAX_CHUNKS && ESP.getFreeHeap() >= SPILL_HEAP_RESERVE + chunkBytes) {
        LogRow* chunk = (LogRow*)malloc(chunkBytes);
        if (!chunk)
            break;
        spillChunks[spillChunkCount++] = chunk;
    }

    spillHead.store(0, std::memory_order_relaxed);
    spillTail.store(0, std::memory_order_relaxed);
    spillCapacity = spillChunkCount * SPILL_CHUNK_ROWS;
    spillRowsThisRun = spillCapacity;
    Serial.printf("RunService: spill buffer %u rows in %u chunks, free heap %u\n",
                  (unsigned)spillCapacity, (unsigned)spillChunkCount, (unsigned)ESP.getFreeHeap());
}

// At the end of a run, before the radio comes back. Only once the writer has
// emptied it: while it holds rows the writer may be reading one.
static void spillGiveBack() {
    if (spillCapacity == 0)
        return;

    for (int i = 0; i < 100 && spillCount() > 0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (spillCount() > 0) {
        Serial.printf("RunService: spill buffer still holds %u rows; kept\n", (unsigned)spillCount());
        return;
    }

    spillCapacity = 0;

    for (uint32_t c = 0; c < spillChunkCount; c++) {
        free(spillChunks[c]);
        spillChunks[c] = nullptr;
    }

    spillChunkCount = 0;
}

static bool takeRow(LogRow& row) {
    for (;;) {
        if (markerHeldBack) {
            if (spillPop(row))
                return true;

            markerHeldBack = false;
            row = LogRow{};
            row.isDrainMarker = true;
            return true;
        }

        if (xQueueReceive(logQueue, &row, 0) == pdTRUE ||
            (spillCount() == 0 && xQueueReceive(logQueue, &row, portMAX_DELAY) == pdTRUE)) {
            if (row.isDrainMarker && spillCount() > 0) {
                markerHeldBack = true;
                continue;
            }

            return true;
        }

        if (spillPop(row))
            return true;
    }
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

        if (!takeRow(row))
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
void RunWriter::init() {
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

bool RunWriter::open(const String& runName) {
    currentRun = runName;
    csvFullPath = String("/sd") + csvPath(currentRun);  // SD.begin()'s default mountpoint

    csvFile = storageOpen(csvPath(currentRun).c_str());

    if (!csvFile)
    {
        Serial.println("RunService: failed to open CSV");
        currentRun = "";
        csvFullPath = "";
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

    lastFlush = millis();
    lastWriteSuccessMs = lastFlush;
    return true;
}

void RunWriter::takeSpill() {
    spillTake();
    heapFreeAtStart = ESP.getFreeHeap();
}

// log()'s side, on the sampling core. Never blocks: if the writer task has
// fallen far enough behind to fill the queue, the row goes to the spill
// buffer, and past that it is dropped rather than stall core 1.
//
// The drop is counted, not shrugged off. This return value used to be
// ignored, which made it the one place data could vanish with no trace
// anywhere: RUN0039 lost ~4.5s of samples out of 34s in 21 holes and
// still reported `incomplete: false`, because that flag only ever tracked
// the pending buffer and the writer stall. Every hole lines up with an SD
// recovery long enough to fill the queue (a reopen + replaying ~300 rows,
// sometimes more than once) — see rows_dropped in the run's .JSON.
//
// Past a full queue, the spill buffer when the run has one — and while
// that holds rows, every row goes there, so none overtakes them.
void RunWriter::push(const LogRow& row) {
    bool queued = spillCount() == 0 && xQueueSend(logQueue, &row, 0) == pdTRUE;

    if (!queued && !spillPush(row))
        rowsDropped++;
}

// Rows already queued from the last few samples haven't necessarily been
// written yet: they go through the writer task before close() touches
// csvFile, or the close could race the writer's own write/flush on the same
// File object.
void RunWriter::drain() {
    LogRow marker{};
    marker.isDrainMarker = true;
    stopping = true;
    xQueueSend(logQueue, &marker, portMAX_DELAY);
    xSemaphoreTake(drainSem, pdMS_TO_TICKS(3000));
    stopping = false;
    spillGiveBack();
}

void RunWriter::close() {
    storageClose(csvFile);
    csvFullPath = "";
    currentRun = "";
}

RunWriter::Report RunWriter::report() {
    Report r;
    r.failed = writeFailedThisRun;
    r.rowsDropped = rowsDropped;
    r.rowsLost = rowsLost;
    r.sdRetryOk = sdFlushRetryOk;
    r.sdRecovered = sdRecoveries;
    r.sdLastErrno = sdLastErrno;
    r.sdLastGapBytes = sdLastGapBytes;
    r.sdRecoverMaxMs = sdRecoverMaxMs;
    r.sdReopenFailed = sdReopenFailed;
    r.sdStatFailed = sdStatFailed;
    r.sdReplayFailed = sdReplayFailed;
    r.spillRows = spillRowsThisRun;
    r.spillMax = spillMaxUsed;
    r.heapFreeRecording = heapFreeAtStart;
    return r;
}

uint32_t RunWriter::lastWriteMs() {
    return lastWriteSuccessMs;
}

bool RunWriter::failed() {
    return writeFailedThisRun;
}
