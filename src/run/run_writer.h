#pragma once
// The run's SD writer (run_writer.cpp), as run_service.cpp drives it. Internal
// to src/run/.
//
// A run, in order: open() the run's CSV, takeSpill() once everything else the
// run needs is allocated, push() every sample, then drain(), close(), and
// report() for the run's .JSON.
#include <Arduino.h>

// One sample on its way to the card. isDrainMarker is drain()'s own: the row
// that tells the writer task the run is ending.
struct LogRow {
    bool isDrainMarker;

    uint32_t t;
    float lat, lon, speed;
    int sats;
    float hdop, alt, pitch, roll, yaw;
    float ax, ay, az;
    float front_travel, rear_travel;
};

namespace RunWriter
{
    // Once, at boot, before WiFi takes the heap: the pending-row buffer, the
    // queue and the writer task.
    void init();

    // Creates the run's CSV with its header and resets the run's counters.
    // False if the file cannot be opened.
    bool open(const String& runName);

    // The spill buffer behind the queue, while the radio is off. Last in a
    // run's start: it takes what heap is left.
    void takeSpill();

    // From the sampling loop; never blocks. A row with nowhere to go is
    // counted in rowsDropped.
    void push(const LogRow& row);

    // Waits for every row pushed so far to be written (up to 3s), then gives
    // the spill buffer back.
    void drain();

    // Closes the run's CSV. After drain().
    void close();

    // What the run's .JSON says about the writing.
    struct Report {
        bool failed;              // rows were lost on the card side
        uint32_t rowsDropped;     // never reached the writer: queue and spill full
        uint32_t rowsLost;        // the writer took them and never got them onto the card
        uint32_t sdRetryOk, sdRecovered;
        int sdLastErrno;
        uint32_t sdLastGapBytes, sdRecoverMaxMs;
        uint32_t sdReopenFailed, sdStatFailed, sdReplayFailed;
        uint32_t spillRows, spillMax, heapFreeRecording;
    };
    Report report();

    // millis() of the last row the writer finished with: if it stops moving
    // while recording, the writer is stuck (see isWriteStalled()).
    uint32_t lastWriteMs();

    // Rows were lost on the card side this run.
    bool failed();
}
