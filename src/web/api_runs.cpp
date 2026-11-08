// The API for recorded runs (/api/runs/*): the list, downloads, the ZIP,
// notes, titles, setup corrections, deleting. Routes are in serverInit().
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "server.h"
#include "power/battery.h"
#include "gps/gps.h"
#include "system/system.h"
#include "system/clock.h"
#include "debug/loop_profiler.h"
#include "debug/log_capture.h"
#include "connectivity/network.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "run/run_service.h"
#include "suspension/suspension.h"
#include "imu/imu.h"
#include "zip_writer.h"
#include "dialed_web_server.h"
#include "system/version.h"
#include "bikes/bikes.h"
#include "api.h"

static const char* getMimeType(const String& filename) {
    if (filename.endsWith(".CSV")) return "text/csv";
    if (filename.endsWith(".JSON")) return "application/json";
    return "application/octet-stream";
}

// One end's setup under the same keys the run's own .JSON uses, so the
// dashboard reads it the same way from either place.
static void addRunSetup(JsonObject run, const char* key, const RunSetup& setup) {
    if (!setup.recorded)
        return;

    JsonObject obj = run[key].to<JsonObject>();

    if (setup.model.length()) obj["model"] = setup.model;
    obj["lsc"] = setup.lsc;
    obj["hsc"] = setup.hsc;
    obj["lsr"] = setup.lsr;
    obj["hsr"] = setup.hsr;
    obj["volume_tokens"] = setup.volumeTokens;
    obj["pressure_psi"] = serialized(String(setup.pressurePsi, 0));
    obj["sag_pct"] = serialized(String(setup.sagPct, 1));
    if (setup.lockout) obj["lockout"] = true;
    obj["has_lsc"] = setup.hasLSC;
    obj["has_hsc"] = setup.hasHSC;
    obj["has_lsr"] = setup.hasLSR;
    obj["has_hsr"] = setup.hasHSR;
}

// The runs list, a page at a time: { total, offset, runs: [...] }, newest
// first, `limit` runs from `offset` (25 unless asked, 50 at most).
//
// It used to be every run in one reply, built whole in memory and then sent.
// Each run is a .JSON read off the card inside a request that has the 8s
// task watchdog behind it, and with 104 runs on the card (2026-10-11) asking
// for the list reset the device, every time. Two things keep it bounded now:
//   - a page is a limited number of runs, and the watchdog is fed after each;
//   - the reply is sent a run at a time and never held whole. A page of 25
//     runs with their setups is 16KB of JSON, and building that next to the
//     document it is made from left the WiFi stack too little memory to send
//     it: replies took 10-40s and the watchdog fired anyway.
constexpr size_t RUNS_PAGE_DEFAULT = 25;
constexpr size_t RUNS_PAGE_MAX = 50;

static String runJson(const RunInfo& run, bool withSetup) {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();

    obj["name"] = run.name;
    obj["bike_name"] = run.bikeName;
    obj["size_kb"] = serialized(String(run.sizeBytes / 1024.0f, 1));
    obj["duration_s"] = serialized(String(run.duration_s, 1));
    obj["front_travel"] = serialized(String(run.frontTravelMM, 0));
    obj["rear_travel"] = serialized(String(run.rearTravelMM, 0));
    obj["full_suspension"] = run.fullSuspension;
    obj["recovered"] = run.recovered;
    obj["incomplete"] = run.incomplete;
    obj["no_gps"] = run.noGpsFix;
    obj["rows_dropped"] = run.rowsDropped;
    obj["rows_written"] = run.rowsWritten;
    if (run.hasRowsLost) obj["rows_lost"] = run.rowsLost;
    obj["notes"] = run.notes;
    obj["title"] = run.title;

    if (run.startUtc > 0)
        obj["start_utc"] = run.startUtc;

    if (run.hasStats) {
        obj["distance_km"] = serialized(String(run.distanceKm, 2));
        obj["max_speed_kmh"] = serialized(String(run.maxSpeedKmh, 1));
        obj["max_front_mm"] = serialized(String(run.maxFrontMM, 1));
        obj["max_rear_mm"] = serialized(String(run.maxRearMM, 1));
        obj["avg_front_mm"] = serialized(String(run.avgFrontMM, 1));
        obj["avg_rear_mm"] = serialized(String(run.avgRearMM, 1));

        if (run.hasBottomOuts) {
            obj["bottom_outs_front"] = run.bottomOutsFront;
            obj["bottom_outs_rear"] = run.bottomOutsRear;
        }
    }

    // Each run's setup more than doubles the reply, and only the pages that
    // show the setup history want it (?setup=1).
    if (withSetup) {
        addRunSetup(obj, "front_suspension", run.frontSetup);
        addRunSetup(obj, "rear_suspension", run.rearSetup);
    }

    String json;
    serializeJson(doc, json);
    return json;
}

