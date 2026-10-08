#pragma once
#include <Arduino.h>

struct RunInfo {
    String name;
    String bikeName;

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