// The run's compact binary telemetry (.CSVB): written from its .CSV once the
// run has stopped (or been recovered), and on demand for runs that have none.
#include <cmath>
#include <SD.h>
#include <esp_task_wdt.h>
#include "run_service.h"
#include "run_files.h"
#include "storage/storage.h"

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

bool writeCompactCsv(const String& runName) {
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
