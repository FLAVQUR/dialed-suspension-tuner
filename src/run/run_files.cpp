// The runs on the card, as files: listing them, reading and editing a
// finished run's .JSON, deleting them, and giving a run that lost its .JSON
// (power cut while recording) a new one at boot. Nothing here touches a run
// being recorded; that is run_service.cpp.
#include <vector>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <ff.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "run_service.h"
#include "run_files.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "suspension/suspension.h"

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
std::vector<String> rootFileNames() {
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

// Continues from the highest run number that already exists on the card,
// rather than backfilling the first free slot — with RUN0001/RUN0002/
// RUN0004 present (RUN0003 deleted), the next run must be RUN0005, not a
// resurrected RUN0003 that could get confused with — or, worse, silently
// overwrite remnants of — the run that used to have that number.
int nextRunNumber() {
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
void addImuReference(JsonDocument& doc) {
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

static void loadRunSetup(JsonObjectConst src, RunSetup& setup) {
    if (!src["lsc"].is<int>())
        return;

    setup.recorded = true;
    setup.model = src["model"] | "";
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
        run.avgFrontMM = stats["avg_front_mm"] | 0.0f;
        run.avgRearMM = stats["avg_rear_mm"] | 0.0f;

        // Only in runs recorded since 2026-11-07, or filled in afterwards by
        // Trends (setBottomOuts()).
        run.hasBottomOuts = stats["bottom_outs_front"].is<unsigned int>() && stats["bottom_outs_rear"].is<unsigned int>();
        run.bottomOutsFront = stats["bottom_outs_front"] | 0U;
        run.bottomOutsRear = stats["bottom_outs_rear"] | 0U;
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

bool RunService::setBottomOuts(const String& name, uint16_t front, uint16_t rear) {
    File f = storageGet(jsonPath(name).c_str());
    if (!f)
        return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    // A run with no stats block (recovered, or no sample at all) has no
    // averages either, so Trends reads its telemetry whatever this says.
    if (err || !doc["stats"].is<JsonObject>())
        return false;

    doc["stats"]["bottom_outs_front"] = front;
    doc["stats"]["bottom_outs_rear"] = rear;

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

bool RunService::setSetupFields(const String& name, bool rear, const std::vector<SetupCorrection>& fields) {
    File f = storageGet(jsonPath(name).c_str());
    if (!f)
        return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
        return false;

    // Only a setup that was recorded can be corrected — a run without one
    // doesn't get a block made of these fields.
    JsonObject setup = doc["bike"][rear ? "rear_suspension" : "front_suspension"];
    if (setup.isNull())
        return false;

    for (const SetupCorrection& c : fields) {
        const char* field = c.field;
        bool pressure = strcmp(field, "pressure_psi") == 0;

        // The model, like the numbers below, keeps what the run was recorded
        // with — unless it was recorded with none (every run before
        // 2026-10-28), when there is nothing to keep.
        if (strcmp(field, "model") == 0) {
            JsonObject recorded = setup["recorded"];
            bool hadOriginal = !recorded.isNull() && recorded["model"].is<const char*>();
            String original = hadOriginal ? recorded["model"].as<String>() : (setup["model"] | "");

            if (c.text.length()) setup["model"] = c.text;
            else setup.remove("model");

            if (c.text == original) {
                if (hadOriginal) recorded.remove("model");
            } else if (!hadOriginal && original.length()) {
                if (recorded.isNull()) recorded = setup["recorded"].to<JsonObject>();
                recorded["model"] = original;
            }

            if (!setup["recorded"].isNull() && setup["recorded"].size() == 0)
                setup.remove("recorded");
            continue;
        }

        // Each in the form writeRunMeta() gives it. "lockout" is only ever
        // there when it is true.
        if (strcmp(field, "lockout") == 0) {
            if (c.value != 0)
                setup[field] = true;
            else
                setup.remove(field);
            continue;
        }

        float value = pressure ? roundf(c.value) : (float)(uint8_t)c.value;

        // What the run was recorded with, kept the first time it changes —
        // and dropped again once the field is back to it.
        JsonObject recorded = setup["recorded"];
        bool hadOriginal = !recorded.isNull() && recorded[field].is<float>();
        float original = hadOriginal ? recorded[field].as<float>() : (setup[field] | 0.0f);

        if (pressure)
            setup[field] = serialized(String(value, 0));
        else
            setup[field] = (uint8_t)value;

        if (value == original) {
            if (hadOriginal) recorded.remove(field);
        } else if (!hadOriginal) {
            if (recorded.isNull()) recorded = setup["recorded"].to<JsonObject>();
            if (pressure) recorded[field] = serialized(String(original, 0));
            else recorded[field] = (uint8_t)original;
        }

        if (!setup["recorded"].isNull() && setup["recorded"].size() == 0)
            setup.remove("recorded");
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