// Sends the runs named in names[from, to) as the elements of a JSON array
// that the caller has opened and will close.
static void sendRuns(const std::vector<String>& names, size_t from, size_t to, bool withSetup) {
    for (size_t i = from; i < to && i < names.size(); i++) {
        String json = runJson(RunService::info(names[i]), withSetup);

        server.sendContent(i == from ? json : "," + json);

        // Opening a file takes longer the further down the card's directory
        // it is, so a run takes longer to read the more runs there are:
        // bounded per run, then, not per request.
        esp_task_wdt_reset();
    }
}

// GET /api/runs, three ways:
//   ?index=1            every run's name and stamp, nothing read off any file:
//                       { fw, total, runs: [[name, stamp], ...] }, newest first
//   ?names=RUN1,RUN2    those runs (RUNS_PAGE_MAX at most): { runs: [...] }
//   ?offset=&limit=     a page of the list: { total, offset, runs: [...] }
// The dashboard asks for the index, then by name for the runs whose stamp it
// has not seen (common.js). The page form is what the list was before that,
// and stays for anything that just wants to look.
void handleApiRuns() {
    bool withSetup = server.hasArg("setup");

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);

    if (server.hasArg("index")) {
        std::vector<RunStamp> runs = RunService::stamps();

        server.beginResponse(200, "application/json");

        // `fw` because what a run's entry holds can change with the
        // firmware: the dashboard drops what it kept when this does. With
        // the build time in it, since two builds of uncommitted changes share
        // a version ("-dirty"): the entries kept before the bottom-out counts
        // were added outlived the build that added them (2026-11-07).
        String piece = "{\"fw\":\"" + String(FIRMWARE_VERSION) + " " + String(FIRMWARE_BUILD) + "\",\"total\":" + String(runs.size()) + ",\"runs\":[";

        for (size_t i = 0; i < runs.size(); i++) {
            piece += String(i == 0 ? "" : ",") + "[\"" + runs[i].name + "\",\"" + runs[i].stamp + "\"]";

            // In pieces: even this grows with the card, and a reply held
            // whole is what the WiFi stack could not send (see above).
            if (piece.length() > 1024) {
                server.sendContent(piece);
                piece = "";
            }
        }

        server.sendContent(piece + "]}");
        server.sendContent("");
        return;
    }

    if (server.hasArg("names")) {
        // Split on commas, keeping only what can be a run's name: these go
        // into file paths.
        std::vector<String> names;
        String list = server.arg("names");

        for (int start = 0; start <= (int)list.length() && names.size() < RUNS_PAGE_MAX; ) {
            int comma = list.indexOf(',', start);
            if (comma < 0) comma = list.length();

            String name = list.substring(start, comma);
            bool plain = name.length() > 0 && name.length() <= 16;

            for (size_t c = 0; plain && c < name.length(); c++)
                plain = isalnum((unsigned char)name[c]);

            if (plain) names.push_back(name);

            start = comma + 1;
        }

        server.beginResponse(200, "application/json");
        server.sendContent("{\"runs\":[");
        sendRuns(names, 0, names.size(), withSetup);
        server.sendContent("]}");
        server.sendContent("");
        return;
    }

    size_t offset = server.hasArg("offset") ? (size_t)max(0L, server.arg("offset").toInt()) : 0;
    size_t limit = server.hasArg("limit") ? (size_t)max(0L, server.arg("limit").toInt()) : RUNS_PAGE_DEFAULT;

    if (limit > RUNS_PAGE_MAX)
        limit = RUNS_PAGE_MAX;

    std::vector<String> names = RunService::names();

    server.beginResponse(200, "application/json");
    server.sendContent("{\"total\":" + String(names.size()) + ",\"offset\":" + String(offset) + ",\"runs\":[");
    sendRuns(names, offset, offset + limit, withSetup);
    server.sendContent("]}");
    server.sendContent("");
}

// Shared by handleApiRunDownload/handleApiRunDownloadDebug/
// handleApiNetworkDebug below — one request is handled at a time on this
// single-threaded server, so there's no concurrency risk, and a separate
// 16KB buffer per handler was enough to overflow the ESP32's DRAM budget
// (only one of these three ever runs at once anyway).
constexpr size_t DOWNLOAD_CHUNK_SIZE = 16384;
static uint8_t downloadBuf[DOWNLOAD_CHUNK_SIZE];

