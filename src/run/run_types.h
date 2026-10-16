#pragma once
#include <Arduino.h>

// One end's suspension setup as a run's .JSON recorded it (the snapshot of
// Settings writeRunMeta() takes when the run ends).
struct RunSetup {
    // False when the run's .JSON has no such block, or one from before
    // compression and rebound were split into four dials.
    bool recorded = false;

    uint8_t lsc = 0;
    uint8_t hsc = 0;
    uint8_t lsr = 0;
    uint8_t hsr = 0;
    uint8_t volumeTokens = 0;
    float pressurePsi = 0;
    float sagPct = 0;

    // The run was ridden with this end locked out (the shock's Threshold
    // lever). Never part of the snapshot taken from Settings — a lever left
    // closed is only noticed once the run is done — so it is only ever set
    // afterwards, on the run itself (RunService::setSetupField()).
    bool lockout = false;

    // Runs recorded before the has_* flags existed have none, and count as
    // having every dial — the call adjusterAvailability() makes in
    // telemetry.js for the same runs.
    bool hasLSC = true;
    bool hasHSC = true;
    bool hasLSR = true;
    bool hasHSR = true;
};

// A run and a stamp that changes whenever its .JSON does: the file's size,
// modification date and modification time as the card's directory has them
// ("1143.23882.36582"). Empty for a run that has no .JSON yet. The dashboard
// keeps what it has read of a run for as long as the stamp stays the same
// (fetchAllRuns() in common.js).
struct RunStamp {
    String name;
    String stamp;
};

struct RunInfo {
    String name;
    String bikeName;

    // The setup each end was ridden with. There is no log of setup changes:
    // the dashboard works the history out from how this differs from one run
    // to the next (setupHistory() in telemetry.js).
    RunSetup frontSetup;
    RunSetup rearSetup;

    size_t sizeBytes = 0;

    float duration_s = 0;

    // When the run started, as UTC seconds since the Unix epoch — 0 if the
    // device didn't know the time when the run ended (see clock.h), which
    // is also the case for every run recorded before this field existed.
    uint32_t startUtc = 0;

    float frontTravelMM = 0;
    float rearTravelMM = 0;
    bool fullSuspension = true;

    // Headline numbers for the Runs list, copied from the "stats" block
    // writeRunMeta() already puts in each run's .JSON. hasStats is false
    // for runs with no such block (recovered after a power loss, or
    // stopped before a single sample) — the rest are meaningless then.
    bool hasStats = false;
    float distanceKm = 0;
    float maxSpeedKmh = 0;
    float maxFrontMM = 0;
    float maxRearMM = 0;

    // True when this run's .JSON was reconstructed at boot because the
    // device never got to write it cleanly (e.g. power loss mid-recording)
    // — its bike/setup fields reflect current Settings, not necessarily
    // what was actually configured during that ride. See
    // RunService::recoverOrphanedRuns().
    bool recovered = false;

    // True when the SD-writer task was ever detected stuck (no row
    // written for several seconds) during this run — the CSV is real up
    // to that point, but likely stops well short of the ride's actual
    // duration. See RunService::isWriteStalled().
    bool incomplete = false;

    // True when not a single sample during this run had a valid GPS fix
    // (lat/lon both non-zero) — no signal indoors, under heavy cover, or
    // with an antenna fault. The CSV/travel data is still fine, but the
    // map, distance, speed and elevation figures on run.html will all be
    // empty or meaningless. See RunStatsAccumulator::gpsFixSeen.
    bool noGpsFix = false;

    // Samples that were taken but never reached the SD card, because the
    // writer had fallen too far behind to take them (see rowsDropped in
    // run_service.cpp). The run is complete in time but has gaps. 0 on any
    // run recorded before the count was kept.
    uint32_t rowsDropped = 0;

    // Samples the writer took and never got onto the card (see rowsLost in
    // run_service.cpp), and the samples that did get there. Only runs
    // recorded since 2026-10-11 have a rows_lost; hasRowsLost tells those
    // apart from a run that simply lost nothing. With rowsDropped and the
    // duration they say how much of a run is missing (runMissingSeconds()
    // in telemetry.js).
    bool hasRowsLost = false;
    uint32_t rowsLost = 0;
    uint32_t rowsWritten = 0;

    // Free-text field a rider can fill in after the ride (run.html) — e.g.
    // "rear bottomed out ~1:30, felt harsh over roots" — so what actually
    // happened is on record instead of relying on memory once it's time to
    // compare against the app's own analysis. Empty until explicitly set
    // via RunService::setNotes(); never written by writeRunMeta() itself.
    String notes;

    // Optional name the rider gives the run ("Trail 4, new rebound") so it
    // can be told apart by more than its RUN number. Empty until set via
    // RunService::setTitle(); the file names on the SD card never change.
    String title;
};