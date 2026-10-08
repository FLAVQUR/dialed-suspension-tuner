#include <Arduino.h>
#include <Preferences.h>
#include "suspension.h"
#include "config/pins.h"
#include "system/settings.h"
#include "system/fast_adc.h"

// ─── NVS keys ────────────────────────────────────────────────────────────────
static constexpr const char* NVS_NS         = "suspension";
static constexpr const char* NVS_F_ZERO     = "f_zero";
static constexpr const char* NVS_F_MAX_RAW  = "f_maxraw";
static constexpr const char* NVS_R_ZERO     = "r_zero";
static constexpr const char* NVS_R_MAX_RAW  = "r_maxraw";

// Legacy keys — maxMM used to live in this module's own NVS namespace
// alongside zero/max raw. It moved to settings.frontMaxTravelMM/
// rearStrokeMM so a bike profile can carry it (see bikes.cpp). Kept here,
// read-only, purely so migrateLegacyMaxMM() can rescue an existing
// install's already-captured value the first time it boots on this
// firmware version — without this, that value would silently read back as
// 0 and zero out every %-travel calculation until manually re-entered.
static constexpr const char* NVS_F_MAX_MM_LEGACY = "f_maxmm";
static constexpr const char* NVS_R_MAX_MM_LEGACY = "r_maxmm";

// suspensionUpdate() is called every loop() iteration, which can spin at an
// unpredictable rate — sampling this often makes the EMA filter below
// meaningless (it converges to raw noise almost instantly). The ADC is
// sampled on a clock of its own instead (settings.suspensionSampleRateHz,
// see suspensionUpdate()), and each sample averages a few raw reads to cut
// short-term ADC noise, same approach as battery.cpp.
//
// Reads are in calibrated millivolts (fast_adc.h — the same value as
// analogReadMilliVolts(), which this used to call): the chip's factory eFuse
// calibration linearises the ESP32 ADC and makes the reading comparable
// across boards. The calibration below therefore stores millivolts, not raw
// 0-4095 counts.
static constexpr int OVERSAMPLE_COUNT = 4;

static uint32_t samplePeriodUs() {
    uint16_t hz = settings.suspensionSampleRateHz;
    return hz > 0 ? (1000000UL / hz) : 4000;  // 4ms (250Hz) fallback, should never hit
}

// Samples taken since boot, and when the latest one was due. The logger
// writes one row per sample and stamps it with that time.
static uint32_t sampleCount = 0;
static uint32_t sampleTimeMs = 0;

// A deeper-than-calibrated reading must hold continuously for this long
// before it's trusted as a real bottom-out and not an ADC glitch — it gets
// baked into the calibration permanently, so it's worth a moment of
// confirmation. Expressed as a duration, not a sample count: a fixed
// sample count shrinks in real time as settings.suspensionSampleRateHz goes
// up (3 samples was ~15ms at the old fixed 200Hz, but only 3ms at 1000Hz —
// short enough for a brief electrical glitch to get baked in as the new
// max permanently, which is exactly what corrupted a calibration in the
// field).
static constexpr uint32_t EXTEND_CONFIRM_MS = 15;

// A reading has to beat the calibrated max by more than this before it's
// even considered a candidate bottom-out. Without a margin, ordinary ADC/
// mechanical noise sitting right at the just-captured max point — a single
// count deeper, with nothing physically deeper happening — satisfies
// EXTEND_CONFIRM_MS just as easily as a real hit and silently creeps maxMM
// upward on its own right after calibration.
static constexpr float EXTEND_MARGIN_MM = 1.0f;

// Rate-limit how often an auto-extended max gets written to flash — several
// bottom-outs in the same ride shouldn't hammer NVS.
static constexpr uint32_t MAX_PERSIST_INTERVAL_MS = 2000;

// ─── Internal state ───────────────────────────────────────────────────────────
struct SuspensionState {
    float    filtered   = 0.0f;
    int      currentRaw = 0;
    SuspensionCalibration cal = {};

    bool     extremePending  = false;
    uint32_t extremeSinceMs  = 0;
    uint32_t lastMaxPersistMs = 0;
};

static SuspensionState front;
static SuspensionState rear;
static Preferences prefs;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static float applyFilter(float newVal, float prev) {
    return prev * 0.85f + newVal * 0.15f;
}

static int readRaw(SuspensionSide side) {
    long sum = 0;

    for (int i = 0; i < OVERSAMPLE_COUNT; i++) {
        sum += (side == FRONT)
            ? fastAdcReadMilliVolts(PIN_SUSPENSION_FRONT)
            : fastAdcReadMilliVolts(PIN_SUSPENSION_REAR);
    }

    return sum / OVERSAMPLE_COUNT;
}

static SuspensionState& getState(SuspensionSide side) {
    return (side == FRONT) ? front : rear;
}