void handleApiRunDownload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing file");
        return;
    }

    String filename = server.arg("file");

    if (!filename.startsWith("/"))
        filename = "/" + filename;

    File file = storageGet(filename.c_str());

    if (!file) {
        server.send(404, "text/plain", "File not found");
        return;
    }

    String downloadName = filename.startsWith("/") ? filename.substring(1) : filename;

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
    server.setContentLength(file.size());
    server.beginResponse(200, getMimeType(filename));

    // Nagle's algorithm holds a small write back for up to ~40ms waiting
    // to see if more data is coming, hoping to coalesce it with the next
    // write into one packet — the exact opposite of useful here, since
    // the next chunk is already sitting in `buf` a few instructions later.
    // With the old 2KB chunk size, a multi-MB run CSV meant thousands of
    // chunks each eating that stall — the actual reason downloads were
    // taking 30-40s, not raw SD/WiFi throughput (SD is already at 20MHz
    // SPI, see storage.cpp).
    server.client().setNoDelay(true);

    // Deliberately not server.streamFile(): it blocks until the whole
    // transfer is done with no way for us to pet the watchdog in
    // between, and a multi-MB run CSV over WiFi — especially through
    // this device's own AP with a marginal signal, exactly the situation
    // right after finishing a ride — can easily take longer than the 8s
    // watchdog timeout (see main.cpp), rebooting the device mid-download
    // and killing the connection. This does the same transfer manually,
    // chunk by chunk, so the loop below can reset the watchdog as it goes.
    //
    // 16KB (not a stack array) — this handler runs on the main loop()
    // task, whose default stack is small enough that the old 2KB buffer
    // was already a meaningful slice of it; the shared static buffer above
    // moves this off the stack entirely, into .bss, so it can be sized for
    // fewer read/send round trips (the real lever on download time)
    // without touching stack headroom at all.
    int n;

    while ((n = file.read(downloadBuf, sizeof(downloadBuf))) > 0) {
        server.sendContent((const char*)downloadBuf, n);
        esp_task_wdt_reset();
    }

    file.close();
}

// Serves a run's compact binary telemetry (see writeCompactCsv() in
// run_compact.cpp for the format and why it exists) instead of the raw
// .CSV — run.html/compare.html/trends.html's own charts only ever needed
// 12 of the .CSV's 17 columns anyway, and re-encoding those as scaled
// fixed-point integers instead of ASCII decimal text cuts what actually
// has to cross WiFi (the real bottleneck a download-speed investigation
// found — not SD, not chunk size/Nagle's algorithm, all already fixed
// above) by roughly 4x. ensureCompactCsv() generates the file on first
// request for any run that predates this feature, so this always has
// something to serve.
void handleApiRunTelemetry() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    if (!RunService::ensureCompactCsv(name)) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    File file = storageGet(("/" + name + ".CSVB").c_str());

    if (!file) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    server.setContentLength(file.size());
    server.beginResponse(200, "application/octet-stream");
    server.client().setNoDelay(true);

    int n;

    while ((n = file.read(downloadBuf, sizeof(downloadBuf))) > 0) {
        server.sendContent((const char*)downloadBuf, n);
        esp_task_wdt_reset();
    }

    file.close();
}

// Bundles a run's .CSV and .JSON into a single .zip so the rider only
// needs one download instead of two — see zip_writer.h for how the
// archive itself gets built.
void handleApiRunDownloadZip() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing name");
        return;
    }

    String name = server.arg("name");

    std::vector<ZipEntry> entries = {
        { "/" + name + ".CSV", name + ".CSV" },
        { "/" + name + ".JSON", name + ".JSON" }
    };

    sendZipResponse(server, entries, name + ".zip");
}

void handleApiRunDelete() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err || !doc["name"].is<const char*>()) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String name = doc["name"].as<String>();

    bool ok = RunService::remove(name);

    if (!ok) {
        server.send(500, "text/plain", "Delete failed");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiRunNotes() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err || !doc["name"].is<const char*>()) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String name = doc["name"].as<String>();

    // Two rider-editable fields share this endpoint, and only the ones
    // actually sent are touched — renaming a run must not blank its notes.
    bool hasNotes = doc["notes"].is<const char*>();
    bool hasTitle = doc["title"].is<const char*>();

    if (!hasNotes && !hasTitle) {
        server.send(400, "text/plain", "Nothing to update");
        return;
    }

    bool ok = true;
    if (hasNotes) ok = RunService::setNotes(name, doc["notes"].as<String>()) && ok;
    if (hasTitle) ok = RunService::setTitle(name, doc["title"].as<String>()) && ok;

    if (!ok) {
        server.send(404, "text/plain", "Run not found");
        return;
    }

    server.send(200, "text/plain", "OK");
}

