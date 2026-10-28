#pragma once
#include <Arduino.h>
#include <vector>
#include "run_types.h"

namespace RunService
{
    // Creates the SD-writer task and its queue. Call once at boot, after
    // storage is initialized — before any run can start.
    void init();

    // Starts a run now. With "WiFi off while recording" this takes the radio
    // down first, so it must not be called from a web request that still
    // has an answer to send — that is what startSoon() is for.
    bool start();
    void stop();

    // stop(), with "stopped_by": "low_battery" in the run's .JSON: the
    // low-battery cutoff is about to put the device to sleep.
    void stopForLowBattery();

    // For a start asked for over the network: schedules it for half a second
    // from now, so that the request can be answered before the radio goes,
    // and returns the name the run will have ("" if the device isn't idle).
    // timedSeconds > 0 makes it a timed run (see startTimed()). Until it
    // happens, startIsPending() is true and currentName() is that name.
    String startSoon(uint32_t timedSeconds = 0);
    bool startIsPending();
    void cancelPendingStart();

    // A run that ends by itself after `seconds`. For measuring the device
    // without someone standing at its button: with "WiFi off while
    // recording" a run started over the API drops off the network half a
    // second later, so the API can't be what stops it. Refused unless the
    // device is idle. Stopping it earlier by hand works as for any run.
    bool startTimed(uint32_t seconds);

    // Called on every pass of loop(): carries out a start scheduled with
    // startSoon(), and ends a timed run when its time is up.
    void update();

    // Scans the SD card for .CSV files with no matching .JSON — a run that
    // never got a clean RunService::stop() call, most likely because power
    // was lost mid-recording — and reconstructs a best-effort .JSON for
    // each so it still shows up (marked "recovered") instead of vanishing
    // from the Runs list. Call once at boot, before any new run can start.
    void recoverOrphanedRuns();

    void log(
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
        float rear_travel
    );

    // The runs on the card, newest first, by name only ("RUN0047") — read
    // from the directory without opening a file.
    std::vector<String> names();

    // The same runs, each with the stamp of its .JSON (see RunStamp). Also
    // one pass over the directory and no file opened: the directory holds
    // each file's size and when it was last written.
    std::vector<RunStamp> stamps();

    // What one of those runs' .JSON says about it. A file read off the card
    // each time: callers ask for a page of runs, never the whole card (see
    // handleApiRuns() in server.cpp).
    RunInfo info(const String& name);

    // Ensures a compact binary companion file exists for this run's CSV
    // (see writeCompactCsv() in run_service.cpp for the format and why it
    // exists), generating it now if this run predates the feature.
    // Returns false only if the run's .CSV itself doesn't exist. Called by
    // the /api/runs/telemetry download handler in server.cpp before
    // serving it — the only other caller is stop() itself, right after a
    // fresh recording finishes.
    bool ensureCompactCsv(const String& name);

    bool remove(const String& name);
    bool removeAll();

    // Overwrites just the free-text "notes" field of an already-finished
    // run's .JSON (see RunInfo::notes) — used by run.html's Notes box.
    // Returns false if the run's .JSON doesn't exist or can't be parsed.
    bool setNotes(const String& name, const String& notes);

    // Same, for the optional name a rider gives a run (see RunInfo::title).
    bool setTitle(const String& name, const String& title);

    // Corrects fields of the setup a finished run was recorded with: click
    // counts, "volume_tokens", "pressure_psi", or "lockout" (see RunSetup).
    // The snapshot is whatever Settings held when the run ended, which is not
    // always what was on the bike. All of them in one rewrite of the .JSON.
    //
    // The first time a field other than "lockout" is corrected, the value
    // the run was recorded with is kept under the setup's "recorded", so a
    // correction never loses what was really written down; correcting it
    // back to that value takes it out again. The lockout is a mark on the
    // run, not a correction of a setting, and keeps no original. "model" is
    // the one text field (in `text`, an empty one taking the model out); a
    // model put on a run that was recorded without one keeps no original
    // either, since nothing was written down to lose.
    //
    // `field` must point at a string that outlives the call (the caller's
    // table of known names). The caller checks the names and the values'
    // ranges. Returns false if the run's .JSON doesn't exist, can't be
    // parsed, or has no setup for that end.
    struct SetupCorrection {
        const char* field;
        float value;
        String text;    // "model" only
    };

    bool setSetupFields(const String& name, bool rear, const std::vector<SetupCorrection>& fields);

    String currentName();
    uint32_t elapsedSeconds();

    // True if we're recording but no row has actually reached the SD
    // writer task in a suspiciously long time — the queue between the
    // sampling loop and that task absorbs brief stalls invisibly by
    // design, so this only fires when something is seriously wrong (the
    // writer task hung inside a blocking SD call, observed in the field
    // after what looked like a hard impact jarred the card's contact).
    // Checking this also latches the run's own "went bad" flag, written
    // into its .JSON as "incomplete" once it stops — see writeRunMeta().
    // Also stays true for the rest of the run once rows were genuinely
    // lost to an SD write failure. A failure that was recovered by
    // reopening the file and replaying the buffered rows does not count —
    // it only shows up in the run's sd_recovered/sd_retry_ok counters.
    // See writeLogRow()/tryRecover().
    bool isWriteStalled();
}