// ─── NVS ──────────────────────────────────────────────────────────────────────
static void nvsLoad() {
    prefs.begin(NVS_NS, true);  // read-only

    front.cal.zeroSet = prefs.isKey(NVS_F_ZERO);
    front.cal.zeroRaw = prefs.getInt(NVS_F_ZERO, 0);
    front.cal.maxSet  = prefs.isKey(NVS_F_MAX_RAW);
    front.cal.maxRaw  = prefs.getInt(NVS_F_MAX_RAW, 0);

    rear.cal.zeroSet = prefs.isKey(NVS_R_ZERO);
    rear.cal.zeroRaw = prefs.getInt(NVS_R_ZERO, 0);
    rear.cal.maxSet  = prefs.isKey(NVS_R_MAX_RAW);
    rear.cal.maxRaw  = prefs.getInt(NVS_R_MAX_RAW, 0);

    prefs.end();

    front.cal.valid = front.cal.zeroSet && front.cal.maxSet;
    rear.cal.valid  = rear.cal.zeroSet  && rear.cal.maxSet;
}

static void nvsSaveZero(SuspensionSide side) {
    auto& s = getState(side);

    prefs.begin(NVS_NS, false);
    prefs.putInt(side == FRONT ? NVS_F_ZERO : NVS_R_ZERO, s.cal.zeroRaw);
    prefs.end();
}

static void nvsSaveMax(SuspensionSide side) {
    auto& s = getState(side);

    prefs.begin(NVS_NS, false);
    prefs.putInt(side == FRONT ? NVS_F_MAX_RAW : NVS_R_MAX_RAW, s.cal.maxRaw);
    prefs.end();
}

// One-time rescue for an install upgrading from before maxMM moved out of
// this module's NVS namespace (see the NVS_*_MAX_MM_LEGACY comment above).
// Only adopts the legacy value while settings.* is still unset, so it never
// overwrites a value the rider has since (re)configured via Bike
// Information or a bike profile.
static void migrateLegacyMaxMM() {
    prefs.begin(NVS_NS, true);
    float legacyFrontMM = prefs.getFloat(NVS_F_MAX_MM_LEGACY, 0.0f);
    float legacyRearMM  = prefs.getFloat(NVS_R_MAX_MM_LEGACY, 0.0f);
    prefs.end();

    bool changed = false;

    if (settings.frontMaxTravelMM <= 0.0f && legacyFrontMM > 0.0f) {
        settings.frontMaxTravelMM = legacyFrontMM;
        changed = true;
    }

    if (settings.rearStrokeMM <= 0.0f && legacyRearMM > 0.0f) {
        settings.rearStrokeMM = legacyRearMM;
        changed = true;
    }

    if (changed) settingsSave();
}

// ─── Public API ──────────────────────────────────────────────────────────────
void suspensionInit() {
    nvsLoad();
    migrateLegacyMaxMM();

    fastAdcAttach(PIN_SUSPENSION_FRONT);
    fastAdcAttach(PIN_SUSPENSION_REAR);

    // Initialize the filter with the current ADC value
    front.currentRaw = readRaw(FRONT);
    front.filtered   = front.currentRaw;

    rear.currentRaw  = readRaw(REAR);
    rear.filtered    = rear.currentRaw;

    Serial.printf("Suspension init: front cal=%s  rear cal=%s\n",
        front.cal.valid ? "OK" : "UNCAL",
        rear.cal.valid  ? "OK" : "UNCAL");
}

// The sample clock. A sample is due every samplePeriodUs(), counted from when
// the previous one was *due* — not from when it was actually taken. loop()
// only notices a sample is due when it next comes round to this function,
// which is always a little late; measuring the next period from that late
// moment (as this did, in whole milliseconds, with the logger doing the same
// on a clock of its own) let the lateness pile up, and "500Hz" came out as
// 375 rows/s on a good day. Measured from the due time, a late sample makes
// the next gap shorter and the average rate is exactly the configured one,
// for as long as a pass of loop() takes less than one period.
//
// On the esp_timer clock in 64 bits rather than micros(), which wraps every
// 71 minutes: the sample's time has to stay on the same scale as millis()
// for the whole of a long ride.
static int64_t dueUs = 0;
static bool clockStarted = false;

void suspensionWaitUntilDue() {
    if (!clockStarted) return;

    // By construction the next sample is never more than a period away.
    // Bounded all the same, so that nothing wrong with the clock could ever
    // turn into a loop() that doesn't come back.
    const int64_t giveUpUs = esp_timer_get_time() + samplePeriodUs();

    for (;;) {
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs >= dueUs || nowUs >= giveUpUs) return;
    }
}