// Trends' one-time fill-in for a run recorded before the device counted its
// bottom-outs: { name, front, rear }, counted from the run's telemetry with
// the same rule the device now uses while recording.
void handleApiRunBottomOuts() {
    JsonDocument doc;

    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String name = doc["name"] | "";
    bool nameOk = name.length() > 0 && name.length() <= 16;
    for (size_t i = 0; nameOk && i < name.length(); i++)
        nameOk = isalnum((unsigned char)name[i]);

    if (!nameOk || !doc["front"].is<unsigned int>() || !doc["rear"].is<unsigned int>() ||
        doc["front"].as<unsigned int>() > 9999 || doc["rear"].as<unsigned int>() > 9999) {
        server.send(400, "text/plain", "Expected name, front, rear");
        return;
    }

    if (!RunService::setBottomOuts(name, doc["front"].as<unsigned int>(), doc["rear"].as<unsigned int>())) {
        server.send(404, "text/plain", "Run not found, or it has no stats");
        return;
    }

    server.send(200, "text/plain", "OK");
}

// The fields a run's recorded setup can be corrected in, and the most each
// may be. The names are the ones the run's .JSON uses — and the pointers are
// what RunService::setSetupFields() is given, so they outlive the request.
struct SetupFieldSpec {
    const char* name;
    float max;          // 0: a true/false field
};

static const SetupFieldSpec SETUP_FIELDS[] = {
    { "lsc", 255 }, { "hsc", 255 }, { "lsr", 255 }, { "hsr", 255 },
    { "volume_tokens", 255 }, { "pressure_psi", 1000 }, { "lockout", 0 }
};

// One field and its value, checked against the table: false if either is
// not acceptable.
static bool setupCorrection(const char* name, JsonVariantConst value, RunService::SetupCorrection& out) {
    // The fork's or shock's model: text, trimmed, at most as long as
    // Settings take it; an empty one takes it off the run.
    if (strcmp(name, "model") == 0) {
        if (!value.is<const char*>()) return false;

        String model = value.as<String>();
        model.trim();
        if (model.length() > MAX_SUSPENSION_MODEL_LEN) return false;

        out = { "model", 0, model };
        return true;
    }

    for (const SetupFieldSpec& spec : SETUP_FIELDS) {
        if (strcmp(spec.name, name) != 0) continue;

        if (spec.max == 0) {
            if (!value.is<bool>()) return false;
            out = { spec.name, value.as<bool>() ? 1.0f : 0.0f };
            return true;
        }

        if (!value.is<float>()) return false;

        float v = value.as<float>();
        if (v < 0 || v > spec.max) return false;

        out = { spec.name, v };
        return true;
    }

    return false;
}

// Corrects the setup a run was recorded with, in that run's .JSON only —
// Settings and the bike's profile are not touched. Body: { name, end:
// "front"|"rear", fields: { lsc: 3, pressure_psi: 235, model: "...", ... } }, applied
// together or not at all (one unknown field or out-of-range value refuses
// the lot). The older { name, end, field, value } form, one field, still
// works.
void handleApiRunSetup() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err || !doc["name"].is<const char*>()) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    String end = doc["end"] | "";

    if (end != "front" && end != "rear") {
        server.send(400, "text/plain", "Unknown end");
        return;
    }

    std::vector<RunService::SetupCorrection> corrections;

    if (doc["fields"].is<JsonObjectConst>()) {
        for (JsonPairConst kv : doc["fields"].as<JsonObjectConst>()) {
            RunService::SetupCorrection c;
            if (!setupCorrection(kv.key().c_str(), kv.value(), c)) {
                server.send(400, "text/plain", String("Invalid field or value: ") + kv.key().c_str());
                return;
            }
            corrections.push_back(c);
        }
    } else {
        RunService::SetupCorrection c;
        if (!setupCorrection(doc["field"] | "", doc["value"], c)) {
            server.send(400, "text/plain", "Invalid field or value");
            return;
        }
        corrections.push_back(c);
    }

    if (corrections.empty()) {
        server.send(400, "text/plain", "Nothing to correct");
        return;
    }

    if (!RunService::setSetupFields(doc["name"].as<String>(), end == "rear", corrections)) {
        server.send(404, "text/plain", "Run or its setup not found");
        return;
    }

    server.send(200, "text/plain", "OK");
}

void handleApiRunsClear() {
    bool ok = RunService::removeAll();

    if (!ok) {
        server.send(500, "text/plain", "Delete failed");
        return;
    }

    server.send(200, "text/plain", "OK");
}
