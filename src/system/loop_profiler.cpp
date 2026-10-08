#include "loop_profiler.h"

namespace {

const char* const SECTION_NAMES[LoopProfiler::SECTION_COUNT] = {
    "wait", "suspension", "logger", "system", "led", "battery", "gps", "wifi", "ota", "server"
};

// Spacing between consecutive rows, in us — upper edge of each bin. Fine
// around 2000us, which is where every row belongs at 500Hz: the middle bins
// say how tightly the samples keep to the clock, the outer ones count the
// rows that came late.
constexpr uint32_t DT_EDGES_US[] = { 1500, 1900, 1950, 1990, 2010, 2050, 2100, 2500, 5000, 20000, 50000 };
constexpr int DT_BINS = sizeof(DT_EDGES_US) / sizeof(DT_EDGES_US[0]) + 1;

const char* const DT_LABELS[DT_BINS] = {
    "<1.5ms", "<1.9ms", "<1.95ms", "<1.99ms", "<2.01ms", "<2.05ms", "<2.1ms", "<2.5ms", "<5ms", "<20ms", "<50ms", ">=50ms"
};

struct Window {
    bool recording = false;
    uint32_t startMs = 0;
    uint32_t endMs = 0;

    uint32_t passes = 0;
    uint64_t busyUs = 0;            // inside loop(), not counting the wait
    uint64_t periodUs = 0;          // start of one pass to the start of the next
    uint32_t maxBusyUs = 0;

    uint64_t sectionUs[LoopProfiler::SECTION_COUNT] = {};
    uint32_t sectionMaxUs[LoopProfiler::SECTION_COUNT] = {};

    uint32_t rows = 0;
    uint32_t firstRowUs = 0;
    uint32_t lastRowUs = 0;
    uint32_t minDtUs = UINT32_MAX;
    uint32_t maxDtUs = 0;
    uint32_t dtHist[DT_BINS] = {};
};

Window current;
Window lastRecording;
bool haveLastRecording = false;

// The pass in progress.
uint32_t passStartUs = 0;
uint32_t prevPassStartUs = 0;
bool havePrevPass = false;
uint32_t markUs = 0;
uint32_t lapUs[LoopProfiler::SECTION_COUNT] = {};

void writeWindow(JsonObject out, const Window& w, uint32_t endMs) {
    out["recording"] = w.recording;
    out["ms"] = endMs - w.startMs;
    out["passes"] = w.passes;

    if (w.passes == 0)
        return;

    out["pass_hz"] = serialized(String(w.periodUs > 0 ? w.passes * 1e6f / (float)w.periodUs : 0.0f, 1));
    out["avg_busy_us"] = serialized(String((float)w.busyUs / w.passes, 1));
    out["max_busy_us"] = w.maxBusyUs;

    // Per section: average over every pass, the worst single pass, and its
    // share of the pass (the wait included, so the shares add up to the
    // whole period and "wait" is the headroom).
    uint64_t totalUs = 0;
    for (int i = 0; i < LoopProfiler::SECTION_COUNT; i++)
        totalUs += w.sectionUs[i];

    JsonObject sections = out["sections"].to<JsonObject>();

    for (int i = 0; i < LoopProfiler::SECTION_COUNT; i++) {
        JsonObject s = sections[SECTION_NAMES[i]].to<JsonObject>();
        s["avg_us"] = serialized(String((float)w.sectionUs[i] / w.passes, 1));
        s["max_us"] = w.sectionMaxUs[i];
        s["pct"] = serialized(String(totalUs > 0 ? 100.0f * (float)w.sectionUs[i] / (float)totalUs : 0.0f, 1));
    }

    out["rows"] = w.rows;

    if (w.rows < 2)
        return;

    uint32_t spanUs = w.lastRowUs - w.firstRowUs;

    out["row_hz"] = serialized(String(spanUs > 0 ? (w.rows - 1) * 1e6f / (float)spanUs : 0.0f, 1));

    JsonObject dt = out["row_dt_us"].to<JsonObject>();
    dt["min"] = w.minDtUs;
    dt["avg"] = spanUs / (w.rows - 1);
    dt["max"] = w.maxDtUs;

    JsonObject hist = out["row_dt_hist"].to<JsonObject>();
    for (int i = 0; i < DT_BINS; i++)
        hist[DT_LABELS[i]] = w.dtHist[i];
}

} // namespace

void LoopProfiler::passStart() {
    passStartUs = micros();
    markUs = passStartUs;

    for (uint32_t& us : lapUs) us = 0;
}

void LoopProfiler::lap(Section section) {
    uint32_t now = micros();
    lapUs[section] += now - markUs;
    markUs = now;
}

uint32_t LoopProfiler::passEnd(bool recording) {
    uint32_t busyUs = (micros() - passStartUs) - lapUs[WAIT];

    // A change of state closes the window. The pass it happened on (the one
    // that opened or closed the run's files, and so took tens of ms) is left
    // out of both sides.
    if (recording != current.recording) {
        if (current.recording) {
            current.endMs = millis();
            lastRecording = current;
            haveLastRecording = true;
        }

        current = Window{};
        current.recording = recording;
        current.startMs = millis();
        havePrevPass = false;

        return busyUs;
    }

    current.passes++;
    current.busyUs += busyUs;
    if (busyUs > current.maxBusyUs) current.maxBusyUs = busyUs;

    if (havePrevPass)
        current.periodUs += passStartUs - prevPassStartUs;

    prevPassStartUs = passStartUs;
    havePrevPass = true;

    for (int i = 0; i < SECTION_COUNT; i++) {
        current.sectionUs[i] += lapUs[i];
        if (lapUs[i] > current.sectionMaxUs[i]) current.sectionMaxUs[i] = lapUs[i];
    }

    return busyUs;
}

const char* LoopProfiler::worstSection(uint32_t& us) {
    int worst = SUSPENSION;

    for (int i = 0; i < SECTION_COUNT; i++) {
        if (i != WAIT && lapUs[i] > lapUs[worst])
            worst = i;
    }

    us = lapUs[worst];
    return SECTION_NAMES[worst];
}

void LoopProfiler::rowLogged() {
    uint32_t now = micros();

    if (current.rows == 0) {
        current.firstRowUs = now;
    } else {
        uint32_t dt = now - current.lastRowUs;

        if (dt < current.minDtUs) current.minDtUs = dt;
        if (dt > current.maxDtUs) current.maxDtUs = dt;

        int bin = 0;
        while (bin < DT_BINS - 1 && dt >= DT_EDGES_US[bin]) bin++;
        current.dtHist[bin]++;
    }

    current.lastRowUs = now;
    current.rows++;
}

void LoopProfiler::writeJson(JsonObject out) {
    writeWindow(out["current"].to<JsonObject>(), current, millis());

    if (haveLastRecording)
        writeWindow(out["last_recording"].to<JsonObject>(), lastRecording, lastRecording.endMs);
}