void suspensionUpdate() {
    const int64_t nowUs = esp_timer_get_time();
    const int64_t periodUs = samplePeriodUs();

    if (!clockStarted) {
        dueUs = nowUs;
        clockStarted = true;
    }

    if (nowUs < dueUs) return;

    // The sample stands for the moment it was due at; it is read up to one
    // pass of loop() later than that.
    int64_t sampleUs = dueUs;
    dueUs += periodUs;

    // A whole period late or more means loop() was held up (the WiFi radio
    // shutting down as a run starts, a file being opened). Start the clock
    // again from here: catching up would only produce a burst of samples
    // that all read this same instant.
    if (nowUs >= dueUs) {
        sampleUs = nowUs;
        dueUs = nowUs + periodUs;
    }

    sampleTimeMs = (uint32_t)(sampleUs / 1000);
    sampleCount++;

    for (SuspensionSide side : {FRONT, REAR}) {
        auto& s = getState(side);

        int raw = readRaw(side);
        s.currentRaw = raw;
        s.filtered   = applyFilter((float)raw, s.filtered);

        if (!s.cal.valid) continue;

        bool rawIncreasesWithMm = s.cal.maxRaw > s.cal.zeroRaw;

        float span = s.cal.maxRaw - s.cal.zeroRaw;
        float maxMM = suspensionGetMaxTravelMM(side);
        float marginRaw = (maxMM > 0.01f) ? fabsf(span) / maxMM * EXTEND_MARGIN_MM : 0.0f;

        bool deeper = rawIncreasesWithMm
            ? (raw > s.cal.maxRaw + marginRaw)
            : (raw < s.cal.maxRaw - marginRaw);

        if (!deeper) {
            s.extremePending = false;
            continue;
        }

        if (!s.extremePending) {
            s.extremePending = true;
            s.extremeSinceMs = millis();
            continue;
        }

        if (millis() - s.extremeSinceMs < EXTEND_CONFIRM_MS)
            continue;

        // Only maxRaw (the electrical calibration point) auto-extends here.
        // maxMM is the rider-declared travel spec (e.g. "170mm") — it's
        // shown throughout the UI as a fixed fact about the bike and must
        // never drift on its own just because a deeper voltage was
        // observed. A reading beyond the old maxRaw now simply means that
        // same declared maxMM is reached a little sooner (t=1.0 shifts to
        // whatever raw value is deepest so far) — the configured number
        // itself stays exactly what the rider typed in.
        s.cal.maxRaw = raw;
        s.extremePending = false;

        if (millis() - s.lastMaxPersistMs > MAX_PERSIST_INTERVAL_MS) {
            nvsSaveMax(side);
            s.lastMaxPersistMs = millis();
        }
    }
}

bool suspensionCaptureZero(SuspensionSide side) {
    auto& s = getState(side);

    s.cal.zeroRaw = readRaw(side);
    s.cal.zeroSet = true;
    s.cal.valid   = s.cal.zeroSet && s.cal.maxSet;

    nvsSaveZero(side);
    return true;
}

bool suspensionCaptureMax(SuspensionSide side, float mm) {
    if (mm <= 0.0f) return false;

    auto& s = getState(side);

    s.cal.maxRaw = readRaw(side);
    s.cal.maxSet = true;
    s.cal.valid  = s.cal.zeroSet && s.cal.maxSet;
    s.extremePending = false;

    if (side == FRONT) settings.frontMaxTravelMM = mm;
    else                settings.rearStrokeMM     = mm;
    settingsSave();

    nvsSaveMax(side);
    return true;
}

bool suspensionSetTravelMM(SuspensionSide side, float mm) {
    if (mm <= 0.0f) return false;

    if (side == FRONT) settings.frontMaxTravelMM = mm;
    else                settings.rearStrokeMM     = mm;
    settingsSave();

    return true;
}

void suspensionResetCalibration() {
    front.cal = {};
    rear.cal  = {};
    front.extremePending = false;
    rear.extremePending  = false;

    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();

    Serial.println("Suspension calibration reset");
}

SuspensionCalibration suspensionGetCalibration(SuspensionSide side) {
    return getState(side).cal;
}

uint32_t suspensionSampleCount() {
    return sampleCount;
}

uint32_t suspensionSampleTimeMs() {
    return sampleTimeMs;
}

float suspensionGetRaw(SuspensionSide side) {
    return getState(side).filtered;
}

static float mmFromReading(const SuspensionState& s, float reading, float maxMM) {
    if (!s.cal.valid) return 0.0f;

    float span = s.cal.maxRaw - s.cal.zeroRaw;
    if (fabsf(span) < 1.0f) return 0.0f;

    float t = (reading - s.cal.zeroRaw) / span;

    // Clamp outside the calibrated range instead of extrapolating — we
    // have no data on the sensor's behavior beyond what was actually
    // measured.
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    return t * maxMM;
}

float suspensionGetTravelMM(SuspensionSide side) {
    auto& s = getState(side);
    return mmFromReading(s, s.filtered, suspensionGetMaxTravelMM(side));
}

float suspensionGetInstantTravelMM(SuspensionSide side) {
    auto& s = getState(side);
    return mmFromReading(s, (float)s.currentRaw, suspensionGetMaxTravelMM(side));
}

float suspensionGetMaxTravelMM(SuspensionSide side) {
    return (side == FRONT) ? settings.frontMaxTravelMM : settings.rearStrokeMM;
}

float suspensionGetTravelPct(SuspensionSide side) {
    float maxMM = suspensionGetMaxTravelMM(side);
    if (maxMM <= 0.0f) return 0.0f;

    float pct = suspensionGetTravelMM(side) / maxMM;

    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;

    return pct;
}
