#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Where loop() spends its time, measured on the device and read back over
// the API (GET /api/debug/loop) — the device normally has no serial cable
// attached, and while it records it has no WiFi either, so the figures for a
// recording are kept until it is over.
//
// One pass of loop() is one sample period: it waits for the sampling clock,
// takes the sample, and does everything else in what is left. So "busy" —
// the pass without its wait — is how much of each period the firmware
// actually needs, and what is left over is the headroom. A pass whose busy
// part is longer than the period makes the next sample late; this is what
// says which call did it. The cost of keeping it is a micros() per section
// and a handful of additions per pass.
//
// Figures are kept per "window": the stretch between two changes of the
// recording state. The last finished recording stays available alongside
// the window in progress.
namespace LoopProfiler
{
    enum Section : uint8_t {
        WAIT,           // idling until the next sample is due
        SUSPENSION,     // the ADC reads
        LOGGER,         // handing the row to the SD writer
        SYSTEM,         // state machine + button
        LED,
        BATTERY,
        GPS,
        WIFI,
        OTA,
        SERVER,
        SECTION_COUNT
    };

    // Bracket one pass of loop(): passStart(), a lap() after each section's
    // work, passEnd(). passEnd() returns the busy part of the pass, in us.
    void passStart();
    void lap(Section section);
    uint32_t passEnd(bool recording);

    // The section (other than the wait) that took longest in the pass that
    // has just ended.
    const char* worstSection(uint32_t& us);

    // One row handed to the SD writer: the spacing between these is the
    // sampling rate actually achieved.
    void rowLogged();

    void writeJson(JsonObject out);
}
